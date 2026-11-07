// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_dm_cheriot_mode_switch (EG2 verification spec REQ_CDM_04; "Debug Controller", "Debug
 * in CHERIoT mode"). The `_cheriot` build is the spec's test (RISC-V, then CHERIoT: the start-up
 * code's CHERIoT lock switches the core while it runs); the RISC-V build is its control (no lock,
 * base mode throughout).
 *
 * The bench (env/seq_lib/chip_sw_rv_dm_cheriot_mode_switch_vseq.sv) debugs the core in drawn
 * phases: halted in this software's wait loop after the lock, halted at the first instruction
 * after an ndmreset (before the lock), and halted in the wait loop again after an ndmreset; each
 * ndmreset reboots this software, which locks again. In each phase it checks that the core's and
 * the debug module's mode are the phase's and that the debug operations give that mode's results
 * (the vseq's header lists them).
 *
 * Here: each phase is the action kEg2DmAction with arg 0 (eg2_dm_corners.h protocol). The software
 * publishes its build's mode in scratch word 1 (1: CHERIoT, the lock done by the start-up code;
 * 0: RISC-V) and waits in eg2_dmx_park (eg2_dm_cap.h) for the bench; afterwards it checks that the
 * wait ended without a trap and that the published word is still there.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                        | Why                                     |
 * |----------------------------------------------------|-----------------------------------------|
 * | the bench's halts, ndmresets, operations and done  | eg2sim's debugger is driven by --stim,  |
 * |                                                    | which model_tests.sh does not pass; its |
 * |                                                    | CHERIoT debug mode lacks aarsize 3, the |
 * |                                                    | program buffer and dcsr.step            |
 *
 * Built with EG2_RV_DM_CHERIOT_MODE_SWITCH_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define MODE_SWITCH_PARAMS(X)                \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(MODE_SWITCH_PARAMS)

enum {
  /** The test's id (kEg2DebugMagic + id ends it); shared with the vseq (finish(0x40)). */
  kEg2DebugIdModeSwitch = 0x40,
#ifdef __CHERIOT__
  kBuildMode = 1,
#else
  kBuildMode = 0,
#endif
};

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction || arg != 0) {
    return eg2_chk_true("action known", false);
  }
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  volatile uint32_t *scratch = eg2_dm_scratch();
  scratch[kEg2DmxScratchArg0] = kBuildMode;
  if (EG2_PARAM(model_gaps) != 0) {
    *eg2_dm_stop() = 1;
  }
  eg2_probe_begin();
  bool trapped = eg2_probe_call(&ctx, NULL, NULL, eg2_dmx_park);
  eg2_probe_end();
  bool ok = eg2_chk_eq("the wait for the debugger ended without a trap", 0, trapped ? 1u : 0u);
  ok = eg2_chk_eq("the build's mode word kept across the halts", kBuildMode,
                  scratch[kEg2DmxScratchArg0]) &&
       ok;
  ok = eg2_chk_true("the stage published its wait loop", scratch[kEg2DmxScratchFlag] != 0) && ok;
  if (EG2_PARAM(model_gaps) != 0) {
    eg2_chk_skip("the bench's halts, ndmresets and debug operations (no debugger stimulus)");
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdModeSwitch,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = 0,
      .model_actions = 1,
      .wfi_ticks = 20,
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
  bool ok = eg2_dm_serve(&cfg);
  // Per boot: 3 checks per phase, and the done word (1).
  return eg2_chk_summary(3) && ok;
}
