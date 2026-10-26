// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_race (EG2 spec "Revocation", "Corner cases"; REQ_CMS_11, REQ_CMS_12;
 * ports the RTOS test `revocation_sweep_race`).
 *
 * Revocation-bitmap writes racing loads and sweeps. Capabilities in slots in sec SRAM's arena B,
 * each based on a one-granule object in arena A (objects of different parts in different bitmap
 * words). Every bit the test sets is set only inside an op window (eg2_revoke.h): no call runs
 * while it is set, and every check is made afterwards, by reloading, once the bit is clear.
 *
 *  A. In flight (bench, `n_sba_batches`): the bench writes the bitmap word of granule G over SBA,
 *     alternately with G's bit set and clear (race_toggle), while a window runs 1-32 CLCs of a
 *     capability based in G, with random spins (up to `spin_max`) between them. SBA writes land
 *     before a CLC's bitmap read, while it is outstanding, and after it (the bench logs how many of
 *     each). The bench records the bit as each of the core's bitmap reads of that word saw it.
 *     Check: as many bitmap reads as CLCs, and each CLC's tag is the inverse of the bit its own
 *     read returned (the expression with REVBM as the check read it, never a mixture).
 *  B. Free and sweep while loading: 16 slots, about half their objects marked (the free), a sweep
 *     over the slots started, the slots reloaded back to back for 4 rounds while it runs, the sweep
 *     waited for and the marks cleared, all in one window. Check: every reload of a marked slot
 *     returns tag 0, of an unmarked one tag 1; after the sweep, the marked slots are untagged in
 *     memory and the others keep their tags; TBRE_STATUS clean and the epoch +2.
 *  C. Ahead of and behind the sweep (bench, `n_hold`): 64 slots, a sweep over them; when it reaches
 *     slot P (16-47), the bench holds the TBRE's next read (tbre_hold_arm). The window then sets and
 *     clears marks ahead of the sweep (slots > P + 2) and behind it (slots < P - 2), reloads every
 *     slot, releases the hold, waits for the end of the sweep and clears every mark. Per slot one
 *     class: never marked (U), marked throughout (M), marked during the hold (A), marked before the
 *     sweep and cleared during the hold (C); slots within 2 of P are U or M only. Check: during the
 *     hold every reload returns tag 0 exactly for a slot marked then (M, A) or swept while it was
 *     marked (C behind); after the sweep, memory tags U 1, M 0, A ahead 0, A behind 1, C ahead 1,
 *     C behind 0
 *     (the sweep revokes by the bitmap as it reads it, theory_of_operation.md "Revocation
 *     Engine"). The bench counts the TBRE's reads (tbre_watch): exactly two per slot, none outside
 *     the slots (no granule skipped or swept twice).
 *  D. 64 slots reloaded back to back through one sweep (revocation_sweep_race): a calibration sweep
 *     over the whole of sec SRAM with nothing marked, timed with mcycle; then the 64 objects marked,
 *     the same sweep started and the 64 slots reloaded in rounds (enough rounds, from the
 *     calibration, to span the sweep), the sweep waited for and the marks cleared. Check: the
 *     calibration sweep leaves every slot tagged; no marked slot reloads tagged during the sweep;
 *     every slot untagged after it; both sweeps clean, the epoch +2 each.
 *
 * `model_gaps` (eg2sim): the SystemC model has no load filter and no revoking TBRE
 * (eg2_mpact_cpu.h) and there is no bench: parts A and C are skipped, and the tags that only
 * revocation makes 0 are not checked.
 *
 * Built with EG2_REVOCATION_RACE_INJECT_FAIL, part B expects its first unmarked slot to reload
 * untagged during the sweep: the run must fail with `MISMATCH B: tag during the sweep #<n>`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVOCATION_RACE_PARAMS(X) \
  X(n_sba_batches, 0, 0, 32)      \
  X(spin_max, 0, 0, 2000)         \
  X(n_hold, 0, 0, 8)              \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOCATION_RACE_PARAMS);

