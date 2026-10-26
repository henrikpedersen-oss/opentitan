// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_sensor_sb (EG2 verification spec REQ_PBF_21, "Post-boot features in CHERIoT
 * mode"): a recoverable or fatal sensor event after secure boot, in RISC-V and CHERIoT mode.
 * Expected behaviour from sensor_ctrl's documentation (hw/top_earlgrey/ip/sensor_ctrl/doc: a
 * recoverable event is registered in RECOV_ALERT, raises recov_alert and is acknowledged; a fatal
 * one (FATAL_ALERT_EN) is registered in FATAL_ALERT, raises fatal_alert and is not acknowledged,
 * "so the alert is sent constantly until the system escalates"; events are aggregated into a
 * wake-up request) and pwrmgr's (WAKE_INFO, the low-power exit through a reset).
 *
 * The event is raised with ALERT_TRIG (the AST event the trigger loops through the AST model).
 * sensor_ctrl's alerts go to class `alert_class` (A-C):
 *
 *   fatal 0 (recoverable). Boot 0: the class counts without escalating. RECOV_ALERT has the event
 *     alone, FATAL_ALERT nothing; the alert handler has recov_alert's cause and the class's
 *     accumulator counted it. Then normal sleep with the trigger still on, and pwrmgr's wake-up
 *     enables: `wake` 1 sensor_ctrl and the aon_timer, `wake` 0 the aon_timer alone (1 ms).
 *     Boot 1: WAKE_INFO is sensor_ctrl alone (wake 1) or the aon_timer alone (wake 0: the event
 *     must not wake the chip when not configured); the event is then acknowledged: RECOV_ALERT
 *     cleared, back to 0.
 *   fatal 1. Boot 0: the class escalates once its accumulator passes kFatalThresh, Phase0 asserting
 *     the escalation reset (no NMI: the CHERIoT start-up code ends the test on any trap); the crash
 *     dump is captured. Boot 1: the reset reason is the escalation alone; the crash dump holds
 *     fatal_alert's cause and the class's accumulator above the threshold: the fatal alert was
 *     sent again and again; FATAL_ALERT is 0 after the reset.
 *
 * Knobs: event 0-10, fatal 0-1, wake 0-1, alert_class 0-2, model_gaps (0 on the chip; eg2sim
 * models sensor_ctrl's STATUS only: the event checks are skipped and named, the sleep with the
 * aon_timer wake-up runs).
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (STATUS.AST_INIT_DONE) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 */

#include "hw/top/sensor_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define SENSOR_SB_PARAMS(X) \
  X(event, 0, 0, 10)        \
  X(fatal, 0, 0, 1)         \
  X(wake, 1, 0, 1)          \
  X(alert_class, 2, 0, 2)   \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(SENSOR_SB_PARAMS)

enum {
  kPolls = 20000,
  kAonFallback = 200,  // the aon_timer wake-up: 1 ms
  kFatalThresh = 3,    // escalate on the fatal alert's 4th arrival
  kRetPhase = 0,
  kRetRecovOk = 1,
};

static void sc_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevSensorCtrl, off, v); }
static uint32_t sc_rd(uint32_t off) { return eg2_dev_read32(kEg2DevSensorCtrl, off); }

static uint32_t alert_id(const char *name) {
  int a = eg2_status_alert_find(name);
  return a < 0 ? 0 : (uint32_t)a;
}

static bool wait_bit(uint32_t off, uint32_t bit) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (sc_rd(off) & bit) {
      return true;
    }
  }
  return false;
}

static bool recoverable_boot_0(eg2_pbf_class_t cls, uint32_t ev) {
  uint32_t recov = alert_id("sensor_ctrl_recov_alert");
  // One escalation signal enabled although it can never fire (threshold 0xffff, no timeout):
  // alert_handler counts alerts in a class only when CTRL.EN and at least one EN_Ex are set
  // (alert_handler_reg_wrap.sv class_en), which its documentation does not say (EN: "Enable
  // escalation mechanisms (accumulation and interrupt timeout)"; rtl_tickets F46). 2026-10-26.
  eg2_pbf_esc_t esc = {.cls = cls, .accum_thresh = 0xffff, .signal_en = 1u << 0, .lock = false};
  eg2_pbf_escalate(&esc, &recov, 1);
  eg2_status_expect_alert(recov);
  eg2_status_expect_intr(kEg2DevAlertHandler, 1u << (uint32_t)cls);
  sc_wr(SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET, 0);
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u << ev);
  bool ok = eg2_pbf_step("RECOV_ALERT registers the event",
                         wait_bit(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u << ev));
  ok = eg2_pbf_check("RECOV_ALERT: the event alone", 1u << ev,
                     sc_rd(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET)) &&
       ok;
  ok = eg2_pbf_check("FATAL_ALERT: none", 0, sc_rd(SENSOR_CTRL_FATAL_ALERT_REG_OFFSET)) && ok;
  ok = eg2_pbf_step("recov_alert reached the alert handler", eg2_status_wait_alert(recov, kPolls)) &&
       ok;
  ok = eg2_pbf_step("the class counted it", eg2_pbf_class_accum(cls) >= 1) && ok;
  eg2_pbf_activity("recoverable sensor events", 1);
  // The alert handler's side is done: clear the class and what the status module expects; the
  // event stays triggered through the sleep.
  eg2_pbf_class_clear(cls);
  eg2_status_clear_expected();
  *eg2_pbf_ret(kRetRecovOk) = ok ? 1 : 0;
  return ok;
}

