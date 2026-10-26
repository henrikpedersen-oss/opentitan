// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_core_write_snoop (EG2 spec "Revocation", "Corner cases"; REQ_CMS_12).
 * Ports cms_tbre_store_race and cms_tbre_snoop_window.
 *
 * `n_slots` capabilities in sec SRAM's arena B, each based in a one-granule object in arena A,
 * `marked_pct` % of whose granules are marked. Each of `n_sweeps` sweeps covers the slots; while it
 * runs the core writes into the swept range from the top slot downwards, so its writes cross the
 * engine (which sweeps upwards) and some hit the capability it is resolving. Each slot it reaches
 * is written with `write_pct` % chance, the kind drawn by weight: a CSC of a fresh tagged
 * capability (based in an unmarked object, so the engine's verdict on it is "keep" whether it sees
 * the old or the new value), a CSC of an untagged capability, or an integer store to the lower or
 * the upper word. The core stops when TBRE_STATUS.busy falls. The bench counts the core's writes to
 * a capability the TBRE had in flight (`snoop_watch`) and holds the SRAM link's responses
 * (`delays`), which slows the engine and widens its in-flight window.
 *
 * Check, after each sweep with the bitmap cleared (cheriot_mem_sys theory_of_operation.md
 * "Revocation Engine", programmers_guide.md: "A capability the core writes while the engine resolves
 * it keeps the tag the core wrote"):
 *  - each capability the core stored keeps the tag the core wrote (fresh: 1, untagged: 0);
 *    integer stores leave tag 0; the untouched slots are untagged exactly when their object is
 *    marked;
 *  - every slot's two data words are what was last written (the engine never modifies data);
 *  - TBRE_STATUS shows no error, the epoch advanced by 2 per sweep;
 *  - with a bench: at least one core write hit a capability the TBRE had in flight over the run
 *    (the stimulus the spec asks for happened).
 *
 * The marks are set only after the slots are initialised, and no capability the test keeps in a
 * register across a call is based in a marked granule.
 *
 * `model_gaps` (eg2sim): the model's TBRE does not revoke and there is no bench: the untouched
 * marked slots' tags and the snoop count are not checked.
 *
 * Built with EG2_TBRE_CORE_WRITE_SNOOP_INJECT_FAIL, the epoch is expected to advance by 2 more:
 * the run must fail with `MISMATCH epoch`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TBRE_CORE_WRITE_SNOOP_PARAMS(X) \
  X(n_slots, 0, 0, 1024)                \
  X(n_sweeps, 0, 0, 8)                  \
  X(marked_pct, 101, 0, 101)            \
  X(write_pct, 101, 0, 101)             \
  X(w_csc_tagged, 40, 1, 100)           \
  X(w_csc_untagged, 20, 1, 100)         \
  X(w_sw_lo, 20, 1, 100)                \
  X(w_sw_hi, 20, 1, 100)                \
  X(delays, 2, 0, 2)                    \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TBRE_CORE_WRITE_SNOOP_PARAMS);

enum {
  kObjects = kEg2ArenaSize / 8,
  kSlots = kEg2ArenaB,
  // The work area: per object marked; per slot its object, the write kind, the expected words.
  kMarked = kEg2WorkBase,
  kSlotObj = kMarked + 4 * kObjects,
  kKind = kSlotObj + 4 * 1024,
  kExpLo = kKind + 4 * 1024,
  kExpHi = kExpLo + 4 * 1024,
  // Write kinds.
  kNone = 0,
  kCscTagged = 1,
  kCscUntagged = 2,
  kSwLo = 3,
  kSwHi = 4,
};

static eg2_tally_t tally;

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }
static void wr(uint32_t a, uint32_t v) { eg2_ms_write32(a, v); }
static uint32_t obj_addr(uint32_t o) { return kEg2ArenaA + 8 * o; }

/** An unmarked object (the fresh capabilities' bases). */
static uint32_t unmarked_obj(void) {
  for (uint32_t t = 0; t < 4 * kObjects; ++t) {
    uint32_t o = eg2_pick(kObjects);
    if (!rd(kMarked + 4 * o)) {
      return o;
    }
  }
  return kObjects;  // none: the caller makes the write untagged instead
}

