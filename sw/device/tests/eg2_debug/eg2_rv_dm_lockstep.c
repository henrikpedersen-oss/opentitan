// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_lockstep (EG2 verification spec REQ_CDM_08, REQ_CIB_03; "Debug Controller" /
 * "Corner cases"), both modes.
 *
 * The bench (env/seq_lib/chip_eg2_rv_dm_lockstep_vseq.sv) halts, steps and runs abstract commands
 * and program buffers while the core runs the PARK kernel of eg2_dm_corners.h with the lockstep
 * comparator on, and checks that no alert comes (Ibex security.rst "Dual core lockstep": only a
 * mismatch between the cores raises the major alert). Then, with the core halted, it glitches one
 * output of the shadow core: rv_core_ibex's fatal_hw_err must come, ERR_STATUS.FATAL_CORE_ERR must
 * read 1 and the core stops fetching (rv_core_ibex "Local Escalation Path"); an ndmreset ends it.
 *
 * The software's side: the PARK kernel's checks (it ran undisturbed across the halts and steps),
 * and the action ALERT_CHECK, which the bench sends after the fault-free phase and after the
 * ndmreset that follows the glitch: no fatal_hw_err cause in the alert handler and
 * ERR_STATUS.FATAL_CORE_ERR 0 (the ndmreset resets rv_core_ibex and the alert handler: rstmgr
 * "System Reset Tree", all logic but the debug module).
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so the halts in CHERIoT mode run the base debug ROM.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_kinds,
 * model_actions, model_gaps as in eg2_rv_dm_sba_corners.c.
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's halts, steps, abstract commands, | eg2sim has no debug module, JTAG or SBA and  |
 * | the glitch, the ndmreset and the done word   | no lockstep; ALERT_CHECK runs once (arg 0)   |
 *
 * Built with EG2_RV_DM_LOCKSTEP_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `DEBUG FAIL`.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define LOCKSTEP_PARAMS(X)                   \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, 0x200, 0, 0x3fe)            \
  X(model_actions, 0x1, 0, 0x1)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(LOCKSTEP_PARAMS)

enum {
  // Not in eg2_dm_corners.h (a file several tests share): this test's id.
  kEg2DebugIdLockstep = 0x41,
  kActionAlertCheck = kEg2DmAction,
};

static bool alert_check(void) {
  bool ok = true;
  int alert = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (!eg2_chk_true("rv_core_ibex_fatal_hw_err in the alert table", alert >= 0)) {
    return false;
  }
  uint32_t err = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_ERR_STATUS_REG_OFFSET);
  eg2_chk_step("ERR_STATUS", err);
  ok = eg2_chk_eq("ERR_STATUS.FATAL_CORE_ERR (no lockstep mismatch)", 0,
                  (err >> RV_CORE_IBEX_ERR_STATUS_FATAL_CORE_ERR_BIT) & 1u) &&
       ok;
  ok = eg2_chk_true("no rv_core_ibex_fatal_hw_err cause in the alert handler",
                    !eg2_status_wait_alert((size_t)alert, 1)) &&
       ok;
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  (void)arg;
  if (kind == kActionAlertCheck) {
    return alert_check();
  }
  return eg2_chk_true("lockstep action known", false);
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdLockstep,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = EG2_PARAM(model_kinds),
      .model_actions = EG2_PARAM(model_actions),
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(4) && ok;
}
