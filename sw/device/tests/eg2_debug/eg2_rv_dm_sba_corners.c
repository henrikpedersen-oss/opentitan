// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_sba_corners (EG2 verification spec REQ_CDM_06, REQ_CMS_16; "Debug Controller" /
 * "Corner cases"), both modes; the tag checks in CHERIoT mode.
 *
 * The bench (env/seq_lib/chip_eg2_rv_dm_sba_corners_vseq.sv) drives rv_dm's system bus access
 * into its corner cases and checks sbcs (sberror, sbbusyerror, auto-increment, read-on-data, the
 * access sizes) against the RISC-V Debug Specification 0.13.2. This software gives it the core's
 * view, through the test's actions:
 *   RACE (arg n)   the core writes byte 0 of scratch word 0 (sb) with its iteration count while the
 *                  bench writes byte 2 with 1..n over SBA (8-bit accesses); the core reads byte 2
 *                  after each write. Checks: the values the core saw only increase, byte 2 ends as
 *                  n, byte 0 as the core's last count, bytes 1 and 3 stay 0 (no access writes a
 *                  lane it does not own). Ends on the stop word.
 *   CAP_PREP       stores a capability to scratch word 8 (a pointer in RISC-V mode) into a slot
 *                  and prints `EG2 DEBUG cap 0x<slot> 0x<base>`: the slot's address and the
 *                  capability's base, whose revocation bit the bench sets.
 *   CAP_CHECK (arg) loads the slot (CLC) and checks its tag: 0 expected with arg 1 (the bench
 *                  revoked the base over SBA, or overwrote the slot over SBA: REQ_CMS_16), 1 with
 *                  arg 0. Prints the bitmap word that covers the base as the core reads it
 *                  (`EG2 DEBUG revbm 0x<word>`), which the bench compares with what it wrote.
 *                  RISC-V mode: no tag check.
 *
 * EXPECTED FAILURE in CHERIoT mode: CAP_CHECK after the bench's SBA write over the slot
 * (REQ_CMS_16, ticket F23: SBA bypasses cheriot_mem_sys, so the tag stays set).
 *
 * Knobs (the vseq writes them): late_debug_sw, max_iters, idle_polls, model_iters, model_kinds,
 * model_actions, model_gaps.
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the bench's SBA accesses, commands, done     | eg2sim has no debug module, JTAG or SBA; the |
 * |                                              | actions run with arg 0 (no SBA writer)       |
 *
 * Built with EG2_RV_DM_SBA_CORNERS_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `DEBUG FAIL`.
 */

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define SBA_PARAMS(X)                        \
  X(late_debug_sw, 0, 0, 1)                  \
  X(max_iters, 0x7fffffff, 16, 0x7fffffff)   \
  X(idle_polls, 50000000, 1000, 0x7fffffff)  \
  X(model_iters, 64, 1, 100000)              \
  X(model_kinds, 0x200, 0, 0x3fe)            \
  X(model_actions, 0x7, 0, 0x7)              \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(SBA_PARAMS)

enum {
  kActionRace = kEg2DmAction,
  kActionCapPrep = kEg2DmAction + 1,
  kActionCapCheck = kEg2DmAction + 2,
  kCapWord = 8,
  kRevbmCovered = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
};

// CAP_PREP's slot (8-byte aligned; a capability in CHERIoT mode).
static void *volatile cap_slot_[2] __attribute__((aligned(8)));

static uint32_t cap_base(void *p) {
#ifdef __CHERIOT__
  return (uint32_t)__builtin_cheri_base_get(p);
#else
  return (uint32_t)(uintptr_t)p;
#endif
}

static bool race(uint32_t n_sba) {
  volatile uint8_t *b = (volatile uint8_t *)eg2_dm_scratch();
  volatile uint32_t *stop = eg2_dm_stop();
  uint32_t max = EG2_PARAM(model_gaps) ? EG2_PARAM(model_iters) : EG2_PARAM(max_iters);
  uint32_t i = 0;
  uint32_t last = b[2];
  uint32_t seen = 0;
  bool rising = true;
  while (*stop == 0 && i < max) {
    b[0] = (uint8_t)i;
    uint32_t v = b[2];
    if (v != last) {
      rising = rising && v > last;
      last = v;
      ++seen;
    }
    ++i;
  }
  uint32_t word = eg2_dm_scratch()[0];
  eg2_chk_step("race iterations", i);
  eg2_chk_step("race SBA values seen", seen);
  bool ok = eg2_chk_eq("race: byte 2 is the last SBA value", n_sba, (word >> 16) & 0xffu);
  ok = eg2_chk_eq("race: byte 0 is the core's last count", (i - 1) & 0xffu, word & 0xffu) && ok;
  ok = eg2_chk_eq("race: bytes 1 and 3 untouched", 0, word & 0xff00ff00u) && ok;
  ok = eg2_chk_true("race: the core saw the SBA values in order", rising) && ok;
  return ok;
}

static bool action(uint32_t kind, uint32_t arg) {
  switch (kind) {
    case kActionRace:
      return race(arg);
    case kActionCapPrep: {
      cap_slot_[0] = (void *)&eg2_dm_scratch()[kCapWord];
      LOG_INFO("EG2 DEBUG cap 0x%08x 0x%08x", eg2_chk_addr_of(&cap_slot_[0]),
               cap_base(cap_slot_[0]));
      return eg2_chk_eq("cap slot written", eg2_chk_addr_of(&eg2_dm_scratch()[kCapWord]),
                        eg2_chk_addr_of(cap_slot_[0]));
    }
    case kActionCapCheck: {
      void *c = cap_slot_[0];
      uint32_t base = cap_base(c);
      bool ok = true;
      if (base >= kRevbmCovered) {
        uint32_t off = ((base - kRevbmCovered) >> 8) << 2;
        uint32_t w = eg2_chk_rd32(TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR + off);
        LOG_INFO("EG2 DEBUG revbm 0x%08x 0x%08x", off, w);
      }
#ifdef __CHERIOT__
      ok = eg2_chk_eq("cap slot tag (0: revoked or written over SBA)", arg ? 0u : 1u,
                      (uint32_t)__builtin_cheri_tag_get(c)) &&
           ok;
#else
      eg2_chk_step("cap slot address (RISC-V mode: no tag)", eg2_chk_addr_of(c));
#endif
      return ok;
    }
    default:
      return eg2_chk_true("sba_corners action known", false);
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  const eg2_dm_cfg_t cfg = {
      .test_id = kEg2DebugIdSbaCorners,
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
