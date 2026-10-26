// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation (EG2 spec "Memory subsystem in CHERIoT mode"; REQ_CMS_11).
 *
 * A granule marked through the software bitmap window clears the tag of every CLC of a capability
 * based in it, in main and sec SRAM, at the first and last granule; a base outside the revocable
 * range is never revoked (RFC CHERIoT in OpenTitan p3 "TRVK Filter Integration"; cheriot_mem_sys
 * theory_of_operation.md "Revocation Engine": "a tagged capability that is not a sealing capability
 * and whose base has its revocation bit set is revoked").
 *
 * Knobs: `n_marked` granules, of `granule_class` (the window's first granule, its last, either
 * side of the main and sec SRAM boundary, interior); `outside_base` capabilities based below or
 * above the window; `seal_pct` of the capabilities sealed (object type 9: revoked like any
 * other) or sealing (permit-seal: never revoked).
 *
 * For each marked granule: a capability based in it (any byte of it), and one based in an
 * unmarked neighbour granule. All are stored with CSC in sec SRAM, the granules marked through the
 * bitmap window (and read back), every capability reloaded with CLC and its tag compared with the
 * test's model: 0 exactly for the non-sealing capabilities based in a marked granule. The marks are
 * then cleared. Interior granules are drawn in sec SRAM and the main SRAM arena, not among the
 * image's globals and stack, whose capabilities the test itself reloads; the window's first and
 * last granules hold the globals' and the stack's first and last bytes, and are marked only
 * around the reload loop, which keeps everything it needs in registers.
 *
 * The SystemC model has no revocation load filter (eg2_mpact_cpu.h "Not modelled"): with
 * `model_gaps` the revoked capabilities' tags are not checked (the others are).
 *
 * Built with EG2_REVOCATION_INJECT_FAIL, the first marked granule's bitmap bit is expected clear: the
 * run must fail with `MISMATCH bitmap bit #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define REVOCATION_PARAMS(X)   \
  X(n_marked, 0, 0, 64)        \
  X(granule_class, 0, 0, 4)    \
  X(outside_base, 9, 0, 9)     \
  X(seal_pct, 101, 0, 101)     \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOCATION_PARAMS);

enum {
  kMaxMarked = 64,
  kMaxCaps = 2 * kMaxMarked + 8,
  // sec SRAM work area: the capability slots, then one word per slot (expected tag, kind), then
  // the marked granules.
  kSlots = kEg2WorkBase,
  kExp = kSlots + 8 * kMaxCaps,
  kMarks = kExp + 4 * kMaxCaps,
  // Slot kinds.
  kKindPlain = 0,
  kKindSealed = 1,
  kKindSealing = 2,
  kKindNeighbour = 3,
  kKindOutside = 4,
};

static eg2_tally_t tally;

static uint32_t interior_granule(void) {
  // sec SRAM's arenas and the main SRAM arena: memory only the test uses.
  if (eg2_pick(2) == 0 || eg2_main_arena() == 0) {
    return kEg2SecBase + 8 * eg2_rand_range(1, kEg2SecSize / 8 - 2);
  }
  return eg2_main_arena() + 8 * eg2_rand_range(0, kEg2MainArenaSize / 8 - 1);
}

static uint32_t pick_granule(uint32_t cls) {
  if (cls == 0) {
    uint32_t d = eg2_rand_range(0, 99);
    cls = d < 20 ? 1 : d < 40 ? 2 : d < 60 ? 3 : 4;
  }
  switch (cls) {
    case 1:
      return kEg2MsHeapBase;
    case 2:
      return kEg2MsHeapTop - 8;
    case 3:
      return eg2_pick(2) ? kEg2SecBase : kEg2SecBase - 8;
    default:
      return interior_granule();
  }
}

static bool is_marked(uint32_t g, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    if (eg2_ms_read32(kMarks + 4 * i) == g) {
      return true;
    }
  }
  return false;
}

/** A sealing capability (permit-seal from the sealing root) based at `base`. */
static void *sealing_cap_at(uint32_t base, void *seal_root) {
  void *c = seal_root;
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(base));
  __asm__ volatile("csetbounds %0, %0, %1" : "+C"(c) : "r"(8u));
  return c;
}

/**
 * The reload loop, with the marks set: CLC of every slot, tags into `tags` (kept in sec SRAM
 * words, reached through one capability held in a register).
 */
static OT_NOINLINE void reload(uint32_t n, volatile uint32_t *tags) {
  void *a = eg2_mem(kSlots, 8 * n);
  for (uint32_t i = 0; i < n; ++i) {
    void *c;
    __asm__ volatile("clc %0, 0(%1)" : "=C"(c) : "C"(a) : "memory");
    uint32_t t;
    __asm__ volatile("cgettag %0, %1" : "=r"(t) : "C"(c));
    tags[i] = t;
    __asm__ volatile("cincoffsetimm %0, %0, 8" : "+C"(a));
  }
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    eg2_puts("EG2 no main SRAM arena below the stack\r\n");
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n_marked = EG2_PARAM(n_marked) != 0 ? EG2_PARAM(n_marked) : eg2_rand_range(1, 64);
  uint32_t outside = EG2_PARAM(outside_base) == 9 ? eg2_rand_range(0, 8) : EG2_PARAM(outside_base);
  uint32_t seal_pct = EG2_PARAM(seal_pct) == 101 ? eg2_rand_range(0, 30) : EG2_PARAM(seal_pct);
  eg2_put_kv("EG2 revocation n_marked", n_marked);
  eg2_put_kv(" outside_base", outside);
  eg2_put_kv(" seal_pct", seal_pct);
  eg2_puts("\r\n");
  void *seal_root = eg2_ms_sealing_cap();
  eg2_ms_revbm_clear();

  // The marked granules: the classes' directed ones first (the window's first and last granule,
  // both sides of the boundary), then drawn.
  uint32_t n = 0;
  static const uint32_t kDirected[] = {kEg2MsHeapBase, kEg2MsHeapTop - 8, kEg2SecBase - 8,
                                       kEg2SecBase};
  for (uint32_t i = 0; i < n_marked; ++i) {
    uint32_t g = i < 4 && EG2_PARAM(granule_class) == 0 ? kDirected[i]
                                                         : pick_granule(EG2_PARAM(granule_class));
    if (!is_marked(g, n)) {
      eg2_ms_write32(kMarks + 4 * n++, g);
    }
  }
  n_marked = n;

  // The capabilities: one based in each marked granule, one in a neighbour that is not marked,
  // and the outside ones.
  uint32_t n_caps = 0;
  for (uint32_t i = 0; i < n_marked; ++i) {
    uint32_t g = eg2_ms_read32(kMarks + 4 * i);
    uint32_t kind = kKindPlain;
    void *c;
    if (eg2_rand_range(0, 99) < seal_pct) {
      kind = eg2_pick(2) ? kKindSealed : kKindSealing;
    }
    if (kind == kKindSealing) {
      c = sealing_cap_at(g, seal_root);
    } else {
      c = eg2_ms_object_cap(g + eg2_pick(8), eg2_rand_range(1, 64), kind == kKindSealed);
    }
    eg2_ms_csc(kSlots + 8 * n_caps, c);
    eg2_ms_write32(kExp + 4 * n_caps, kind);
    ++n_caps;
    uint32_t nb = eg2_pick(2) ? g + 8 : g - 8;
    if (nb >= kEg2MsHeapBase && nb < kEg2MsHeapTop && !is_marked(nb, n_marked)) {
      eg2_ms_csc(kSlots + 8 * n_caps, eg2_ms_object_cap(nb + eg2_pick(8), 8, false));
      eg2_ms_write32(kExp + 4 * n_caps, kKindNeighbour);
      ++n_caps;
    }
  }
  for (uint32_t i = 0; i < outside; ++i) {
    uint32_t base = (i & 1) ? kEg2MsHeapTop + 8 * eg2_rand_range(0, 64)
                            : kEg2MsHeapBase - 8 * eg2_rand_range(1, 64);
    eg2_ms_csc(kSlots + 8 * n_caps, eg2_ms_object_cap(base, 8, false));
    eg2_ms_write32(kExp + 4 * n_caps, kKindOutside);
    ++n_caps;
  }
  // The outside ones' neighbours in the bitmap: the window's first and last granules are marked
  // whatever the draw, so that a filter that wrapped an outside base into the bitmap would revoke
  // them.
  bool edge_marks = outside != 0;

  // Mark, read the bitmap back, reload, unmark.
  for (uint32_t i = 0; i < n_marked; ++i) {
    eg2_ms_revbm_mark(eg2_ms_read32(kMarks + 4 * i), true);
  }
  if (edge_marks) {
    eg2_ms_revbm_mark(kEg2MsHeapBase, true);
    eg2_ms_revbm_mark(kEg2MsHeapTop - 8, true);
  }
  for (uint32_t i = 0; i < n_marked; ++i) {
    uint32_t w, b;
    (void)eg2_ms_revbm_bit(eg2_ms_read32(kMarks + 4 * i), &w, &b);
    uint32_t exp_bit = 1;
#ifdef EG2_REVOCATION_INJECT_FAIL
    exp_bit = i == 0 ? 0 : 1;
#endif
    eg2_check(&tally, "bitmap bit", i, (eg2_ms_revbm_read(w) >> b) & 1, exp_bit);
  }
  volatile uint32_t *tags = eg2_mem(kMarks + 4 * kMaxMarked, 4 * kMaxCaps);
  reload(n_caps, tags);
  eg2_ms_revbm_clear();

  // The expected tags. The window's first and last granules were marked for the outside ones' sake
  // too: a capability based there is revoked as well.
  for (uint32_t i = 0; i < n_caps; ++i) {
    ++tally.planned;
    uint32_t kind = eg2_ms_read32(kExp + 4 * i);
    void *c = eg2_ms_clc(kSlots + 8 * i);
    uint32_t base = eg2_ms_cap_base(c);
    uint32_t w, b;
    bool marked = eg2_ms_revbm_bit(base, &w, &b) &&
                  (is_marked(base & ~7u, n_marked) ||
                   (edge_marks && ((base & ~7u) == kEg2MsHeapBase ||
                                   (base & ~7u) == kEg2MsHeapTop - 8)));
    bool revoked = marked && kind != kKindSealing;
    uint32_t exp = revoked ? 0 : 1;
    if (!(gaps && revoked)) {
      if (!eg2_check(&tally, "tag", i, tags[i], exp)) {
        eg2_put_kv("  kind", kind);
        eg2_put_kv(" base", base);
        eg2_puts("\r\n");
      }
    }
    ++tally.done;
  }
  eg2_tally_print("revocation", &tally);
  return eg2_tally_ok(&tally);
}