/** CSC of `c` to slot `s`, and the words it leaves. */
static void store_cap(uint32_t s, void *c) {
  eg2_cap_t m = eg2_hw_read(c);
  eg2_ms_csc(kSlots + 8 * s, c);
  wr(kExpLo + 4 * s, m.addr);
  wr(kExpHi + 4 * s, m.hi);
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0, bench = !gaps;
  uint32_t n = EG2_PARAM(n_slots) != 0 ? EG2_PARAM(n_slots) : eg2_rand_range(64, 1024);
  uint32_t sweeps = EG2_PARAM(n_sweeps) != 0 ? EG2_PARAM(n_sweeps) : eg2_rand_range(4, 8);
  uint32_t marked_pct =
      EG2_PARAM(marked_pct) == 101 ? eg2_rand_range(10, 90) : EG2_PARAM(marked_pct);
  uint32_t write_pct = EG2_PARAM(write_pct) == 101 ? eg2_rand_range(30, 100) : EG2_PARAM(write_pct);
  bool delays = EG2_PARAM(delays) == 2 ? eg2_pick(4) != 0 : EG2_PARAM(delays) != 0;
  eg2_family_t kinds = {.n = 4, .plain_mask = 0, .boosted = 4};
  kinds.w[0] = EG2_PARAM(w_csc_tagged);
  kinds.w[1] = EG2_PARAM(w_csc_untagged);
  kinds.w[2] = EG2_PARAM(w_sw_lo);
  kinds.w[3] = EG2_PARAM(w_sw_hi);
  eg2_put_kv("EG2 tbre_core_write_snoop slots", n);
  eg2_put_kv(" sweeps", sweeps);
  eg2_put_kv(" marked_pct", marked_pct);
  eg2_put_kv(" write_pct", write_pct);
  eg2_put_kv(" delays", delays);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  if (delays) {
    (void)eg2_rv_delays(true, bench);
  }

  uint32_t epoch0 = eg2_ms_tbre_epoch(), hits_all = 0, writes_all = 0;
  for (uint32_t k = 0; k < sweeps; ++k) {
    ++tally.planned;
    // Initialise with the bitmap clear: the objects' marks, then each slot's capability.
    for (uint32_t o = 0; o < kObjects; ++o) {
      wr(kMarked + 4 * o, eg2_rand_range(0, 99) < marked_pct);
    }
    for (uint32_t s = 0; s < n; ++s) {
      uint32_t o = eg2_pick(kObjects);
      wr(kSlotObj + 4 * s, o);
      wr(kKind + 4 * s, kNone);
      store_cap(s, eg2_ms_object_cap(obj_addr(o) + eg2_pick(8), 8, eg2_pick(4) == 0));
    }
    for (uint32_t o = 0; o < kObjects; ++o) {
      if (rd(kMarked + 4 * o)) {
        eg2_ms_revbm_mark(obj_addr(o), true);
      }
    }

    // The sweep, and the core's writes from the top down while it runs.
    (void)eg2_rv_snoop_watch_begin(bench);
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(kSlots, n, &sw);
    eg2_check(&tally, "sweep started", k, sw.started ? 1u : 0u, 1u);
    uint32_t writes = 0;
    for (uint32_t s = n; s-- > 0;) {
      if (!(eg2_ms_tbre_status() & 1u)) {
        break;
      }
      if (eg2_rand_range(0, 99) >= write_pct) {
        continue;
      }
      uint32_t kind = kCscTagged + eg2_family_pick(&kinds);
      uint32_t a = kSlots + 8 * s;
      if (kind == kCscTagged || kind == kCscUntagged) {
        uint32_t o = unmarked_obj();
        if (o == kObjects) {
          kind = kCscUntagged;
          o = 0;
        }
        void *c = eg2_ms_object_cap(obj_addr(o), 8, false);
        if (kind == kCscUntagged) {
          __asm__ volatile("ccleartag %0, %0" : "+C"(c));
        }
        store_cap(s, c);
      } else {
        uint32_t v = eg2_rand();
        uint32_t off = kind == kSwHi ? 4 : 0;
        wr(a + off, v);
        wr((kind == kSwHi ? kExpHi : kExpLo) + 4 * s, v);
      }
      wr(kKind + 4 * s, kind);
      ++writes;
    }
    eg2_ms_tbre_wait(&sw, 10000000);
    uint32_t hits;
    (void)eg2_rv_snoop_watch_end(&hits, bench);
    hits_all += hits;
    writes_all += writes;
    eg2_check(&tally, "sweep done", k, sw.done ? 1u : 0u, 1u);
    eg2_check(&tally, "TBRE_STATUS", k, sw.status, 0);
    eg2_ms_revbm_clear();

    // The slots, with the bitmap clear.
    for (uint32_t s = 0; s < n; ++s) {
      uint32_t a = kSlots + 8 * s, kind = rd(kKind + 4 * s);
      bool marked = rd(kMarked + 4 * rd(kSlotObj + 4 * s)) != 0;
      uint32_t exp = kind == kNone ? !marked : kind == kCscTagged ? 1u : 0u;
      if (!(gaps && kind == kNone && marked)) {
        eg2_check(&tally,
                  kind == kNone        ? "untouched slot's tag"
                  : kind == kCscTagged ? "core-stored capability's tag"
                                       : "overwritten slot's tag",
                  s, eg2_ms_tag(a), exp);
      }
      eg2_check(&tally, "lower word", s, rd(a), rd(kExpLo + 4 * s));
      eg2_check(&tally, "upper word", s, rd(a + 4), rd(kExpHi + 4 * s));
    }
    ++tally.done;
  }
  if (delays) {
    (void)eg2_rv_delays(false, bench);
  }
  uint32_t exp_epoch = epoch0 + 2 * sweeps;
#ifdef EG2_TBRE_CORE_WRITE_SNOOP_INJECT_FAIL
  exp_epoch += 2;
#endif
  eg2_check(&tally, "epoch", 0, eg2_ms_tbre_epoch(), exp_epoch);
  if (bench) {
    eg2_check(&tally, "a core write hit a capability in flight", 0, hits_all != 0, 1u);
  }
  eg2_ms_tbre_clear();
  eg2_put_kv("EG2 tbre_core_write_snoop core writes during sweeps", writes_all);
  eg2_put_kv(" snoop hits", hits_all);
  eg2_puts("\r\n");
  eg2_tally_print("tbre_core_write_snoop", &tally);
  return eg2_tally_ok(&tally);
}