enum {
  // Objects: one granule each, in arena A; each part's in bitmap words of its own.
  kObjB = kEg2ArenaA,                  // 16 granules
  kObjC = kEg2ArenaA + 0x200,          // 64 granules (two bitmap words)
  kObjD = kEg2ArenaA + 0x400,          // 64 granules
  kObjA = kEg2ArenaA + 0x800,          // part A: one granule in [0x800, 0x2000)
  // Slots in arena B.
  kSlotA = kEg2ArenaB,
  kSlotB = kEg2ArenaB + 0x100,
  kSlotC = kEg2ArenaB + 0x400,
  kSlotD = kEg2ArenaB + 0x800,
  kNumB = 16,
  kNumC = 64,
  kNumD = 64,
  kRoundsB = 4,
  // The work area: the window's ops, its out words, the release word, the classes.
  kOps = kEg2WorkBase,
  kMaxOps = 2048,
  kOut = kOps + 12 * kMaxOps,
  kRelease = kOut + 4 * kMaxOps,
  kClass = kRelease + 8,
  kSecCaps = kEg2SecSize / 8,
};

typedef enum cls {
  kClsU = 0,
  kClsM = 1,
  kClsA = 2,
  kClsC = 3,
} cls_t;

static eg2_tally_t tally;
static bool gaps, bench;

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static uint32_t out(uint32_t i) { return rd(kOut + 4 * i); }

static uint32_t revbm_word(uint32_t addr) { return (addr - kEg2MsHeapBase) / 256; }
static uint32_t revbm_bit(uint32_t addr) { return ((addr - kEg2MsHeapBase) / 8) % 32; }

/** Tagged slots: slot i of `slots` based on object i of `objs`. */
static bool slots_fill(uint32_t slots, uint32_t objs, uint32_t n, const char *what) {
  for (uint32_t i = 0; i < n; ++i) {
    eg2_ms_csc(slots + 8 * i, eg2_ms_object_cap(objs + 8 * i, 8, false));
    if (!eg2_check(&tally, what, i, eg2_ms_tag(slots + 8 * i), 1)) {
      return false;
    }
  }
  return true;
}

/**
 * Appends the bitmap words covering objects [objs, objs + 8n) with the objects whose `marked`
 * bit is set (one bit per object, up to 64).
 */
static void op_marks(eg2_rv_ops_t *o, uint32_t objs, uint32_t n, uint64_t marked) {
  uint32_t w0 = revbm_word(objs), w1 = revbm_word(objs + 8 * n - 8);
  for (uint32_t w = w0; w <= w1; ++w) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t a = objs + 8 * i;
      if (revbm_word(a) == w && ((marked >> i) & 1)) {
        v |= 1u << revbm_bit(a);
      }
    }
    eg2_rv_op_revbm(o, w, v);
  }
}

static bool sweep_clean(const char *what, uint32_t idx, uint32_t wait_out, uint32_t epoch0) {
  bool ok = eg2_check(&tally, what, idx, wait_out != 0 ? 1u : 0u, 1u);
  uint32_t st = eg2_ms_tbre_status();
  ok = eg2_check(&tally, "TBRE_STATUS after the sweep", idx, st, 0) && ok;
  ok = eg2_check(&tally, "epoch after the sweep", idx, eg2_ms_tbre_epoch(), epoch0 + 2) && ok;
  eg2_ms_tbre_clear();
  return ok;
}

// ------------------------------------------- A -----------------------------------------------

