// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_reg_tags (EG2 verification spec REQ_CDM_08; "Debug Controller" / "Corner cases"),
 * CHERIoT mode only.
 *
 * Phases (each a command of eg2_dm_corners.h's protocol, kind kEg2DmAction, arg = the phase; the
 * bench's part and its checks are env/seq_lib/chip_eg2_rv_dm_reg_tags_vseq.sv):
 *   0 Abstract writes of registers that hold tagged capabilities. The core waits in eg2_dmx_park
 *     with ca0 = the context, ca1 = a data capability over [R, R + 16), ca2 = one over
 *     [R + 0x40, R + 0x60), ca4 = the landing capability (all tagged). The bench writes a drawn
 *     subset of ca1, ca2, ca4 by abstract command (aarsize 2: an integer; aarsize 3: ca0's address
 *     and metadata) and publishes what it wrote in scratch words 1-3. Checked here after resume
 *     (rv_dm theory_of_operation.md "CHERIoT Mode": "a capability written by the debugger is
 *     untagged"): a1 = the value written (else ca1's address); ca2 written: untagged with the
 *     written address, and for aarsize 3 ca0's base and metadata (CGetBase, CGetHigh); ca2 not
 *     written: still the capability passed, tag included.
 *   1, 2 Special capability registers read from the program buffer with and without SR: the core
 *     waits in eg2_dmx_park entered through a PCC with SR (1) or with SR cleared (2); the
 *     software publishes MTCC's and MScratchC's addresses in scratch words 1 and 2 (the probe's
 *     handler and trap record, eg2_probe.h). The bench reads them in the program buffer and by
 *     aarsize 3; it expects no trap in either phase (rv_dm: in debug mode Ibex applies no
 *     permission check; the spec row's "without SR traps" disagrees: see the vseq). Here: the
 *     SR-less PCC is tagged, executable and without SR, and the wait ends without a trap.
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_gaps.
 *
 * | Skipped with model_gaps = 1                            | Why                                 |
 * |--------------------------------------------------------|-------------------------------------|
 * | the bench's halts, abstract commands, program buffers  | eg2sim's debugger is driven by      |
 * | and done word; phase 0's register checks after resume | --stim, which model_tests.sh does   |
 * |                                                        | not pass; its CHERIoT debug mode    |
 * |                                                        | lacks aarsize 3 and the program     |
 * |                                                        | buffer                              |
 *
 * Built with EG2_RV_DM_REG_TAGS_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_cheriot/eg2_cap_model.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define REG_TAGS_PARAMS(X)                   \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(REG_TAGS_PARAMS)

// Shared with chip_eg2_rv_dm_reg_tags_vseq.sv.
enum {
  kEg2DebugIdRegTags = 0x41,
  kPhWrites = 0,
  kPhScrSr = 1,
  kPhScrNoSr = 2,
  // Scratch word 1 after phase 0: bit i = target i written (a1, a2, a4), bit 4 + i = by aarsize 3.
  kWrA1 = 1u << 0,
  kWrA2 = 1u << 1,
  kCapA2 = 1u << 5,
  // The region (sec SRAM's work area, eg2_cheriot_rand.h), reached by address (F24).
  kRegion = kEg2WorkBase,
};

static bool model_gaps(void) { return EG2_PARAM(model_gaps) != 0; }

#ifdef __CHERIOT__

static bool phase_writes(eg2_dmx_ctx_t *ctx) {
  volatile uint32_t *scratch = eg2_dm_scratch();
  void *c1 = eg2_ms_data_cap(kRegion, 16);
  void *c2 = eg2_ms_data_cap(kRegion + 0x40, 32);
  bool ok = eg2_chk_eq("construction: ca1 tagged", 1, eg2_ms_cap_tag(c1));
  ok = eg2_chk_eq("construction: ca2 tagged", 1, eg2_ms_cap_tag(c2)) && ok;
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  bool trapped = eg2_probe_call(ctx, c1, c2, eg2_dmx_park);
  ok = eg2_chk_eq("no trap while the bench wrote registers", 0, trapped ? 1u : 0u) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phase 0: the registers the bench wrote (no debugger stimulus)");
    return ok;
  }
  uint32_t mask = scratch[kEg2DmxScratchArg1];
  uint32_t v1 = scratch[kEg2DmxScratchArg2];
  uint32_t v2 = scratch[kEg2DmxScratchArg3];
  eg2_chk_step("targets written (bit i) and by aarsize 3 (bit 4 + i)", mask);
  uint32_t a1 = eg2_trap_rec.value2;
  void *ca2 = eg2_trap_rec.cap.p;
  if ((mask & kWrA1) != 0) {
    ok = eg2_chk_eq("a1 written by the debugger: its value", v1, a1) && ok;
  } else {
    ok = eg2_chk_eq("a1 not written: ca1's address", eg2_ms_cap_addr(c1), a1) && ok;
  }
  if ((mask & kWrA2) != 0) {
    ok = eg2_chk_eq("ca2 written by the debugger: untagged", 0, eg2_ms_cap_tag(ca2)) && ok;
    ok = eg2_chk_eq("ca2 written by the debugger: the address written", v2,
                    eg2_ms_cap_addr(ca2)) &&
         ok;
    if ((mask & kCapA2) != 0) {
      ok = eg2_chk_eq("ca2 written by aarsize 3: ca0's metadata (CGetHigh)",
                      eg2_ms_cap_high(ctx), eg2_ms_cap_high(ca2)) &&
           ok;
      ok = eg2_chk_eq("ca2 written by aarsize 3: ca0's base", eg2_ms_cap_base(ctx),
                      eg2_ms_cap_base(ca2)) &&
           ok;
    }
  } else {
    ok = eg2_chk_eq("ca2 not written: tag kept", 1, eg2_ms_cap_tag(ca2)) && ok;
    ok = eg2_chk_true("ca2 not written: the capability passed, unchanged",
                      __builtin_cheri_equal_exact(ca2, c2)) &&
         ok;
  }
  return ok;
}

static uint32_t scr_addr(uint32_t which) {
  void *c;
  if (which == 0) {
    __asm__ volatile("cspecialr %0, mtcc" : "=C"(c));
  } else {
    __asm__ volatile("cspecialr %0, mscratchc" : "=C"(c));
  }
  return eg2_ms_cap_addr(c);
}

static bool phase_scr(eg2_dmx_ctx_t *ctx, bool with_sr) {
  volatile uint32_t *scratch = eg2_dm_scratch();
  void *stub = (void *)eg2_dmx_park;
  if (!with_sr) {
    stub = __builtin_cheri_perms_and(stub, ~(uint32_t)kEg2PermSr);
  }
  eg2_chk_step(with_sr ? "SCRs, stage PCC with SR" : "SCRs, stage PCC without SR", 0);
  bool ok = eg2_chk_eq("construction: the stage's PCC tagged", 1, eg2_ms_cap_tag(stub));
  ok = eg2_chk_eq("construction: the stage's PCC SR", with_sr ? 1u : 0u,
                  (__builtin_cheri_perms_get(stub) & kEg2PermSr) != 0 ? 1u : 0u) &&
       ok;
  ok = eg2_chk_eq("construction: the stage's PCC EX", 1,
                  (__builtin_cheri_perms_get(stub) & kEg2PermEx) != 0 ? 1u : 0u) &&
       ok;
  scratch[kEg2DmxScratchArg1] = scr_addr(0);
  scratch[kEg2DmxScratchArg2] = scr_addr(1);
  if (model_gaps()) {
    *eg2_dm_stop() = 1;
  }
  bool trapped = eg2_probe_call(ctx, NULL, NULL, stub);
  ok = eg2_chk_eq("no trap while the bench read the SCRs", 0, trapped ? 1u : 0u) && ok;
  if (model_gaps()) {
    eg2_chk_skip("phases 1-2: the program buffer's SCR reads (no debugger stimulus)");
  }
  return ok;
}

#endif  // __CHERIOT__

static bool run_phase(uint32_t phase) {
  eg2_dmx_ctx_t ctx;
  eg2_dmx_ctx_init(&ctx);
  bool ok = true;
  eg2_chk_step("phase", phase);
  eg2_probe_begin();
  switch (phase) {
#ifdef __CHERIOT__
    case kPhWrites:
      ok = phase_writes(&ctx);
      break;
    case kPhScrSr:
    case kPhScrNoSr:
      ok = phase_scr(&ctx, phase == kPhScrSr);
      break;
#endif
    default:
      ok = eg2_chk_true("phase known in this mode", false);
      break;
  }
  eg2_probe_end();
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  if (kind != kEg2DmAction) {
    return eg2_chk_true("action known", false);
  }
  if (!model_gaps()) {
    return run_phase(arg);
  }
  bool ok = true;
  for (uint32_t p = kPhWrites; p <= kPhScrNoSr; ++p) {
    ok = run_phase(p) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
#ifdef __CHERIOT__
  if (!eg2_sec_sram_init()) {
    return eg2_chk_true("sec SRAM initialised", false);
  }
#endif
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdRegTags,
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
  // eg2sim: 3 + 4 + 4 checks and the done word's; the chip adds phase 0's 2-4.
  return eg2_chk_summary(model_gaps() ? 11 : 13) && ok;
}
