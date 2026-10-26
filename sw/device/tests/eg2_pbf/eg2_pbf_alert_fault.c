// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_alert_fault (EG2 verification spec REQ_PBF_17, "Post-boot faults"): software
 * tries to stop an escalation in progress, and the escalation continues. Expected behaviour from
 * the alert handler's documentation (hw/top_earlgrey/ip_autogen/alert_handler/doc: CLASSx_CTRL.LOCK
 * "there is no way to stop the escalation protocol for class x once it has been triggered", which
 * clears CLASSx_CLR_REGWEN; CLASSx_REGWEN and ALERT_REGWEN_n lock the configuration).
 *
 * Boot 0: one alert, drawn with the seed, goes to class `alert_class` (A-C), which escalates on its
 * first alert through Phase0..Phase3, CTRL.LOCK set, escalation signal 3 (the escalation reset)
 * in Phase3 and signals 0-2 off (no NMI: the CHERIoT start-up code ends the test on any trap). The
 * test locks the class's configuration (CLASSx_REGWEN, ALERT_REGWEN_n written 0), as the
 * programmer's guide asks, raises the alert and waits for the fault point: the escalation's start
 * Phase0 (0), its middle Phase1 (1), its last step software sees, Phase2 (2: Phase3 asserts the
 * reset as it begins). There it tries the stop `fault` names:
 *   0 CLASSx_CLR written (the clear);
 *   1 CLASSx_CTRL written with EN 0;
 *   2 the alert's ALERT_EN_n written 0;
 *   3 the remaining phases' PHASEx_CYC written with the maximum.
 * The state, CLR_REGWEN and the class's escalation count after the attempt are kept in retention
 * SRAM; the escalation reset must end the boot.
 * Boot 1: the reset reason is the escalation alone; at the attempt CLR_REGWEN read 0 and the state
 * did not go back to Idle; the crash dump (latched on entering Phase3) holds the class in Phase3,
 * so the escalation went on after the attempt, with accumulator 1 and the alert's cause.
 *
 * Knobs: alert_class 0-2, fault_point 0-2, fault 0-3, model_gaps (0 on the chip).
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (the reset reason) expects bit 0 flipped:
 * the run must fail with `PBF FAIL`.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define ALERT_FAULT_PARAMS(X) \
  X(alert_class, 1, 0, 2)     \
  X(fault_point, 1, 0, 2)     \
  X(fault, 0, 0, 3)           \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ALERT_FAULT_PARAMS)

enum {
  kPhaseCycles = 24000,  // io_div4 cycles per phase (1 ms)
  kPolls = 2000000,
  kStateIdle = 0,
  kStatePhase0 = 4,
  kStatePhase3 = 7,
  kClassStride =
      ALERT_HANDLER_CLASSB_REGWEN_REG_OFFSET - ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET,
  // Retention words.
  kRetAlert = 0,
  kRetAttempted = 1,
  kRetStateAt = 2,
  kRetClrRegwen = 3,
  kRetStateAfter = 4,
  kRetBackToIdle = 5,
};

static uint32_t class_reg(eg2_pbf_class_t cls, uint32_t class_a_offset) {
  return class_a_offset + (uint32_t)cls * kClassStride;
}

static void ah_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

/** The attempt to stop the escalation. */
static void try_to_stop(eg2_pbf_class_t cls, uint32_t alert) {
  switch (EG2_PARAM(fault)) {
    case 0:
      eg2_pbf_class_clear(cls);
      break;
    case 1:
      ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET), 0);
      break;
    case 2:
      ah_write(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * alert, 0);
      break;
    default:
      ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET), 0xffffffffu);
      ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET), 0xffffffffu);
      ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET), 0xffffffffu);
      ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET), 0xffffffffu);
      break;
  }
}