static void part_a(uint32_t batch, uint32_t spin_max) {
  uint32_t g = kObjA + 8 * eg2_pick((kEg2ArenaSize - 0x800) / 8);
  uint32_t k = eg2_rand_range(1, 32);
  ++tally.planned;
  eg2_ms_csc(kSlotA, eg2_ms_object_cap(g, 8, false));
  if (!eg2_check(&tally, "A: slot tagged", batch, eg2_ms_tag(kSlotA), 1)) {
    return;
  }
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  for (uint32_t i = 0; i < k; ++i) {
    (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlotA, 0);
    (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, eg2_rand_range(0, spin_max));
  }
  uint32_t word_addr = kEg2MsMetaBase + 4 * revbm_word(g);
  (void)eg2_rv_race_toggle_begin(word_addr, revbm_bit(g), bench);
  eg2_rv_ops_run(&o, kOut);
  uint32_t reads, seen;
  (void)eg2_rv_race_toggle_end(&reads, &seen, bench);
  // The bench left the word 0.
  eg2_check(&tally, "A: bitmap word after the toggling", batch, eg2_ms_revbm_read(revbm_word(g)),
            0);
  eg2_check(&tally, "A: bitmap reads", batch, reads, k);
  uint32_t marked = 0;
  for (uint32_t i = 0; i < k && i < reads; ++i) {
    uint32_t bit = (seen >> i) & 1;
    marked += bit;
    eg2_check(&tally, "A: tag against the bitmap read", batch * 32 + i, out(i), bit ^ 1u);
  }
  eg2_put_kv("EG2 A batch", batch);
  eg2_put_kv(" clcs", k);
  eg2_put_kv(" marked reads", marked);
  eg2_puts("\r\n");
  ++tally.done;
}

// ------------------------------------------- B -----------------------------------------------

static void part_b(void) {
  ++tally.planned;
  if (!slots_fill(kSlotB, kObjB, kNumB, "B: slot tagged")) {
    return;
  }
  uint64_t marked = 0;
  for (uint32_t i = 0; i < kNumB; ++i) {
    if (eg2_pick(2)) {
      marked |= 1ull << i;
    }
  }
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  op_marks(&o, kObjB, kNumB, marked);
  eg2_rv_op_sweep(&o, kSlotB, kNumB);
  uint32_t first = o.outs;
  for (uint32_t r = 0; r < kRoundsB; ++r) {
    for (uint32_t i = 0; i < kNumB; ++i) {
      (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlotB + 8 * i, 0);
    }
  }
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  op_marks(&o, kObjB, kNumB, 0);
  eg2_check(&tally, "B: window fits", 0, o.full, 0);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_rv_ops_run(&o, kOut);

  bool injected = false;
  (void)injected;
  for (uint32_t r = 0; r < kRoundsB; ++r) {
    for (uint32_t i = 0; i < kNumB; ++i) {
      bool m = (marked >> i) & 1;
      uint32_t exp = m ? 0 : 1;
#ifdef EG2_REVOCATION_RACE_INJECT_FAIL
      if (!m && !injected) {
        injected = true;
        exp = 0;
      }
#endif
      if (!(gaps && m)) {
        eg2_check(&tally, "B: tag during the sweep", r * kNumB + i, out(first + r * kNumB + i),
                  exp);
      }
    }
  }
  sweep_clean("B: sweep ended", 0, out(wait), epoch0);
  for (uint32_t i = 0; i < kNumB; ++i) {
    bool m = (marked >> i) & 1;
    if (!(gaps && m)) {
      eg2_check(&tally, "B: tag after the sweep", i, eg2_ms_tag(kSlotB + 8 * i), m ? 0 : 1);
    }
  }
  ++tally.done;
}

// ------------------------------------------- C -----------------------------------------------