static bool sleep_with_event(uint32_t ev, bool event_on) {
  uint32_t mask = 1u << kSysWakeAonTimer;
  if (EG2_PARAM(wake) && event_on) {
    mask = 1u << kSysWakeSensorCtrl;
  }
  *eg2_pbf_ret(kRetPhase) = 1;
  if (!eg2_pbf_step("low-power entry armed (CFG_CDC_SYNC)", sys_low_power_arm(mask, false))) {
    return false;
  }
  if (!EG2_PARAM(wake) || !event_on) {
    sys_wkup_after(kAonFallback);
  }
  (void)ev;
  sys_wfi();
  sys_low_power_disarm();
  return eg2_pbf_step("sleep entered (the WFI did not return)", false);
}

static bool recoverable_boot_1(uint32_t ev, bool event_on) {
  uint32_t info = sys_wake_info(true);
  uint32_t want = (EG2_PARAM(wake) && event_on) ? 1u << kSysWakeSensorCtrl
                                                : 1u << kSysWakeAonTimer;
  bool ok = eg2_pbf_step("boot 0's event checks", *eg2_pbf_ret(kRetRecovOk) == 1 || !event_on);
  ok = eg2_pbf_check("WAKE_INFO: the configured source alone", want, info) && ok;
  ok = eg2_pbf_check("reset reason: the low-power exit alone", kSysRstLowPowerExit,
                     eg2_pbf_reset_reason()) &&
       ok;
  sys_wkup_clear();
  // Acknowledge: the trigger off, the latch cleared once the event has gone down.
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0);
  bool cleared = false;
  for (uint32_t i = 0; i < 200 && event_on; ++i) {
    sc_wr(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 1u << ev);
    if (sc_rd(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) == 0) {
      cleared = true;
      break;
    }
    eg2_pbf_spin(50);
  }
  if (event_on) {
    ok = eg2_pbf_step("the event acknowledged (RECOV_ALERT cleared)", cleared) && ok;
    // The always-on event may have reached this boot's alert handler: the test's own cause.
    uint32_t recov = alert_id("sensor_ctrl_recov_alert");
    if (eg2_status_wait_alert(recov, 1)) {
      eg2_status_expect_alert(recov);
      eg2_status_clear_expected();
    }
  }
  eg2_pbf_activity("sensor wake-up checks", ok ? 1 : 0);
  return ok;
}

static bool fatal_boot_0(eg2_pbf_class_t cls, uint32_t ev) {
  uint32_t fatal = alert_id("sensor_ctrl_fatal_alert");
  eg2_pbf_esc_t esc = {
      .cls = cls,
      .accum_thresh = kFatalThresh,
      .phase_cycles = {100, 100, 100, 100},
      .signal_en = 1u << 3,
      .map = {1, 2, 3, 0},  // MAP_E3 = Phase0: the escalation reset at once
      .crashdump_phase = 0,
      .lock = true,
  };
  eg2_pbf_escalate(&esc, &fatal, 1);
  sys_crashdump_enable();
  *eg2_pbf_ret(kRetPhase) = 2;
  sc_wr(SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET, 1u << ev);
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u << ev);
  for (uint32_t i = 0; i < 1000000; ++i) {
    eg2_pbf_spin(10);
  }
  return eg2_pbf_step("the fatal event escalated to a reset", false);
}

static bool fatal_boot_1(eg2_pbf_class_t cls) {
  uint32_t fatal = alert_id("sensor_ctrl_fatal_alert");
  bool ok = eg2_pbf_check("reset reason: the escalation alone", kSysRstEscalation,
                          eg2_pbf_reset_reason());
  uint32_t dump[kSysDumpMaxWords] = {0};
  uint32_t n = sys_crashdump_read(dump, kSysDumpMaxWords);
  eg2_pbf_note("crash dump words", n);
  ok = eg2_pbf_step("crash dump: fatal_alert's cause", sys_dump_cause(dump, fatal)) && ok;
  uint32_t accum = sys_dump_accum(dump, (uint32_t)cls);
  eg2_pbf_note("crash dump: class accumulator", accum);
  ok = eg2_pbf_step("the fatal alert was sent again and again (accumulator > threshold)",
                    accum > kFatalThresh) &&
       ok;
  ok = eg2_pbf_check("FATAL_ALERT after the reset", 0, sc_rd(SENSOR_CTRL_FATAL_ALERT_REG_OFFSET)) &&
       ok;
  eg2_pbf_activity("fatal sensor escalations", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("sensor", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  eg2_pbf_boot_stage();
  uint32_t boot = eg2_pbf_boot_index();
  eg2_pbf_class_t cls = (eg2_pbf_class_t)EG2_PARAM(alert_class);
  uint32_t ev = EG2_PARAM(event);
  bool events = EG2_PARAM(model_gaps) == 0;
  if (boot == 0) {
    bool ok = eg2_pbf_check("STATUS.AST_INIT_DONE", 1,
                            sc_rd(SENSOR_CTRL_STATUS_REG_OFFSET) &
                                (1u << SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT));
    if (!events) {
      eg2_pbf_model_gap("sensor events (ALERT_TRIG, RECOV_ALERT, FATAL_ALERT, the wake-up)");
      return sleep_with_event(ev, false) && ok;
    }
    if (EG2_PARAM(fatal)) {
      return ok && fatal_boot_0(cls, ev);
    }
    ok = recoverable_boot_0(cls, ev) && ok;
    return ok && sleep_with_event(ev, true);
  }
  if (boot == 1 && *eg2_pbf_ret(kRetPhase) == 1) {
    return recoverable_boot_1(ev, events);
  }
  if (boot == 1 && *eg2_pbf_ret(kRetPhase) == 2) {
    return fatal_boot_1(cls);
  }
  return eg2_pbf_step("no further boot expected", false);
}
