// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_bounds (EG2 spec "Revocation", "Corner cases"; REQ_CMS_05, REQ_CMS_11,
 * REQ_CIB_04). Ports revocation_base.
 *
 * Capabilities based at the revocable window's edges and around it, each stored in sec SRAM's
 * arena B and loaded twice by one stub (eg2_revoke.S): a CLC straight after the bitmap bit of its
 * base's granule is set, and one straight after it is cleared again. For a base outside the window
 * the bit set is that of its alias, the granule a filter that wrapped the base into the bitmap would
 * read (the 12-bit bitmap byte address: modulo 256 KiB; for 0x1003_0000 modulo the window, the
 * window's first granule). Cases:
 *  - bases 0x1000_0000 (the window's first granule), 0x1001_FFF8 and 0x1002_0000 (main meets sec
 *    SRAM), 0x1002_FFF8 (the last), 0x1003_0000 (the first outside); at address 0, in ROM, in RRAM
 *    and in MMIO;
 *  - base and address in different granules, both ways round (the base's granule marked, the
 *    address's not, and the other way);
 *  - a zero-length capability one past the end of a two-granule object whose granules are marked;
 *  - a sealed memory capability (type 9) and a forward sentry (type 1) based in a marked granule
 *    (revoked), a sealing capability (SE) based in one (not revoked);
 *  - an untagged capability based in a marked granule;
 *  - a misaligned CLC, a CLC outside its authority's bounds and one through an authority without
 *    LD: each traps (mcause 4; capability bounds and load-permission violations on ca0).
 *
 * Check, the expression (eg2_revoke.h) for each load's tag; the verdict at each edge as REQ_CMS_05's
 * mapping gives (bit b of word w covers 0x1000_0000 + (w * 32 + b) * 8); the zero-length capability
 * not revoked by its neighbour's bits. The bench counts each case's requests: two data requests per
 * CLC to the slot, and bitmap reads of the word the case marked: one per CLC of a tagged, non-sealing
 * capability based in the window, none for an untagged one or one based outside (REQ: "a bitmap
 * read only for tagged capabilities based in the revocable range"; for a sealing capability in the
 * window none or one is accepted, the spec does not say). The trapping CLCs make no data request and
 * no bitmap read.
 *
 * Only the stub runs while a bit is set, so a mark at the window's first granule (the image's
 * globals) cannot strip a capability the test holds.
 *
 * `model_gaps` (eg2sim): no load filter and no bench: the revoked tags and the counts are skipped.
 *
 * Built with EG2_REVOCATION_BOUNDS_INJECT_FAIL, the first case's tag after the clear is expected 0:
 * the run must fail with `MISMATCH tag, bit clear #0`.
 */

#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVOCATION_BOUNDS_PARAMS(X) \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVOCATION_BOUNDS_PARAMS);

extern char eg2_rv_stub_mark_sw[];

enum {
  kSlot = kEg2ArenaB,
  kMcauseLoadMisaligned = 4,
};

static eg2_tally_t tally;
static bool gaps, bench;
static uint32_t idx;

/** The granule whose bit stands for `base`: its own, or the alias of a base outside the window. */
static uint32_t alias_of(uint32_t base) {
  if (eg2_rv_revocable(base)) {
    return base & ~7u;
  }
  uint32_t a = kEg2MsHeapBase + ((base - kEg2MsHeapBase) & 0x3ffffu);
  if (a >= kEg2MsHeapTop) {
    a = kEg2MsHeapBase + (a - kEg2MsHeapTop);
  }
  return a & ~7u;
}

/**
 * One case: `v` stored in the slot, `mark` (a granule, or two consecutive ones) set by the stub's
 * word write; loaded under the mark and after the clear.
 */
static void run_case(const char *what, void *v, uint32_t mark, uint32_t n_granules) {
  uint32_t i = idx++;
  ++tally.planned;
  eg2_ms_csc(kSlot, v);
  eg2_cap_t mem = eg2_hw_read(eg2_ms_clc(kSlot));
  uint32_t w, b;
  if (!eg2_ms_revbm_bit(mark, &w, &b)) {
    eg2_check(&tally, "mark in the bitmap", i, 0, 1);
    return;
  }
  uint32_t mask = (n_granules == 2 ? 3u : 1u) << b;
  uint32_t base = eg2_cap_base(mem);
  bool revbm = (base & ~7u) >= mark && (base & ~7u) < mark + 8 * n_granules;
  if (!eg2_rv_revocable(base)) {
    revbm = alias_of(base) >= mark && alias_of(base) < mark + 8 * n_granules;
  }
  eg2_cap_t auth = eg2_hw_read(eg2_ms_auth(kSlot));
  eg2_cap_t e_mark = eg2_rv_expect(auth, mem, revbm);
  eg2_cap_t e_clear = eg2_rv_expect(auth, mem, false);

  (void)eg2_rv_count_revbm(mark, mark + 8 * n_granules, bench);
  (void)eg2_rv_count_begin(kSlot, kSlot + 8, bench);
  bool trapped = eg2_probe_call(eg2_mem(kEg2MsMetaBase + 4 * w, 4), (void *)(uintptr_t)mask,
                                eg2_mem(kSlot, 8), eg2_rv_stub_mark_sw);
  uint32_t t_mark = eg2_trap_rec.value2, t_clear = eg2_trap_rec.value;
  eg2_rv_counts_t cnt;
  (void)eg2_rv_count_end(&cnt, bench);

  uint32_t failed = tally.failed;
  eg2_check(&tally, "stub trapped", i, trapped, 0);
  if (!(gaps && e_clear.tag && !e_mark.tag)) {
    eg2_check(&tally, "tag, bit set", i, t_mark, e_mark.tag);
  }
  uint32_t exp_clear = e_clear.tag;
#ifdef EG2_REVOCATION_BOUNDS_INJECT_FAIL
  if (i == 0) {
    exp_clear = 0;
  }
#endif
  eg2_check(&tally, "tag, bit clear", i, t_clear, exp_clear);
  if (cnt.valid) {
    eg2_check(&tally, "data requests", i, cnt.core, 4);
    eg2_check(&tally, "crossbar data requests", i, cnt.xbar, 4);
    bool in_window = mem.tag && eg2_rv_revocable(base);
    if (in_window && eg2_rv_sealing(mem)) {
      // Not stated by the spec: none or one read per load.
      eg2_check(&tally, "bitmap reads (sealing, at most 2)", i, cnt.bitmap <= 2, 1);
    } else {
      eg2_check(&tally, "bitmap reads", i, cnt.bitmap, in_window ? 2 : 0);
    }
  }
  if (tally.failed != failed && tally.failed <= kEg2MaxReports) {
    eg2_puts("  case ");
    eg2_puts(what);
    eg2_put_kv(" base", base);
    eg2_put_kv(" mark", mark);
    eg2_puts("\r\n");
  }
  ++tally.done;
}

/** A CLC that must trap: no data request, no bitmap read. */
static void run_fault(const char *what, void *auth, uint32_t mcause, uint32_t mtval) {
  uint32_t i = idx++;
  ++tally.planned;
  (void)eg2_rv_count_revbm(kEg2ArenaA, kEg2ArenaA + 8, bench);
  (void)eg2_rv_count_begin(kSlot - 64, kSlot + 64, bench);
  bool trapped = eg2_probe_call(auth, NULL, NULL, eg2_stub_clc);
  uint32_t got_cause = eg2_trap_rec.mcause, got_tval = eg2_trap_rec.mtval;
  eg2_rv_counts_t cnt;
  (void)eg2_rv_count_end(&cnt, bench);
  uint32_t failed = tally.failed;
  eg2_check(&tally, "faulting CLC trapped", i, trapped, 1);
  eg2_check(&tally, "faulting CLC mcause", i, got_cause, mcause);
  if (mcause == kEg2McauseCheri) {
    eg2_check(&tally, "faulting CLC mtval", i, got_tval, mtval);
  }
  if (cnt.valid) {
    eg2_check(&tally, "faulting CLC data requests", i, cnt.core, 0);
    eg2_check(&tally, "faulting CLC bitmap reads", i, cnt.bitmap, 0);
  }
  if (tally.failed != failed && tally.failed <= kEg2MaxReports) {
    eg2_puts("  fault ");
    eg2_puts(what);
    eg2_puts("\r\n");
  }
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  eg2_cp_save_sealing_root();
  eg2_ms_revbm_clear();
  eg2_probe_begin();

  // The window's edges.
  static const uint32_t kEdges[] = {0x10000000u, 0x1001fff8u, 0x10020000u, 0x1002fff8u,
                                    0x10030000u};
  for (uint32_t k = 0; k < ARRAYSIZE(kEdges); ++k) {
    run_case("edge", eg2_mem(kEdges[k], 8), alias_of(kEdges[k]), 1);
  }
  // Outside: address 0, ROM, RRAM, MMIO (all alias to the window's first granule).
  static const uint32_t kOutside[] = {0x00000000u, 0x00040000u, 0x30000000u, 0x40000000u};
  for (uint32_t k = 0; k < ARRAYSIZE(kOutside); ++k) {
    run_case("outside", eg2_mem(kOutside[k], 8), alias_of(kOutside[k]), 1);
  }

  // Base and address in different granules, both ways round.
  uint32_t g = kEg2ArenaA + 8 * eg2_rand_range(8, 1000);
  void *c = eg2_mem(g, 64);
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(g + 24));
  run_case("base marked, address not", c, g, 1);
  c = eg2_mem(g, 64);
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(g + 24));
  run_case("address marked, base not", c, g + 24, 1);

  // A zero-length capability one past a freed two-granule object (both granules in one word).
  uint32_t obj = kEg2ArenaA + 256 * eg2_rand_range(1, 30) + 16 * eg2_rand_range(0, 14);
  run_case("zero length past a freed object", eg2_mem(obj + 16, 0), obj, 2);

  // Sealed, a sentry, a sealing capability, an untagged one.
  g = kEg2ArenaA + 8 * eg2_rand_range(8, 1000);
  run_case("sealed", eg2_ms_object_cap(g, 8, true), g, 1);
  {
    eg2_cp_t exe = eg2_cp_set_addr(eg2_cp_root_exe(), g);
    exe = eg2_cp_bounds(exe, 8, kEg2BoundsExact);
    eg2_cp_t s = eg2_cp_set_addr(eg2_cp_root_seal(), kEg2OtypeSentry);
    run_case("sentry", eg2_cp_seal(exe, s).hw, g, 1);
  }
  {
    eg2_cp_t s = eg2_cp_set_addr(eg2_cp_root_seal(), g);
    s = eg2_cp_bounds(s, 8, kEg2BoundsExact);
    s = eg2_cp_and_perm(s, kEg2PermGl | kEg2PermSe);
    run_case("sealing", s.hw, g, 1);
  }
  c = eg2_mem(g, 8);
  __asm__ volatile("ccleartag %0, %0" : "+C"(c));
  run_case("untagged", c, g, 1);

  // Faulting CLCs (ca0 is register 10).
  eg2_ms_csc(kSlot, eg2_mem(kEg2ArenaA, 8));
  void *a = eg2_mem(kSlot, 16);
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(a) : "r"(kSlot + 4));
  run_fault("misaligned", a, kEg2McauseLoadMisaligned, 0);
  run_fault("bounds", eg2_mem(kSlot, 4), kEg2McauseCheri, eg2_cap_mtval(10, kEg2CauseBounds));
  a = eg2_mem(kSlot, 8);
  __asm__ volatile("candperm %0, %0, %1" : "+C"(a) : "r"(kEg2PermAll & ~(uint32_t)kEg2PermLd));
  run_fault("no LD", a, kEg2McauseCheri, eg2_cap_mtval(10, kEg2CauseLd));

  eg2_probe_end();
  eg2_tally_print("revocation_bounds", &tally);
  return eg2_tally_ok(&tally);
}
