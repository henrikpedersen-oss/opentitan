// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revoke_store_clear (EG2 spec "Revocation", "Corner cases"; REQ_CMS_10,
 * REQ_CMS_19; ports the core bench's `cheriot_revoke_basic`).
 *
 * An integer store to a tagged capability clears its tag (REQ_CMS_10: a capability is two 32-bit
 * accesses with the tag beside them; RFC CHERIoT Memory Subsystem, the tag filter: a write without
 * the tag clears the granule's tag; cheriot_mem_sys theory_of_operation.md "Tag Filter"). Each case
 * stores two tagged capabilities (CSC) to a slot and to its neighbouring granule (below or above),
 * in main SRAM (the test's main arena) or sec SRAM (arena A), each pointing at an object in the
 * work area, then one byte (`sb`), halfword (`sh`) or word (`sw`) integer store to either word of
 * the slot, at any aligned offset in that word, through an expected-trap probe. The first 12 cases
 * cover every memory x width x word; the rest are drawn.
 *
 * Checks, all by reloading:
 *  - the store did not trap, and both words of the slot hold what the store merged in;
 *  - CLC of the slot returns tag 0;
 *  - CLC of the neighbour returns tag 1 (the clear is per granule);
 *  - a CLC of the slot and an `lw` through the loaded capability as the next instruction
 *    (eg2_rv_stub_clc_lw) traps: CHERI exception (mcause 28), tag violation (capcause 0x02) on
 *    ca2 (register 12): mtval = 12 << 5 | 0x02;
 *  - the same through the neighbour does not trap and reads the object's first word.
 * Nothing is marked in the revocation bitmap (cleared at the start), so the load filter returns
 * every tag as stored.
 *
 * Built with EG2_REVOKE_STORE_CLEAR_INJECT_FAIL, the first case's slot is expected to keep its tag:
 * the run must fail with `MISMATCH tag after the store #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVOKE_STORE_CLEAR_PARAMS(X) \
  X(n_cases, 0, 0, 256)              \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOKE_STORE_CLEAR_PARAMS);

// eg2_revoke.S: CLC from ca0, then lw a2, 0(ca2).
extern char eg2_rv_stub_clc_lw[];

enum {
  // The objects the capabilities point at: 16 granules at the start of the work area.
  kObjects = kEg2WorkBase,
  kNumObjects = 16,
  kCapReg = 12,  // ca2, the register eg2_rv_stub_clc_lw loads into
};

static eg2_tally_t tally;

static uint32_t obj_addr(uint32_t i) { return kObjects + 8 * (i % kNumObjects); }
static uint32_t obj_word(uint32_t a) { return a ^ 0x3c3c5a5au; }

/** The word `old` with the `width`-byte store of `value` at byte `off` merged in. */
static uint32_t merge(uint32_t old, uint32_t value, uint32_t width, uint32_t off) {
  uint32_t mask = width == 4 ? 0xffffffffu : ((1u << (8 * width)) - 1) << (8 * off);
  return (old & ~mask) | ((value << (8 * off)) & mask);
}

static void one_case(uint32_t i, uint32_t main_arena, bool in_main, uint32_t width,
                     uint32_t word) {
  uint32_t base = in_main ? main_arena : kEg2ArenaA;
  uint32_t granules = (in_main ? kEg2MainArenaSize : kEg2ArenaSize) / 8;
  // A slot with a granule on both sides inside the arena.
  uint32_t slot = base + 8 * eg2_rand_range(1, granules - 2);
  uint32_t nb = eg2_pick(2) == 0 ? slot - 8 : slot + 8;
  uint32_t obj_s = obj_addr(eg2_pick(kNumObjects)), obj_n = obj_addr(eg2_pick(kNumObjects));
  ++tally.planned;

  eg2_ms_csc(slot, eg2_ms_object_cap(obj_s, 8, false));
  eg2_ms_csc(nb, eg2_ms_object_cap(obj_n, 8, false));
  if (!eg2_check(&tally, "slot tagged before the store", i, eg2_ms_tag(slot), 1) ||
      !eg2_check(&tally, "neighbour tagged before the store", i, eg2_ms_tag(nb), 1)) {
    return;
  }
  uint32_t w0 = eg2_ms_read32(slot), w1 = eg2_ms_read32(slot + 4);

  // The integer store, through an authority over exactly the bytes it writes.
  uint32_t off = width == 4 ? 0 : width * eg2_pick(4 / width);
  uint32_t value = eg2_rand();
  uint32_t at = slot + 4 * word + off;
  void *stub = width == 1 ? eg2_stub_sb : width == 2 ? eg2_stub_sh : eg2_stub_sw;
  bool trapped = eg2_probe_call(eg2_mem(at, width), (void *)(uintptr_t)value, NULL, stub);
  eg2_check(&tally, "integer store trapped", i, trapped, 0);
  if (word == 0) {
    w0 = merge(w0, value, width, off);
  } else {
    w1 = merge(w1, value, width, off);
  }
  eg2_check(&tally, "slot word 0 after the store", i, eg2_ms_read32(slot), w0);
  eg2_check(&tally, "slot word 1 after the store", i, eg2_ms_read32(slot + 4), w1);

  uint32_t exp_tag = 0;
#ifdef EG2_REVOKE_STORE_CLEAR_INJECT_FAIL
  if (i == 0) {
    exp_tag = 1;
  }
#endif
  eg2_check(&tally, "tag after the store", i, eg2_ms_tag(slot), exp_tag);
  eg2_check(&tally, "neighbour's tag after the store", i, eg2_ms_tag(nb), 1);

  // The pointer reloaded and used: a tag violation on ca2.
  trapped = eg2_probe_call(eg2_ms_auth(slot), NULL, NULL, eg2_rv_stub_clc_lw);
  if (eg2_check(&tally, "lw through the reloaded slot trapped", i, trapped, 1)) {
    eg2_check(&tally, "lw through the reloaded slot mcause", i, eg2_trap_rec.mcause,
              kEg2McauseCheri);
    eg2_check(&tally, "lw through the reloaded slot mtval", i, eg2_trap_rec.mtval,
              eg2_cap_mtval(kCapReg, kEg2CauseTag));
  }
  trapped = eg2_probe_call(eg2_ms_auth(nb), NULL, NULL, eg2_rv_stub_clc_lw);
  if (eg2_check(&tally, "lw through the reloaded neighbour trapped", i, trapped, 0)) {
    eg2_check(&tally, "lw through the reloaded neighbour", i, eg2_trap_rec.value,
              obj_word(obj_n));
  }
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  uint32_t main_arena = eg2_main_arena();
  uint32_t n = EG2_PARAM(n_cases) != 0 ? EG2_PARAM(n_cases) : eg2_rand_range(32, 128);
  if (n < 12) {
    n = 12;
  }
  eg2_put_kv("EG2 revoke_store_clear cases", n);
  eg2_put_kv(" main_arena", main_arena);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  for (uint32_t j = 0; j < kNumObjects; ++j) {
    uint32_t a = obj_addr(j);
    eg2_ms_write32(a, obj_word(a));
    eg2_ms_write32(a + 4, obj_word(a + 4));
  }
  eg2_probe_begin();
  static const uint32_t kWidths[3] = {1, 2, 4};
  for (uint32_t i = 0; i < n; ++i) {
    bool in_main;
    uint32_t width, word;
    if (i < 12) {
      // Every memory x width x word once.
      in_main = (i & 1) != 0;
      word = (i >> 1) & 1;
      width = kWidths[i / 4];
    } else {
      in_main = eg2_pick(2) != 0;
      word = eg2_pick(2);
      width = kWidths[eg2_pick(3)];
    }
    one_case(i, main_arena, in_main, width, word);
  }
  eg2_probe_end();
  eg2_tally_print("revoke_store_clear", &tally);
  return eg2_tally_ok(&tally);
}
