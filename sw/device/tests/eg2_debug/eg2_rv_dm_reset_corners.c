// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_reset_corners (EG2 verification spec REQ_CDM_04, REQ_PBF_20; "Debug Controller"
 * / "Corner cases"), both modes.
 *
 * The bench resets the system with dmcontrol.ndmreset while the core is halted, while an abstract
 * command runs and while an SBA access is in flight; pulses dmactive low during a session; and
 * asks for a halt on reset (setresethaltreq, then haltreq held across ndmreset). The core runs
 * the PARK kernel of eg2_dm_corners.h meanwhile. The checks are the vseq's
 * (env/seq_lib/chip_eg2_rv_dm_reset_corners_vseq.sv): the debug module's state across ndmreset
 * and dmactive, the first instruction after reset, the reset reason this software prints at each
 * boot (`EG2 DEBUG boot`, rstmgr RESET_INFO). Here the software checks that the loop ran
 * undisturbed across the halts (its count) and took no trap; every ndmreset reboots it.
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so the halts in CHERIoT mode run the base debug ROM.
 *
 * Knobs: late_debug_sw, max_iters, idle_polls, model_iters, model_kinds, model_gaps as in
 * eg2_rv_dm_halt_corners.c (the vseq writes them).
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's commands, halts and done word    | eg2sim has no debug module, JTAG or SBA      |
 *
 * Built with EG2_RV_DM_RESET_CORNERS_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define RESET_PARAMS(X)                        \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, 0x200, 0, 0x3fe)            \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(RESET_PARAMS)

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdResetCorners,
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
      .action = NULL,
  };
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(4) && ok;
}
