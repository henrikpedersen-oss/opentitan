// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_rv_dm_jtag (EG2 verification spec REQ_PBF_20, "Debug Controller" / "After boot";
 * testpoint chip_rv_dm_jtag of hw/top_earlgrey/data/ip/chip_rv_dm_testplan.hjson), both modes.
 *
 * The JTAG TAP of rv_dm: IDCODE reads the top's RV_DM_JTAG_IDCODE (jtag_id_pkg: version 1,
 * part type 2, TAP type 1, lowRISC's JEDEC id) and a write leaves it unchanged; BYPASS (IR
 * 0x00 and 0x1f) holds 0; DTMCS reads version 1 (RISC-V Debug Specification 0.13.2), dmistat 0
 * and abits of at least 7. Nothing here needs the core's mode, so the CHERIoT run checks the
 * same.
 *
 * The software keeps the core in a known loop and checks it ran undisturbed (eg2_debug.h); the
 * checks of the debug module are the vseq's (env/seq_lib/chip_eg2_rv_dm_*_vseq.sv), made over
 * JTAG with the JTAG agent and the debugger model. The vseq ends the loop by writing
 * 0x600d0001 to `done` over SBA.
 *
 * Knobs (the vseq writes them; it also draws the life-cycle state among the debug-enabled ones,
 * TEST_UNLOCKED0-7, DEV and RMA, and its timing knobs w_when_<t>):
 *   late_debug_sw  1: write rv_dm LATE_DEBUG_ENABLE = MuBi32True first (DEV with OTP's
 *                  DIS_RV_DM_LATE_DEBUG not True)
 *   max_iters      loop iterations before the software gives up on `done`
 *   min_iters      progress the loop must have made before `done`
 *   model_iters    iterations in eg2sim
 *   model_gaps     skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why                                            |
 * |-------------------------------------------|------------------------------------------------|
 * | the done word and the loop's progress     | eg2sim has no debug module, JTAG or SBA        |
 *
 * Built with EG2_RV_DM_JTAG_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_debug/eg2_debug.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define RV_DM_PARAMS(X)                \
  X(late_debug_sw, 0, 0, 1)            \
  X(max_iters, 5000000, 1000, 100000000) \
  X(min_iters, 16, 0, 1000000)         \
  X(model_iters, 5000, 1, 1000000)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(RV_DM_PARAMS)

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_debug_cfg_t cfg = {
      .test_id = kEg2DebugIdJtag,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .min_iters = EG2_PARAM(min_iters),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
  };
  bool ok = eg2_debug_run(&cfg);
  return eg2_chk_summary(2) && ok;
}