static void part_c(uint32_t round) {
  ++tally.planned;
  if (!slots_fill(kSlotC, kObjC, kNumC, "C: slot tagged")) {
    return;
  }
  uint32_t p = eg2_rand_range(16, 47);
  uint64_t before = 0, during = 0;
  for (uint32_t i = 0; i < kNumC; ++i) {
    bool near = i + 2 >= p && i <= p + 2;
    cls_t c = (cls_t)(near ? eg2_pick(2) : eg2_pick(4));
    eg2_ms_write32(kClass + 4 * i, c);
    if (c == kClsM || c == kClsC) {
      before |= 1ull << i;
    }
    if (c == kClsM || c == kClsA) {
      during |= 1ull << i;
    }
  }
  eg2_ms_write32(kRelease, 0);
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  op_marks(&o, kObjC, kNumC, before);
  eg2_rv_op_sweep(&o, kSlotC, kNumC);
  uint32_t held = eg2_rv_op(&o, kEg2RvOpPollEq, kEg2RvAuthAck, eg2_rv_ack_addr() + 4,
                            kEg2RvHoldMagic);
  // The lookups of the slots already read settle.
  (void)eg2_rv_op(&o, kEg2RvOpSpin, kEg2RvAuthHeap, 0, 300);
  op_marks(&o, kObjC, kNumC, during);
  uint32_t first = o.outs;
  for (uint32_t i = 0; i < kNumC; ++i) {
    (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlotC + 8 * i, 0);
  }
  (void)eg2_rv_op(&o, kEg2RvOpSw, kEg2RvAuthHeap, kRelease, 1);
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  op_marks(&o, kObjC, kNumC, 0);
  eg2_check(&tally, "C: window fits", round, o.full, 0);

  uint32_t epoch0 = eg2_ms_tbre_epoch();
  (void)eg2_rv_tbre_watch_begin(kSlotC, kSlotC + 8 * kNumC, bench);
  (void)eg2_rv_tbre_hold_arm(kSlotC + 8 * p, kRelease, bench);
  eg2_rv_ops_run(&o, kOut);
  uint32_t cycles, in, outside;
  (void)eg2_rv_tbre_hold_end(&cycles, bench);
  (void)eg2_rv_tbre_watch_end(&in, &outside, bench);

  eg2_check(&tally, "C: hold took effect", round, out(held) != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "C: hold cycles", round, cycles != 0 ? 1u : 0u, 1u);
  sweep_clean("C: sweep ended", round, out(wait), epoch0);
  eg2_check(&tally, "C: TBRE reads of the slots", round, in, 2 * kNumC);
  eg2_check(&tally, "C: TBRE reads outside the slots", round, outside, 0);
  uint32_t counts[4][2] = {{0}};
  for (uint32_t i = 0; i < kNumC; ++i) {
    cls_t c = (cls_t)rd(kClass + 4 * i);
    bool ahead = i > p;
    counts[c][ahead ? 1 : 0]++;
    // During the hold: 0 if marked now, or if behind the sweep and marked when it passed (its
    // memory tag already cleared).
    bool marked_now = (during >> i) & 1, swept_marked = i < p && ((before >> i) & 1);
    uint32_t during_tag = (marked_now || swept_marked) ? 0 : 1;
    eg2_check(&tally, "C: tag during the hold", round * kNumC + i, out(first + i), during_tag);
    uint32_t exp;
    switch (c) {
      case kClsU:
        exp = 1;
        break;
      case kClsM:
        exp = 0;
        break;
      case kClsA:
        exp = ahead ? 0 : 1;
        break;
      default:
        exp = ahead ? 1 : 0;
        break;
    }
    eg2_check(&tally, "C: tag after the sweep", round * kNumC + i, eg2_ms_tag(kSlotC + 8 * i), exp);
  }
  eg2_put_kv("EG2 C round", round);
  eg2_put_kv(" P", p);
  eg2_put_kv(" hold cycles", cycles);
  eg2_put_kv(" A ahead", counts[kClsA][1]);
  eg2_put_kv(" A behind", counts[kClsA][0]);
  eg2_put_kv(" C ahead", counts[kClsC][1]);
  eg2_put_kv(" C behind", counts[kClsC][0]);
  eg2_puts("\r\n");
  ++tally.done;
}

// ------------------------------------------- D -----------------------------------------------

