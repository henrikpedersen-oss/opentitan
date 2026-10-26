// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_priv (EG2 verification spec REQ_CDM_08; "Debug Controller" / "Corner cases"; the
 * chip form of the core specification's REQ_DBG_04), both modes.
 *
 * Phases (commands of eg2_dm_corners.h's protocol, kind kEg2DmAction, arg = the phase; the bench's
 * part is env/seq_lib/chip_eg2_rv_dm_priv_vseq.sv):
 *   4 (first) The core waits in eg2_dmx_park; the bench reads misa (S never; U in RISC-V mode
 *     only: the core specification's privilege modes) and runs an `ecall` and an illegal
 *     instruction in the program buffer: cmderr 3 each, mcause, mepc and dpc unchanged (RISC-V
 *     Debug Specification 0.13.2 3.12.6, 4.1). Here: no trap reached the software.
 *   0-3 The core waits in eg2_dmx_park_ecall; the bench writes dcsr.prv = the phase and reads it
 *     back: only a level the mode implements (Debug Specification 4.8.1, prv is WARL; CHERIoT mode
 *     M only, RISC-V mode M and U), the written one when implemented. It publishes the value read
 *     back in scratch word 1 and resumes; the wait then ends in an `ecall`, whose cause says the
 *     privilege dret returned to: 8 from U, 11 from M (core REQ_DBG_04).
 * `mret` in the program buffer is not run: neither the Debug Specification nor Ibex's
 * documentation defines it in debug mode, and it would leave the program buffer.
 *
 * EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
 * input, so every halt in CHERIoT mode runs the base debug ROM.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                       | Why                                     |
 * |---------------------------------------------------|-----------------------------------------|
 * | the bench's halts, dcsr writes and done word      | eg2sim has no debug module, JTAG or SBA |
 * | the privilege after dret (eg2sim: no dret, M)     | the ecall is checked as from M          |
 *
 * Built with EG2_RV_DM_PRIV_INJECT_FAIL the first value check expects bit 0 flipped: the run must
 * fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define PRIV_PARAMS(X)                       \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(PRIV_PARAMS)

// Shared with chip_eg2_rv_dm_priv_vseq.sv.
enum {
  kPhProbes = 4,
  kPrvU = 0,
  kPrvM = 3,
  kMcauseEcallU = 8,
  kMcauseEcallM = 11,
  // The bench writes the dcsr.prv it read back here (scratch word 1), with this marker above it.
  kPrvPublished = 0x5052560u,  // "PRV" << 4
};

static bool model_gaps(void) { return EG2_PARAM(model_gaps) != 0; }

static bool phase_prv(uint32_t v) {
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  bool ok = true;
  eg2_chk_step("dcsr.prv written", v);
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  eg2_probe_begin();
#ifndef __CHERIOT__
  // An armed trap from U returns to M (the probe's recovery runs in M).
  eg2_trap_rec.mpp_m = 1;
#endif
  bool trapped = eg2_probe_call(&ctx, NULL, NULL, eg2_dmx_park_ecall);
  uint32_t mcause = eg2_trap_rec.mcause;
  eg2_probe_end();
  ok = eg2_chk_true("the wait ended in its ecall", trapped) && ok;
  uint32_t pub = eg2_dm_scratch()[kEg2DmxScratchArg0];
  uint32_t r = kPrvM;
  if (model_gaps()) {
    eg2_chk_skip("the dcsr.prv the bench read back (no debug module): the ecall is from M");
  } else {
    ok = eg2_chk_eq("the bench published dcsr.prv", kPrvPublished, pub >> 4) && ok;
    r = pub & 3u;
#ifdef __CHERIOT__
    ok = eg2_chk_eq("dcsr.prv read back (CHERIoT mode: M only)", kPrvM, r) && ok;
#else
    ok = eg2_chk_true("dcsr.prv read back is U or M", r == kPrvU || r == kPrvM) && ok;
#endif
  }
  ok = eg2_chk_eq("ecall cause: the privilege dret returned to",
                  r == kPrvU ? kMcauseEcallU : kMcauseEcallM, mcause) &&
       ok;
  return ok;
}

static bool phase_probes(void) {
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  eg2_probe_begin();
  bool trapped = eg2_probe_call(&ctx, NULL, NULL, eg2_dmx_park);
  eg2_probe_end();
  if (model_gaps()) {
    eg2_chk_skip("the program-buffer exceptions and misa (no debug module)");
  }
  return eg2_chk_true("no trap reached the software from the program buffer", !trapped);
}

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction) {
    return eg2_chk_true("action known", false);
  }
  if (!model_gaps()) {
    return arg == kPhProbes ? phase_probes() : arg < 4 ? phase_prv(arg)
                                                       : eg2_chk_true("phase known", false);
  }
  bool ok = phase_probes();
  for (uint32_t v = 0; v < 4; ++v) {
    ok = phase_prv(v) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdPriv,
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
  return eg2_chk_summary(4) && ok;
}
