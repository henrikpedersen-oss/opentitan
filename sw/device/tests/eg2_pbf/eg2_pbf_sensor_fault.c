// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_sensor_fault (EG2 verification spec REQ_PBF_21, "Post-boot faults"): a sensor
 * event while another is being acknowledged, and a corrupted sensor configuration. Check: the
 * alert is raised again, no event is lost, the corruption has no effect.
 *
 * Expected behaviour from sensor_ctrl's documentation (hw/top_earlgrey/ip/sensor_ctrl/doc:
 * RECOV_ALERT registers every recoverable event and is cleared by writing 1, a new event raises
 * recov_alert again; CFG_REGWEN, once cleared, locks FATAL_ALERT_EN until reset).
 *
 *   fault 0, a second event during the acknowledgement of event `event`: the acknowledgement is
 *     read RECOV_ALERT, write its bits back (rw1c), then turn the trigger off. The second event
 *     (`event2`: another event, or the same one again with `same` 1) is triggered at
 *     `fault_point`: before the read (0), between the read and the write (1), after the write
 *     (2). Check: RECOV_ALERT afterwards still has the second event's bit unless its own
 *     acknowledgement cleared it (fault_point 0 with a different event is read with the first and
 *     cleared with it: then both were registered); recov_alert reached the alert handler at least
 *     twice (the class's accumulator); finally both are acknowledged and RECOV_ALERT reads 0.
 *   fault 1, a corrupted configuration: FATAL_ALERT_EN set recoverable for the event, CFG_REGWEN
 *     cleared, then FATAL_ALERT_EN written with the event fatal (the corruption). Check: the write
 *     had no effect (CFG_REGWEN 0, FATAL_ALERT_EN unchanged), and the event then triggered is
 *     handled as recoverable (RECOV_ALERT, not FATAL_ALERT).
 *
 * Knobs: fault 0-1, fault_point 0-2, event 0-10, event2 0-10, same 0-1, alert_class 0-2,
 * model_gaps (0 on the chip; eg2sim models sensor_ctrl's STATUS only).
 * Gap: the spec row's "integrity error" (a TL-UL integrity error on sensor_ctrl's configuration
 * write) needs the bench's integrity injection; REQ_TL_04's chip_sw_eg2_tl_intg_links covers it.
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (STATUS.AST_INIT_DONE) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 */

#include "hw/top/sensor_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define SENSOR_FAULT_PARAMS(X) \
  X(fault, 0, 0, 1)            \
  X(fault_point, 1, 0, 2)      \
  X(event, 0, 0, 10)           \
  X(event2, 1, 0, 10)          \
  X(same, 0, 0, 1)             \
  X(alert_class, 1, 0, 2)      \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(SENSOR_FAULT_PARAMS)

enum {
  kPolls = 20000,
};

static void sc_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevSensorCtrl, off, v); }
static uint32_t sc_rd(uint32_t off) { return eg2_dev_read32(kEg2DevSensorCtrl, off); }

static bool wait_bits(uint32_t off, uint32_t bits) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if ((sc_rd(off) & bits) == bits) {
      return true;
    }
  }
  return false;
}

/** The triggers off, then RECOV_ALERT cleared once the events have gone down. */
static bool acknowledge_all(void) {
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0);
  for (uint32_t i = 0; i < 200; ++i) {
    sc_wr(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 0x7ffu);
    if (sc_rd(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) == 0) {
      return true;
    }
    eg2_pbf_spin(50);
  }
  return false;
}

static bool second_event(eg2_pbf_class_t cls) {
  uint32_t e1 = EG2_PARAM(event);
  uint32_t e2 = EG2_PARAM(same) ? e1 : EG2_PARAM(event2);
  if (e2 == e1 && !EG2_PARAM(same)) {
    e2 = (e1 + 1) % 11;
  }
  uint32_t b1 = 1u << e1, b2 = 1u << e2;
  uint32_t point = EG2_PARAM(fault_point);
  eg2_pbf_note("first event", e1);
  eg2_pbf_note("second event", e2);
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, b1);
  bool ok = eg2_pbf_step("the first event registered", wait_bits(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET,
                                                                 b1));
  // The first event's trigger goes off with the acknowledgement's start, so only the second can
  // set a bit again.
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0);
  eg2_pbf_spin(200);
  if (point == 0) {
    sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, b2);
    (void)wait_bits(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, b2);
  }
  uint32_t snap = sc_rd(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  if (point == 1) {
    sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, b2);
    (void)wait_bits(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, b2);
  }
  sc_wr(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, snap);
  if (point == 2) {
    sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, b2);
    (void)wait_bits(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, b2);
  }
  eg2_pbf_spin(200);
  uint32_t after = sc_rd(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET);
  eg2_pbf_note("RECOV_ALERT read at the acknowledgement", snap);
  eg2_pbf_note("RECOV_ALERT after it", after);
  if (point == 0) {
    // Both events were registered before the read, and the read-back write cleared both; the
    // second's trigger is still on, so it is registered again.
    ok = eg2_pbf_step("both events registered before the acknowledgement",
                      (snap & (b1 | b2)) == (b1 | b2)) &&
         ok;
  }
  if (point != 0) {
    // No event lost: the second event came after the read, so writing back the read value must
    // not clear it (and with `same` 1 the event raised again is registered again).
    ok = eg2_pbf_step("the second event is registered after the acknowledgement",
                      (after & b2) != 0) &&
         ok;
  }
  if (e2 != e1) {
    ok = eg2_pbf_step("the first event is acknowledged", (after & b1) == 0 || point == 0) && ok;
  }
  ok = eg2_pbf_step("recov_alert raised again (class accumulator >= 2)",
                    eg2_pbf_class_accum(cls) >= 2) &&
       ok;
  eg2_pbf_note("class accumulator", eg2_pbf_class_accum(cls));
  ok = eg2_pbf_step("every event acknowledged (RECOV_ALERT 0)", acknowledge_all()) && ok;
  eg2_pbf_activity("sensor events during an acknowledgement", 1);
  return ok;
}