static void part_d(void) {
  ++tally.planned;
  if (!slots_fill(kSlotD, kObjD, kNumD, "D: slot tagged")) {
    return;
  }
  // The calibration sweep: nothing marked.
  eg2_rv_ops_t o;
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  uint32_t t0 = eg2_rv_op(&o, kEg2RvOpMcycle, kEg2RvAuthHeap, 0, 0);
  eg2_rv_op_sweep(&o, kEg2SecBase, kSecCaps);
  uint32_t wait = eg2_rv_op_sweep_wait(&o);
  uint32_t t1 = eg2_rv_op(&o, kEg2RvOpMcycle, kEg2RvAuthHeap, 0, 0);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_rv_ops_run(&o, kOut);
  uint32_t cal = out(t1) - out(t0);
  sweep_clean("D: calibration sweep ended", 0, out(wait), epoch0);
  for (uint32_t i = 0; i < kNumD; ++i) {
    eg2_check(&tally, "D: tag after the calibration sweep", i, eg2_ms_tag(kSlotD + 8 * i), 1);
  }

  // The 64 slots marked, reloaded back to back through the sweep. A round of 64 CLCs takes some
  // 64 x 12 cycles; enough rounds to span the calibration time, at most 24.
  uint32_t rounds = cal / (kNumD * 12) + 1;
  if (rounds > 24) {
    rounds = 24;
  }
  eg2_rv_ops_init(&o, kOps, kMaxOps);
  op_marks(&o, kObjD, kNumD, ~0ull);
  eg2_rv_op_sweep(&o, kEg2SecBase, kSecCaps);
  uint32_t first = o.outs;
  for (uint32_t r = 0; r < rounds; ++r) {
    for (uint32_t i = 0; i < kNumD; ++i) {
      (void)eg2_rv_op(&o, kEg2RvOpClcTag, kEg2RvAuthHeap, kSlotD + 8 * i, 0);
    }
  }
  uint32_t busy_after = eg2_rv_op(&o, kEg2RvOpLw, kEg2RvAuthRegs,
                                  TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR +
                                      CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET,
                                  0);
  wait = eg2_rv_op_sweep_wait(&o);
  op_marks(&o, kObjD, kNumD, 0);
  eg2_check(&tally, "D: window fits", 0, o.full, 0);
  epoch0 = eg2_ms_tbre_epoch();
  eg2_rv_ops_run(&o, kOut);
  if (!gaps) {
    uint32_t tagged = 0;
    for (uint32_t k = 0; k < rounds * kNumD; ++k) {
      tagged += out(first + k);
    }
    eg2_check(&tally, "D: marked slots reloaded tagged during the sweep", 0, tagged, 0);
  }
  sweep_clean("D: sweep ended", 1, out(wait), epoch0);
  if (!gaps) {
    for (uint32_t i = 0; i < kNumD; ++i) {
      eg2_check(&tally, "D: tag after the sweep", i, eg2_ms_tag(kSlotD + 8 * i), 0);
    }
  }
  eg2_put_kv("EG2 D calibration cycles", cal);
  eg2_put_kv(" rounds", rounds);
  eg2_put_kv(" sweep still busy after the reloads", out(busy_after) & 1);
  eg2_puts("\r\n");
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  uint32_t n_sba = EG2_PARAM(n_sba_batches) != 0 ? EG2_PARAM(n_sba_batches) : eg2_rand_range(4, 12);
  uint32_t spin_max = EG2_PARAM(spin_max) != 0 ? EG2_PARAM(spin_max) : eg2_rand_range(0, 400);
  uint32_t n_hold = EG2_PARAM(n_hold) != 0 ? EG2_PARAM(n_hold) : eg2_rand_range(2, 4);
  eg2_put_kv("EG2 revocation_race sba_batches", bench ? n_sba : 0);
  eg2_put_kv(" spin_max", spin_max);
  eg2_put_kv(" holds", bench ? n_hold : 0);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  for (uint32_t i = 0; i < kEg2ArenaSize / 4; ++i) {
    eg2_ms_write32(kEg2ArenaA + 4 * i, 0xc0de0000u + i);
  }

  if (bench) {
    for (uint32_t b = 0; b < n_sba; ++b) {
      part_a(b, spin_max);
    }
  }
  part_b();
  if (bench) {
    for (uint32_t r = 0; r < n_hold; ++r) {
      part_c(r);
    }
  }
  part_d();
  eg2_ms_tbre_clear();
  eg2_tally_print("revocation_race", &tally);
  return eg2_tally_ok(&tally);
}
