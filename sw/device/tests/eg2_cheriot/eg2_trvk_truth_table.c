// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_trvk_truth_table (EG2 spec "Revocation", "Corner cases"; REQ_CMS_05,
 * REQ_CMS_11). Ports cheriot_revoke_load_barrier, cheriot_revoke_attenuation and
 * revocation_barrier_phase1-3.
 *
 * The oracle is the load filter's expression (eg2_revoke.h, from the TRVK and TBRE software test
 * plan): tag = Tag(c) & !(!Sealing(c) & Revocable(Base(c)) & REVBM(Base(c))), with Tag(c) and the
 * attenuation as CHERIoT-Sail's LoadCapImm gives them before the revocation check.
 *
 * Each phase stores its capabilities ("cases"), sets the bitmap, reloads every case through its
 * authority, clears the bitmap, then checks every reload (tag, high word, address) against the
 * expression, and reloads every case again: with the bitmap clear every case is back to what
 * LoadCapImm gives without revocation (the filter does not change memory).
 *  1. Each variable alone, bitmap: 32 tagged plain capabilities based in sec SRAM's arena A, at
 *     bit selects 0 and 31 of their bitmap words and at base offsets 0-7 within the granule, under
 *     the bitmap all 0s, all 1s, alternating (0x55555555, 0xaaaaaaaa) and random.
 *  2. Sealing, all marked: plain, sealed memory capabilities (object type 9: revoked like unsealed
 *     ones), sealing capabilities with SE, US and U0 each alone and all three, and the sealing root
 *     with every permission removed (not sealing: revoked).
 *  3. Revocable, all marked: bases in the window, and outside it whose bitmap alias (the base's
 *     offset from the window modulo 256 KiB, the reach of the 12-bit bitmap byte address) is a
 *     marked granule: above the window, below it, in RRAM, in ROM and in MMIO.
 *  4. The 16 combinations of Tag, Sealing, Revocable and REVBM, each stored in main SRAM, sec SRAM
 *     and RRAM, and loaded through an authority with every memory permission, one without LG and
 *     one without LM (the attenuation follows the tag before revocation).
 *  5. `n_random` random cases: any of the above, large exponents (lengths to 64 KiB), the address
 *     outside the bounds but representable, both ways round, over a random bitmap.
 *  6. Phase 4's bitmap again, a sweep of each range holding cases (main SRAM, sec SRAM, RRAM), the
 *     bitmap cleared, every case reloaded: the expression is committed to the tags in memory.
 *
 * The test keeps a shadow of the bitmap in sec SRAM; the marked granules are arena A's (and, in
 * phase 5, the main SRAM arena's), where no capability the test keeps in a register across a call
 * is based. RRAM slots are 16 bytes apart, each programmed through rram_ctrl and, if tagged, given
 * its tag by a verified CSC (programmers_guide.md "Storing Capabilities in the NVM").
 *
 * `model_gaps` (eg2sim): the SystemC model has no load filter and its TBRE does not revoke
 * (eg2_mpact_cpu.h): a tag the expression revokes is not checked (high word and address are).
 *
 * Built with EG2_TRVK_TRUTH_TABLE_INJECT_FAIL, the first case's tag with the bitmap clear is
 * expected inverted: the run must fail with `MISMATCH tag, bitmap clear #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TRVK_TRUTH_TABLE_PARAMS(X) \
  X(n_random, 0, 0, 128)           \
  X(rram_cases, 2, 0, 2)           \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TRVK_TRUTH_TABLE_PARAMS);

enum {
  kMaxCases = 512,
  kArenaGranules = kEg2ArenaSize / 8,
  // The work area: the bitmap's shadow, then the cases.
  kShadow = kEg2WorkBase,
  kShadowWords = kEg2MsRevbmSize / 4,
  kCases = kShadow + kEg2MsRevbmSize,
  kCaseWords = 8,
  // Per case: slot address, flags, the stored capability (high word, address, tag), the reload
  // (high word, address, tag).
  kCSlot = 0,
  kCFlags = 1,
  kCHi = 2,
  kCAddr = 3,
  kCTag = 4,
  kCGotHi = 5,
  kCGotAddr = 6,
  kCGotTag = 7,
  // Flags: the authority (bits 1:0), the region (bits 3:2).
  kAuthFull = 0,
  kAuthNoLg = 1,
  kAuthNoLm = 2,
  kRegSec = 0,
  kRegMain = 1,
  kRegRram = 2,
  // The sweep would have cleared the tag; the model's TBRE does not (model_gaps).
  kFlagSweepGap = 1u << 4,
  // Slot areas: arena B (sec SRAM), the main SRAM arena, the RRAM area (16 bytes apart).
  kSecSlots = kEg2ArenaB,
  kSecSlotMax = kEg2ArenaSize / 8,
  kMainSlotMax = kEg2MainArenaSize / 8,
  kRramSlotMax = 128,
};

// The value kinds.
enum {
  kVPlain = 0,
  kVSealed,
  kVSe,
  kVUs,
  kVU0,
  kVSeUsU0,
  kVSealNoPerm,
  kVKinds,
};

// Non-revocable bases whose bitmap alias ((base - 0x1000_0000) mod 256 KiB) is arena A's granule
// at the same offset: above and below the window, RRAM, ROM, MMIO.
static uint32_t nonrev_base(uint32_t which, uint32_t granule) {
  uint32_t off = 8 * granule;
  switch (which % 5) {
    case 0:
      return 0x10060000u + off;
    case 1:
      return 0x0ffe0000u + off;
    case 2:
      return 0x30020000u + off;
    case 3:
      return 0x00060000u + off;
    default:
      return 0x40020000u + off;
  }
}

static eg2_tally_t tally;
static bool gaps;
static uint32_t n_cases, n_sec, n_main, n_rram;
static bool rram_ok;

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static void wr(uint32_t a, uint32_t v) { eg2_ms_write32(a, v); }
static uint32_t cw(uint32_t i, uint32_t f) { return rd(kCases + 4 * (kCaseWords * i + f)); }
static void cw_set(uint32_t i, uint32_t f, uint32_t v) { wr(kCases + 4 * (kCaseWords * i + f), v); }

// ------------------------------------------ Bitmap -------------------------------------------

static bool shadow_bit(uint32_t granule_addr) {
  uint32_t w, b;
  if (!eg2_ms_revbm_bit(granule_addr, &w, &b)) {
    return false;
  }
  return (rd(kShadow + 4 * w) >> b) & 1;
}

/** REVBM(base): the bit of the base's granule, or of its alias for a base outside the window. */
static bool revbm_of(uint32_t base) {
  uint32_t a = kEg2MsHeapBase + ((base - kEg2MsHeapBase) & 0x3ffffu);
  return a < kEg2MsHeapTop && shadow_bit(a & ~7u);
}

static void bm_word(uint32_t w, uint32_t v) {
  wr(kShadow + 4 * w, v);
  eg2_ms_revbm_write(w, v);
}

static void bm_mark(uint32_t granule_addr, bool on) {
  uint32_t w, b;
  if (!eg2_ms_revbm_bit(granule_addr, &w, &b)) {
    return;
  }
  uint32_t v = rd(kShadow + 4 * w);
  bm_word(w, on ? v | (1u << b) : v & ~(1u << b));
}

static uint32_t arena_word(uint32_t k) { return (kEg2ArenaA - kEg2MsHeapBase) / 256 + k; }
static uint32_t arena_granule(uint32_t g) { return kEg2ArenaA + 8 * g; }

// ------------------------------------------ Values -------------------------------------------

/**
 * A value of kind `vk` over [base, base + len), its address moved by `delta` (0: at the base), its
 * tag cleared if `!tagged`.
 */
static void *make_value(uint32_t vk, uint32_t base, uint32_t len, int32_t delta, bool tagged) {
  void *c;
  if (vk <= kVSealed) {
    c = eg2_mem(base, len);
  } else {
    c = eg2_cp_root_seal().hw;
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(base));
    __asm__ volatile("csetbounds %0, %0, %1" : "+C"(c) : "r"(len));
    uint32_t mask = kEg2PermGl | (vk == kVSe        ? kEg2PermSe
                                  : vk == kVUs      ? kEg2PermUs
                                  : vk == kVU0      ? kEg2PermU0
                                  : vk == kVSeUsU0  ? (kEg2PermSe | kEg2PermUs | kEg2PermU0)
                                                    : 0);
    if (vk == kVSealNoPerm) {
      mask = 0;
    }
    __asm__ volatile("candperm %0, %0, %1" : "+C"(c) : "r"(mask));
  }
  if (delta != 0) {
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(base + (uint32_t)delta));
  }
  if (vk == kVSealed) {
    void *s = eg2_cp_root_seal().hw;
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(s) : "r"(9u));
    __asm__ volatile("cseal %0, %0, %1" : "+C"(c) : "C"(s));
  }
  if (!tagged) {
    __asm__ volatile("ccleartag %0, %0" : "+C"(c));
  }
  return c;
}

/** The CLC authority over the slot: every memory permission, or without LG or LM. */
static void *make_auth(uint32_t slot, uint32_t kind) {
  void *a = eg2_ms_auth(slot);
  if (kind != kAuthFull) {
    uint32_t mask = kEg2PermAll & ~(uint32_t)(kind == kAuthNoLg ? kEg2PermLg : kEg2PermLm);
    __asm__ volatile("candperm %0, %0, %1" : "+C"(a) : "r"(mask));
  }
  return a;
}

/**
 * Stores `v` as a new case in `region` with authority `auth`. Returns false if there is no room
 * (the case is dropped) or the RRAM could not be programmed.
 */
static bool add_case(void *v, uint32_t region, uint32_t auth) {
  if (n_cases >= kMaxCases) {
    return false;
  }
  if (region == kRegRram && (!rram_ok || n_rram >= kRramSlotMax)) {
    region = kRegSec;
  }
  if (region == kRegMain && (eg2_main_arena() == 0 || n_main >= kMainSlotMax)) {
    region = kRegSec;
  }
  if (region == kRegSec && n_sec >= kSecSlotMax) {
    return false;
  }
  eg2_cap_t m = eg2_hw_read(v);
  uint32_t slot;
  if (region == kRegRram) {
    slot = kEg2MsRramArea + 16 * n_rram++;
    uint32_t w[4] = {m.addr, m.hi, 0, 0};
    uint32_t err;
    if (!eg2_ms_rram_program(slot, w, 4, &err)) {
      eg2_ms_note("rram_ctrl program failed, ERR_CODE", err);
      eg2_check(&tally, "rram program", n_rram, err, 0);
      return false;
    }
    // A verified CSC gives the tag; an untagged value is left as programmed (a CSC of an untagged
    // capability to the NVM is refused).
    if (m.tag) {
      eg2_ms_csc(slot, v);
    }
  } else {
    slot = region == kRegMain ? eg2_main_arena() + 8 * n_main++ : kSecSlots + 8 * n_sec++;
    eg2_ms_csc(slot, v);
  }
  uint32_t i = n_cases++;
  cw_set(i, kCSlot, slot);
  cw_set(i, kCFlags, auth | region << 2);
  cw_set(i, kCHi, m.hi);
  cw_set(i, kCAddr, m.addr);
  cw_set(i, kCTag, m.tag);
  return true;
}

// ----------------------------------------- Phases --------------------------------------------

/** Reloads cases [lo, hi) through their authorities (the bitmap as the phase set it). */
static OT_NOINLINE void reload(uint32_t lo, uint32_t hi) {
  for (uint32_t i = lo; i < hi; ++i) {
    void *a = make_auth(cw(i, kCSlot), cw(i, kCFlags) & 3);
    void *c;
    uint32_t g_hi, g_addr, g_tag;
    // One statement: the loaded capability is read out before anything else runs (no spill and
    // reload of it through the filter). CGetAddr is spelled out (the assembler turns `cgetaddr`
    // into `mv`).
    __asm__ volatile(
        "clc %3, 0(%4)\n"
        "cgethigh %0, %3\n"
        ".insn r 0x5b, 0, 0x7f, %1, %3, x15\n"
        "cgettag %2, %3\n"
        : "=r"(g_hi), "=r"(g_addr), "=r"(g_tag), "=&C"(c)
        : "C"(a)
        : "memory");
    cw_set(i, kCGotHi, g_hi);
    cw_set(i, kCGotAddr, g_addr);
    cw_set(i, kCGotTag, g_tag);
  }
}

/** Compares cases [lo, hi) with the expression under the shadow bitmap (`clear`: all 0). */
static void check(const char *phase, uint32_t lo, uint32_t hi, bool clear) {
  for (uint32_t i = lo; i < hi; ++i) {
    eg2_cap_t mem = {.hi = cw(i, kCHi), .addr = cw(i, kCAddr), .tag = cw(i, kCTag) != 0};
    eg2_cap_t auth = eg2_hw_read(make_auth(cw(i, kCSlot), cw(i, kCFlags) & 3));
    bool bit = !clear && revbm_of(eg2_cap_base(mem));
    eg2_cap_t e = eg2_rv_expect(auth, mem, bit);
    bool revoked = eg2_cap_clc_result(auth, mem).tag && !e.tag;
    uint32_t exp_tag = e.tag;
#ifdef EG2_TRVK_TRUTH_TABLE_INJECT_FAIL
    if (clear && i == 0) {
      exp_tag = !exp_tag;
    }
#endif
    // A tag the model's TBRE would have cleared (phase 6) is a model gap as well.
    revoked = revoked || (gaps && (cw(i, kCFlags) & kFlagSweepGap));
    uint32_t failed = tally.failed;
    if (!(gaps && revoked)) {
      eg2_check(&tally, clear ? "tag, bitmap clear" : "tag", i, cw(i, kCGotTag), exp_tag);
    }
    eg2_check(&tally, "high word", i, cw(i, kCGotHi), e.hi);
    eg2_check(&tally, "address", i, cw(i, kCGotAddr), e.addr);
    if (tally.failed != failed && tally.failed <= kEg2MaxReports) {
      eg2_puts("  ");
      eg2_puts(phase);
      eg2_put_kv(" slot", cw(i, kCSlot));
      eg2_put_kv(" flags", cw(i, kCFlags));
      eg2_put_kv(" base", eg2_cap_base(mem));
      eg2_put_kv(" revbm", bit);
      eg2_put_kv(" stored hi", mem.hi);
      eg2_put_kv(" tag", mem.tag);
      eg2_puts("\r\n");
    }
  }
}

/** Reloads with the phase's bitmap, clears it, checks; then reloads with it clear and checks. */
static void run_phase(const char *phase, uint32_t lo, uint32_t hi) {
  reload(lo, hi);
  // The shadow still holds the phase's marks for the check.
  for (uint32_t w = 0; w < kShadowWords; ++w) {
    if (rd(kShadow + 4 * w) != 0) {
      eg2_check(&tally, "bitmap word read back", w, eg2_ms_revbm_read(w), rd(kShadow + 4 * w));
      eg2_ms_revbm_write(w, 0);
    }
  }
  check(phase, lo, hi, false);
  for (uint32_t w = 0; w < kShadowWords; ++w) {
    wr(kShadow + 4 * w, 0);
  }
  reload(lo, hi);
  check(phase, lo, hi, true);
  tally.done += hi - lo;
}

/** A granule of arena A not yet used in this phase (marked in `used`, one word per granule). */
static uint32_t fresh_granule(uint32_t used) {
  for (uint32_t tries = 0; tries < 64; ++tries) {
    uint32_t g = eg2_pick(kArenaGranules);
    if (!rd(used + 4 * g)) {
      wr(used + 4 * g, 1);
      return g;
    }
  }
  for (uint32_t g = 0; g < kArenaGranules; ++g) {
    if (!rd(used + 4 * g)) {
      wr(used + 4 * g, 1);
      return g;
    }
  }
  return 0;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n_random = EG2_PARAM(n_random) != 0 ? EG2_PARAM(n_random) : eg2_rand_range(16, 128);
  uint32_t rram_cases = EG2_PARAM(rram_cases) == 2 ? 1 : EG2_PARAM(rram_cases);
  eg2_put_kv("EG2 trvk_truth_table n_random", n_random);
  eg2_put_kv(" rram", rram_cases);
  eg2_puts("\r\n");
  eg2_cp_save_sealing_root();
  rram_ok = rram_cases != 0 && eg2_ms_rram_setup();
  if (rram_cases != 0 && !rram_ok) {
    eg2_puts("EG2 rram_ctrl initialisation did not complete\r\n");
    return false;
  }
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  for (uint32_t w = 0; w < kShadowWords; ++w) {
    wr(kShadow + 4 * w, 0);
  }
  // The used-granule map of a phase: after the cases' table.
  uint32_t used = kCases + 4 * kCaseWords * kMaxCases;

  // 1. The bitmap alone: 32 cases at bit selects 0 and 31 and base offsets 0-7.
  uint32_t p1 = n_cases;
  for (uint32_t k = 0; k < 32; ++k) {
    uint32_t g = 32 * (k % 32) + ((k & 1) ? 31 : 0);
    (void)add_case(make_value(kVPlain, arena_granule(g) + (k % 8), 8, 0, true), kRegSec, kAuthFull);
  }
  static const uint32_t kPatterns[4] = {0, 0xffffffffu, 0x55555555u, 0};
  for (uint32_t p = 0; p < 4; ++p) {
    for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
      uint32_t v = p == 3 ? eg2_rand() : p == 2 ? (kPatterns[2] << (k & 1)) : kPatterns[p];
      bm_word(arena_word(k), v);
    }
    run_phase(p == 0 ? "bitmap 0s" : p == 1 ? "bitmap 1s" : p == 2 ? "alternating" : "random",
              p1, n_cases);
  }

  // 2. Sealing, with arena A marked.
  uint32_t p2 = n_cases;
  for (uint32_t vk = 0; vk < kVKinds; ++vk) {
    for (uint32_t k = 0; k < 3; ++k) {
      (void)add_case(make_value(vk, arena_granule(eg2_pick(kArenaGranules)) + eg2_pick(8), 8, 0,
                                true),
                     kRegSec, kAuthFull);
    }
  }
  for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
    bm_word(arena_word(k), 0xffffffffu);
  }
  run_phase("sealing", p2, n_cases);

  // 3. Revocable, with arena A marked: the window and the aliases from outside it.
  uint32_t p3 = n_cases;
  for (uint32_t which = 0; which < 6; ++which) {
    for (uint32_t k = 0; k < 3; ++k) {
      uint32_t g = eg2_pick(kArenaGranules);
      uint32_t base = which == 5 ? arena_granule(g) : nonrev_base(which, g);
      (void)add_case(make_value(kVPlain, base + eg2_pick(8), 8, 0, true), kRegSec, kAuthFull);
    }
  }
  for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
    bm_word(arena_word(k), 0xffffffffu);
  }
  run_phase("revocable", p3, n_cases);

  // 4. The 16 combinations, by region and authority.
  for (uint32_t g = 0; g < kArenaGranules; ++g) {
    wr(used + 4 * g, 0);
  }
  uint32_t p4 = n_cases;
  for (uint32_t region = 0; region < 3; ++region) {
    for (uint32_t auth = 0; auth < 3; ++auth) {
      for (uint32_t combo = 0; combo < 16; ++combo) {
        bool tag = combo & 1, sealing = combo & 2, revocable = combo & 4, revbm = combo & 8;
        uint32_t g = fresh_granule(used);
        uint32_t base = revocable ? arena_granule(g) : nonrev_base(eg2_pick(5), g);
        uint32_t vk = sealing ? kVSe + eg2_pick(4) : (eg2_pick(3) == 0 ? kVSealed : kVPlain);
        base += vk <= kVSealed ? eg2_pick(8) : 0;
        if (add_case(make_value(vk, base, 8, 0, tag), region, auth) && revbm) {
          bm_mark(arena_granule(g), true);
        }
      }
    }
  }
  // The bitmap of phase 4, kept for phase 6.
  uint32_t p4_end = n_cases;
  uint32_t p4_marks = used + 4 * kArenaGranules;
  for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
    wr(p4_marks + 4 * k, rd(kShadow + 4 * arena_word(k)));
  }
  run_phase("combinations", p4, p4_end);

  // 5. Random cases over a random bitmap (arena A and the main SRAM arena).
  uint32_t p5 = n_cases;
  for (uint32_t k = 0; k < n_random; ++k) {
    uint32_t vk = eg2_pick(kVKinds);
    uint32_t len = eg2_pick(4) == 0 ? (1u << eg2_rand_range(10, 16)) : eg2_rand_range(1, 64);
    uint32_t where = eg2_pick(4);
    uint32_t base;
    if (len >= 1024) {
      // A large exponent: based at arena A's first granule (aligned to 64 KiB) or outside.
      base = where == 0 ? 0x10060000u : kEg2ArenaA;
    } else if (where == 0) {
      base = nonrev_base(eg2_pick(5), eg2_pick(kArenaGranules)) + eg2_pick(8);
    } else if (where == 1 && eg2_main_arena() != 0) {
      base = eg2_main_arena() + 8 * eg2_pick(kMainSlotMax) + eg2_pick(8);
    } else {
      base = arena_granule(eg2_pick(kArenaGranules)) + eg2_pick(8);
    }
    // The address outside the bounds (but representable), below the base or above the top.
    int32_t delta = 0;
    uint32_t d = eg2_pick(4);
    if (d == 1) {
      delta = -(int32_t)(8 * eg2_rand_range(1, 4));
    } else if (d == 2) {
      delta = (int32_t)(len + 8 * eg2_rand_range(0, 4));
    }
    (void)add_case(make_value(vk, base, len, delta, eg2_pick(8) != 0), eg2_pick(3), eg2_pick(3));
  }
  for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
    bm_word(arena_word(k), eg2_rand());
  }
  if (eg2_main_arena() != 0) {
    for (uint32_t g = 0; g < kMainSlotMax; ++g) {
      if (eg2_pick(2)) {
        bm_mark(eg2_main_arena() + 8 * g, true);
      }
    }
  }
  run_phase("random", p5, n_cases);

  // 6. Phase 4's bitmap, a sweep of each slot range, the bitmap cleared, every case reloaded.
  for (uint32_t k = 0; k < kArenaGranules / 32; ++k) {
    bm_word(arena_word(k), rd(p4_marks + 4 * k));
  }
  uint32_t epoch0 = eg2_ms_tbre_epoch(), n_sweeps = 0;
  struct {
    uint32_t base, n;
  } ranges[3] = {
      {kSecSlots, n_sec},
      {eg2_main_arena(), n_main},
      {kEg2MsRramArea, 2 * n_rram},
  };
  for (uint32_t r = 0; r < 3; ++r) {
    if (ranges[r].n == 0) {
      continue;
    }
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(ranges[r].base, ranges[r].n, &sw);
    eg2_ms_tbre_wait(&sw, 10000000);
    eg2_check(&tally, "sweep", r, sw.started && sw.done ? sw.status : 0xffffffffu, 0);
    ++n_sweeps;
  }
  eg2_check(&tally, "epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2 * n_sweeps);
  // What the sweeps left in memory: the expression on the stored value under phase 4's bitmap.
  for (uint32_t i = 0; i < n_cases; ++i) {
    eg2_cap_t mem = {.hi = cw(i, kCHi), .addr = cw(i, kCAddr), .tag = cw(i, kCTag) != 0};
    bool revoked = eg2_rv_revokes(mem, revbm_of(eg2_cap_base(mem)));
    if (revoked) {
      // The model's TBRE leaves the tag (and so the attenuation on reload) as stored.
      cw_set(i, kCFlags, cw(i, kCFlags) | kFlagSweepGap);
      if (!gaps) {
        cw_set(i, kCTag, 0);
      }
    }
  }
  for (uint32_t w = 0; w < kShadowWords; ++w) {
    if (rd(kShadow + 4 * w) != 0) {
      eg2_ms_revbm_write(w, 0);
      wr(kShadow + 4 * w, 0);
    }
  }
  reload(0, n_cases);
  check("after the sweeps", 0, n_cases, true);
  tally.planned += n_cases;
  tally.done += n_cases;

  eg2_ms_tbre_clear();
  eg2_put_kv("EG2 trvk_truth_table cases", n_cases);
  eg2_put_kv(" sec", n_sec);
  eg2_put_kv(" main", n_main);
  eg2_put_kv(" rram", n_rram);
  eg2_puts("\r\n");
  // Planned: every case of phases 1-5 (phase 1 four times).
  tally.planned += (p2 - p1) * 4 + (n_cases - p2);
  eg2_tally_print("trvk_truth_table", &tally);
  return eg2_tally_ok(&tally);
}
