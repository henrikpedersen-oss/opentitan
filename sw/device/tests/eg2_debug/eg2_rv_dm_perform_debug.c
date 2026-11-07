// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_rv_dm_perform_debug and chip_rv_dm_perform_debug_cheriot (EG2 verification spec
 * REQ_CDM_08, REQ_PBF_20; "Debug Controller"; the knobs of `chip_rv_dm_perform_debug_cheriot`
 * under "Debug in CHERIoT mode"), both modes.
 *
 * The procedure of OpenTitan's chip_rv_dm_perform_debug testpoint (rom_e2e_jtag_debug_*,
 * chip_sw_rom_e2e_jtag_debug_vseq.sv) run on this image instead of the ROM, so that in CHERIoT
 * mode it runs after the lock: the bench (env/seq_lib/chip_eg2_rv_dm_perform_debug_vseq.sv)
 * halts the core, reads and writes registers and memory through the core (abstract commands and
 * the program buffer), sets every hardware breakpoint and runs into each of them, single-steps
 * (also over `wfi`) and resumes, in a debug-enabled life-cycle state it draws.
 *
 * The software serves the bench's commands (eg2_dm_corners.h): the kernels PARK, WFI and CAP, and
 * one action of its own:
 *   PD_TAGS (kEg2DmAction + 0): stores a 16-byte capability to scratch word 8 (a pointer in RISC-V
 *     mode) in a slot, prints `EG2 DEBUG cap 0x<slot> 0x<base>`, clears scratch words 16-19 and
 *     24-27, runs PARK until the bench's stop word, then checks:
 *       - the slot still holds the capability: address, and in CHERIoT mode tag 1 and length 16
 *         (REQ_CDM_08: debug entry and exit keep every capability; the firmware tag self-check);
 *       - scratch word 16 + i, written by the bench through the core (program buffer `sw`), equals
 *         scratch word 24 + i, written over SBA, xor 0xa5a5a5a5, for i = 0..3, and is not 0: both
 *         memory paths reached the same memory.
 * The CAP kernel's own checks (both slots tagged after the swaps) cover the tags across the
 * halts and steps the bench makes in it.
 *
 * Knobs (the vseq writes them, as eg2_debug.h states; its life-cycle state and timing draws too):
 *   late_debug_sw, max_iters, idle_polls, wfi_ticks, model_iters, model_kinds, model_actions,
 *   model_gaps:
 *
 * | Skipped with model_gaps = 1                          | Why                                  |
 * |------------------------------------------------------|--------------------------------------|
 * | the bench's commands, halts, breakpoints, steps, done | eg2sim has no debug module, JTAG or  |
 * | PD_TAGS's scratch-word check (no bench writes them)  | SBA                                  |
 *
 * Built with EG2_RV_DM_PERFORM_DEBUG_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `DEBUG FAIL`.
 */

#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

enum {
  // This test's id (kEg2DebugMagic + id ends it); local, as the bench's finish() uses it.
  kPerformDebugId = 0x40,
  kActionTags = kEg2DmAction + 0,
  kCapWord = 8,
  kCapLen = 16,
  kCoreWord = 16,
  kSbaWord = 24,
  kWords = 4,
  kSbaXor = 0xa5a5a5a5u,
};

// PARK (9), WFI (5) and CAP (4) run in eg2sim.
#define PD_PARAMS(X)                        \
  X(late_debug_sw, 0, 0, 1)                 \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)  \
  X(idle_polls, 50000000, 1000, 0x7fffffff) \
  X(wfi_ticks, 20, 2, 100000)               \
  X(model_iters, 64, 1, 100000)             \
  X(model_kinds, 0x230, 0, 0x3fe)           \
  X(model_actions, 1, 0, 1)                 \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(PD_PARAMS)

// PD_TAGS's slot (8-byte aligned: a capability in CHERIoT mode).
static void *volatile cap_slot_ __attribute__((aligned(8)));

static uint32_t cap_base(void *p) {
#ifdef __CHERIOT__
  return (uint32_t)__builtin_cheri_base_get(p);
#else
  return (uint32_t)(uintptr_t)p;
#endif
}

static bool action(uint32_t kind, uint32_t arg);

// The configuration of eg2_dm_serve, built where it is used: a pointer to it kept in a global
// would store a stack capability (local) into a global, which CHERIoT refuses.
static void pd_cfg(eg2_dm_cfg_t *cfg) {
  *cfg = (eg2_dm_cfg_t){
      .test_id = kPerformDebugId,
      .late_debug_sw = EG2_PARAM(late_debug_sw),
      .max_iters = EG2_PARAM(max_iters),
      .idle_polls = EG2_PARAM(idle_polls),
      .model_gaps = EG2_PARAM(model_gaps),
      .model_iters = EG2_PARAM(model_iters),
      .model_kinds = EG2_PARAM(model_kinds),
      .model_actions = EG2_PARAM(model_actions),
      .wfi_ticks = EG2_PARAM(wfi_ticks),
      .nmi_bark = 4,
      .nmi_spin = 200000,
      .action = action,
  };
}

static bool action_tags(void) {
  eg2_dm_cfg_t cfg;
  pd_cfg(&cfg);
  volatile uint32_t *s = eg2_dm_scratch();
  for (uint32_t i = 0; i < kWords; ++i) {
    s[kCoreWord + i] = 0;
    s[kSbaWord + i] = 0;
  }
  void *c = (void *)&s[kCapWord];
#ifdef __CHERIOT__
  c = __builtin_cheri_bounds_set(c, kCapLen);
#endif
  cap_slot_ = c;
  LOG_INFO("EG2 DEBUG cap 0x%08x 0x%08x", eg2_chk_addr_of(&cap_slot_), cap_base(c));
  uint32_t max = cfg.model_gaps ? cfg.model_iters : cfg.max_iters;
  bool ok = eg2_dm_run_kernel(&cfg, kEg2DmPark, max);
  void *back = cap_slot_;
  ok = eg2_chk_eq("tags: capability address after the debug session", eg2_chk_addr_of(c),
                  eg2_chk_addr_of(back)) &&
       ok;
#ifdef __CHERIOT__
  ok = eg2_chk_eq("tags: capability tag after the debug session", 1,
                  (uint32_t)__builtin_cheri_tag_get(back)) &&
       ok;
  ok = eg2_chk_eq("tags: capability length after the debug session", kCapLen,
                  (uint32_t)__builtin_cheri_length_get(back)) &&
       ok;
#endif
  if (cfg.model_gaps) {
    eg2_chk_skip("memory written through the core and over SBA (no bench)");
    return ok;
  }
  for (uint32_t i = 0; i < kWords; ++i) {
    uint32_t core = s[kCoreWord + i];
    ok = eg2_chk_true("memory: the word written through the core is set", core != 0) && ok;
    ok = eg2_chk_eq("memory: core-written word = SBA-written word ^ 0xa5a5a5a5",
                    s[kSbaWord + i] ^ kSbaXor, core) &&
         ok;
  }
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  (void)arg;
  if (kind == kActionTags) {
    return action_tags();
  }
  return eg2_chk_true("perform_debug action known", false);
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_dm_cfg_t cfg;
  pd_cfg(&cfg);
  bool ok = eg2_dm_serve(&cfg);
  return eg2_chk_summary(4) && ok;
}
