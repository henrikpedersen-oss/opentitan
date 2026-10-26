// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_lc_drop (EG2 verification spec REQ_CDM_05, REQ_SEC_04; "Debug Controller" /
 * "Corner cases"), both modes.
 *
 * Escalation withdraws lc_hw_debug_en during a debug session. The bench (env/seq_lib/
 * chip_eg2_rv_dm_lc_drop_vseq.sv) opens the session (core halted, an SBA access in flight, or an
 * abstract command running) and asks for action ARM: this software classifies rv_core_ibex's
 * fatal_sw_err in alert class A, which cannot be stopped (CTRL.LOCK) and escalates on the first
 * alert after `esc_delay` cycles of phase 0 (no escalation signal), then in phase 1 to lc_ctrl
 * (escalation signals 1 and 2: the ESCALATE state, which withdraws HW_DEBUG_EN and CPU_EN), then
 * after `esc_hold` cycles of phase 2 to the escalation reset (signal 3, phase 3). It raises the
 * alert through ALERT_TEST, prints `EG2 DEBUG armed 0x<esc_delay>` and parks; the escalation stops
 * the core (lc_ctrl's CPU_EN), so the action never returns on the chip. No NMI (signal 0 off).
 * The bench checks the drop: no JTAG, DMI or SBA access afterwards and no instruction retired.
 * Boot 2 (the escalation reset) prints rstmgr's reset reason (`EG2 DEBUG boot`) and serves again;
 * the bench checks the reason and ends the test.
 *
 * Late debug enable (DEV with OTP's DIS_RV_DM_LATE_DEBUG not MuBi8True, `late_delayed` = 1): the
 * software first prints `EG2 DEBUG late pending`, waits `late_polls`, writes LATE_DEBUG_ENABLE =
 * MuBi32True and prints `EG2 DEBUG late enabled`; the bench checks the debug module was gated
 * before (rv_dm "Late Debug Enable Mechanism") and works after.
 *
 * EXPECTED FAILURE in CHERIoT mode for the halted session until ticket F13 (status/rtl_tickets.md):
 * rv_dm has no CHERIoT input. The SBA and late-enable sessions do not halt the core.
 *
 * Knobs (the vseq writes them): late_debug_sw, late_delayed, late_polls, esc_delay, esc_hold,
 * max_iters, idle_polls, model_iters, model_kinds, model_gaps.
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's commands, ARM and the done word  | eg2sim has no debug module, JTAG or SBA; ARM |
 * |                                              | escalates and resets the system              |
 *
 * Built with EG2_RV_DM_LC_DROP_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `DEBUG FAIL`.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/rv_dm_regs.h"          // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define LC_DROP_PARAMS(X)                    \
  X(late_debug_sw, 0, 0, 1)                  \
  X(late_delayed, 0, 0, 1)                   \
  X(late_polls, 20000, 0, 10000000)          \
  X(esc_delay, 2400, 1, 1000000)             \
  X(esc_hold, 48000, 100, 10000000)          \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, 0x200, 0, 0x3fe)            \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(LC_DROP_PARAMS)

enum {
  kActionArm = kEg2DmAction,
};

static eg2_dm_cfg_t cfg_;

static void ah_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

/** Class A: the alert escalates to lc_ctrl after esc_delay, to the reset esc_hold later. */
static void configure_class_a(uint32_t alert) {
  ah_write(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * alert,
           ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  ah_write(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * alert, 1);
  ah_write(ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET, EG2_PARAM(esc_delay));
  ah_write(ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET, 100);
  ah_write(ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET, EG2_PARAM(esc_hold));
  ah_write(ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET, 100);
  ah_write(ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
           (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_LOCK_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E1_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E2_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E3_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E1_OFFSET) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E2_OFFSET) |
               (3u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E3_OFFSET));
}

/** ARM: the escalation armed and started; the core parks until lc_ctrl stops it. */
static bool action(uint32_t kind, uint32_t arg) {
  (void)arg;
  if (kind != kActionArm) {
    return eg2_chk_true("lc_drop action known", false);
  }
  int alert = eg2_status_alert_find("rv_core_ibex_fatal_sw_err");
  if (!eg2_chk_true("rv_core_ibex_fatal_sw_err found", alert >= 0)) {
    return false;
  }
  configure_class_a((uint32_t)alert);
  eg2_status_raise_alert((size_t)alert);
  LOG_INFO("EG2 DEBUG armed 0x%08x", EG2_PARAM(esc_delay));
  bool ok = eg2_dm_run_kernel(&cfg_, kEg2DmPark, cfg_.max_iters);
  // Reached only if the bench stopped the kernel: the escalation did not stop the core.
  return eg2_chk_true("the escalation stopped the core (lc_ctrl CPU_EN)", false) && ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool ok = true;
  if (EG2_PARAM(late_delayed)) {
    LOG_INFO("EG2 DEBUG late pending");
    eg2_chk_delay(EG2_PARAM(late_polls));
    eg2_dev_write32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, kMultiBitBool32True);
    ok = eg2_chk_eq("late debug enable (delayed)", kMultiBitBool32True,
                    eg2_dev_read32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET)) &&
         ok;
    LOG_INFO("EG2 DEBUG late enabled");
  }
  cfg_ = (eg2_dm_cfg_t){
      .test_id = kEg2DebugIdLcDrop,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = EG2_PARAM(model_kinds),
      .model_actions = 0,
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  if (cfg_.model_gaps) {
    eg2_chk_skip("ARM: the escalation to lc_ctrl and the escalation reset");
  }
  ok = eg2_dm_serve(&cfg_) && ok;
  return eg2_chk_summary(4) && ok;
}