static bool corrupted_config(void) {
  uint32_t ev = EG2_PARAM(event), b = 1u << ev;
  sc_wr(SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET, 0);
  sc_wr(SENSOR_CTRL_CFG_REGWEN_REG_OFFSET, 0);
  bool ok = eg2_pbf_check("CFG_REGWEN after the lock", 0,
                          sc_rd(SENSOR_CTRL_CFG_REGWEN_REG_OFFSET) & 1u);
  sc_wr(SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET, b);  // the corruption
  ok = eg2_pbf_check("FATAL_ALERT_EN unchanged by the corruption", 0,
                     sc_rd(SENSOR_CTRL_FATAL_ALERT_EN_REG_OFFSET)) &&
       ok;
  sc_wr(SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, b);
  ok = eg2_pbf_step("the event handled as recoverable (RECOV_ALERT)",
                    wait_bits(SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, b)) &&
       ok;
  ok = eg2_pbf_check("FATAL_ALERT: none", 0, sc_rd(SENSOR_CTRL_FATAL_ALERT_REG_OFFSET)) && ok;
  ok = eg2_pbf_step("the event acknowledged (RECOV_ALERT 0)", acknowledge_all()) && ok;
  eg2_pbf_activity("corrupted sensor configurations refused", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("sensor_fault", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  bool ok = eg2_pbf_check("STATUS.AST_INIT_DONE", 1,
                          sc_rd(SENSOR_CTRL_STATUS_REG_OFFSET) &
                              (1u << SENSOR_CTRL_STATUS_AST_INIT_DONE_BIT));
  eg2_pbf_gap("a TL-UL integrity error on sensor_ctrl's configuration",
              "needs the bench's integrity injection (REQ_TL_04, chip_sw_eg2_tl_intg_links)");
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("sensor events and CFG_REGWEN (eg2sim models sensor_ctrl's STATUS only)");
    return ok;
  }
  eg2_pbf_class_t cls = (eg2_pbf_class_t)EG2_PARAM(alert_class);
  int recov = eg2_status_alert_find("sensor_ctrl_recov_alert");
  if (!eg2_pbf_step("sensor_ctrl_recov_alert found", recov >= 0)) {
    return false;
  }
  uint32_t a = (uint32_t)recov;
  // One escalation signal enabled although it can never fire (threshold 0xffff, no timeout):
  // alert_handler counts alerts in a class only when CTRL.EN and at least one EN_Ex are set
  // (alert_handler_reg_wrap.sv class_en), which its documentation does not say (EN: "Enable
  // escalation mechanisms (accumulation and interrupt timeout)"; rtl_tickets F46). 2026-10-26.
  eg2_pbf_esc_t esc = {.cls = cls, .accum_thresh = 0xffff, .signal_en = 1u << 0, .lock = false};
  eg2_pbf_escalate(&esc, &a, 1);
  eg2_status_expect_alert(a);
  eg2_status_expect_intr(kEg2DevAlertHandler, 1u << (uint32_t)cls);
  ok = (EG2_PARAM(fault) == 0 ? second_event(cls) : corrupted_config()) && ok;
  // The alert handler's side: the class cleared, the expected cause and interrupt cleared.
  // (regr-vlt 2026-10-26, "unexpected interrupt alert_handler INTR_STATE = 0x2": nothing cleared
  // INTR_STATE.CLASSx, because eg2_status_clear_expected skipped the alert handler's declared
  // interrupt; fixed in eg2_status.c, so one clear does it. A late alert would now show as an
  // unexpected alert cause in the sweep, not be hidden by repeated clears.)
  eg2_pbf_class_clear(cls);
  eg2_status_clear_expected();
  return ok;
}