static bool boot_0(eg2_pbf_class_t cls, uint32_t alert) {
  eg2_pbf_esc_t esc = {
      .cls = cls,
      .accum_thresh = 0,
      .phase_cycles = {kPhaseCycles, kPhaseCycles, kPhaseCycles, kPhaseCycles},
      .signal_en = 1u << 3,
      // MAP_Ex: the phase that asserts escalation signal x.
      .map = {0, 1, 2, 3},
      .crashdump_phase = 3,
      .lock = true,
  };
  uint32_t a = alert;
  eg2_pbf_escalate(&esc, &a, 1);
  // Lock the configuration (alert handler programmer's guide).
  eg2_dev_write32(kEg2DevAlertHandler, class_reg(cls, ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET), 0);
  eg2_dev_write32(kEg2DevAlertHandler, ALERT_HANDLER_ALERT_REGWEN_0_REG_OFFSET + 4 * alert, 0);
  sys_crashdump_enable();
  *eg2_pbf_ret(kRetAlert) = alert;
  // Phase3 asserts the escalation reset as it starts, so Phase2 is the last step software sees.
  uint32_t at = kStatePhase0 + EG2_PARAM(fault_point);
  eg2_status_expect_alert(alert);
  eg2_puts("PBF note alert: raising ");
  eg2_puts(eg2_status_alert_name(alert));
  eg2_puts(", the stop attempt in state ");
  eg2_putdec(at);
  eg2_puts("\r\n");
  eg2_status_raise_alert(alert);

  // No printing from here on: the phases are 1 ms long.
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t s = eg2_pbf_class_state(cls);
    if (s >= at) {
      *eg2_pbf_ret(kRetStateAt) = s;
      try_to_stop(cls, alert);
      *eg2_pbf_ret(kRetAttempted) = 1;
      *eg2_pbf_ret(kRetClrRegwen) = sys_class_clr_regwen(cls);
      break;
    }
  }
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t s = eg2_pbf_class_state(cls);
    *eg2_pbf_ret(kRetStateAfter) = s;
    if (s == kStateIdle) {
      *eg2_pbf_ret(kRetBackToIdle) = 1;
    }
  }
  return eg2_pbf_step("escalation reset came", false);
}

static bool boot_1(eg2_pbf_class_t cls) {
  uint32_t alert = *eg2_pbf_ret(kRetAlert);
  bool ok = eg2_pbf_check("reset reason: the escalation alone", kSysRstEscalation,
                          eg2_pbf_reset_reason());
  ok = eg2_pbf_step("the stop was attempted", *eg2_pbf_ret(kRetAttempted) == 1) && ok;
  eg2_pbf_note("state at the attempt", *eg2_pbf_ret(kRetStateAt));
  eg2_pbf_note("state last seen", *eg2_pbf_ret(kRetStateAfter));
  ok = eg2_pbf_check("CLR_REGWEN of a locked class in escalation", 0,
                     *eg2_pbf_ret(kRetClrRegwen)) &&
       ok;
  ok = eg2_pbf_step("the class never went back to Idle", *eg2_pbf_ret(kRetBackToIdle) == 0) && ok;
  uint32_t dump[kSysDumpMaxWords] = {0};
  uint32_t n = sys_crashdump_read(dump, kSysDumpMaxWords);
  eg2_pbf_note("crash dump words", n);
  if (EG2_PARAM(model_gaps) && n == 0) {
    eg2_pbf_model_gap("crash dump");
  } else {
    ok = eg2_pbf_check("crash dump: the class reached Phase3", kStatePhase3,
                       sys_dump_state(dump, (uint32_t)cls)) &&
         ok;
    ok = eg2_pbf_check("crash dump: class accumulator", 1, sys_dump_accum(dump, (uint32_t)cls)) &&
         ok;
    ok = eg2_pbf_step("crash dump: the alert's cause", sys_dump_cause(dump, alert)) && ok;
  }
  eg2_pbf_activity("escalations past a stop attempt", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("alert_fault", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  uint32_t boot = eg2_pbf_boot_index();
  eg2_pbf_class_t cls = (eg2_pbf_class_t)EG2_PARAM(alert_class);
  if (boot == 0) {
    return boot_0(cls, eg2_rand_range(0, kEg2AlertCount - 1));
  }
  if (boot == 1) {
    return boot_1(cls);
  }
  return eg2_pbf_step("no further boot expected", false);
}
