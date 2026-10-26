// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_halt_corners (EG2 verification spec REQ_CDM_03, REQ_CDM_08; "Debug Controller" /
 * "Corner cases"), both modes.
 *
 * The bench halts the core at drawn moments of each kernel of eg2_dm_corners.h (a divide, a load
 * waiting on RRAM, a store waiting on the always-on CDC, the two beats of a CLC/CSC, wfi, trap
 * entry, rv_plic's software interrupt arriving with the halt, the watchdog NMI), and in CHERIoT
 * mode also at the start-up code's CHERIOT_LOCK store (after an ndmreset). Its checks are the
 * vseq's (env/seq_lib/chip_eg2_rv_dm_halt_corners_vseq.sv): dcsr.cause, dcsr.prv, dpc and the
 * registers against the core's RVFI. Here the software checks that each kernel ran undisturbed
 * across the halts: its checksum, its exceptions and interrupts counted once each (no interrupt
 * lost or taken twice), CAP's capabilities still tagged.
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so the halts in CHERIoT mode run the base debug ROM (as chip_rv_dm_control_status).
 *
 * Knobs (the vseq writes them, as eg2_debug.h states; also its life-cycle state and timing draws):
 *   late_debug_sw  write LATE_DEBUG_ENABLE first (DEV without OTP's DIS_RV_DM_LATE_DEBUG)
 *   max_iters      a kernel's iteration limit (the bench's stop word ends it first)
 *   idle_polls     polls of the command word before the software gives up
 *   wfi_ticks      WFI: rv_timer ticks between wake-ups
 *   nmi_bark       NMI: the watchdog's bark threshold (aon_timer ticks)
 *   nmi_spin       NMI: polls for the NMI per iteration
 *   model_iters    iterations per kernel in eg2sim
 *   model_kinds    the kernels eg2sim runs (bit per kind)
 *   model_gaps     skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's commands, halts and done word    | eg2sim has no debug module, JTAG or SBA      |
 * | NMI in CHERIoT mode (model_kinds default)    | MPACT-CHERIoT has no NMI                     |
 *
 * Built with EG2_RV_DM_HALT_CORNERS_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

// eg2sim runs every kernel, but NMI in CHERIoT mode: MPACT-CHERIoT has no NMI (a model gap).
#ifdef __CHERIOT__
#define kModelKinds 0x2fe
#else
#define kModelKinds 0x3fe
#endif

#define HALT_PARAMS(X)                       \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x00ffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(wfi_ticks, 20, 2, 100000)                \
  X(nmi_bark, 4, 1, 100000)                  \
  X(nmi_spin, 200000, 100, 0x7fffffff)       \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, kModelKinds, 0, 0x3fe)      \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(HALT_PARAMS)

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdHaltCorners,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = EG2_PARAM(model_kinds),
      .model_actions = 0,
      .wfi_ticks = EG2_PARAM(wfi_ticks),
      .nmi_bark = EG2_PARAM(nmi_bark),
      .nmi_spin = EG2_PARAM(nmi_spin),
      .action = NULL,
  };
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(4) && ok;
}
