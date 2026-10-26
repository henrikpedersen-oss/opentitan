// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_write_then_fetch (EG2 verification spec REQ_RRAM_05, "RRAM controller in RISC-V
 * mode"): after a controller write to a page, host reads of that page return the new data, also
 * when the old data were in the read buffer; reads of other pages are unchanged.
 *
 * Expected behaviour: rram_ctrl theory of operation "RRAM Read Buffer": "Any read-buffer entry
 * matching the written page is invalidated, so subsequent reads re-fetch and re-descramble from
 * the updated macro contents" (REQ_RRAM_05 is derived from this; decisions.md item 10 asks the RRAM
 * RFC owner to state it). Each round:
 *   1. code A (a0 + K1) is written to page P, called twice and its words loaded: now in the read
 *      buffer (and in the iCache when it is on);
 *   2. the controller writes `write_words` words to page Q at the drawn distance from P (the same
 *      page: over code A, as code a0 + K2 followed by data; an adjacent or a far page: new code and
 *      data there), then `fence.i` (RISC-V: the iCache is not coherent with stores);
 *   3. code at P returns a0 + K2 when Q = P, a0 + K1 otherwise; the code and every data word
 *      written to Q read back as written.
 *
 * Knobs:
 *   n_rounds      rounds
 *   write_class   the write's length: 0 one RRAM word, 1 up to 8 RRAM words, 2 up to a page, 3
 *                 drawn log-uniform per round
 *   distance      the written page from the fetched one: 0 the same, 1 adjacent, 2 far, 3 drawn
 *   icache        0 as booted, 1 off, 2 on, 3 drawn per round
 *   scramble      the scratch region's scramble attribute: 0 off, 1 on, 2 drawn
 *   model_gaps    skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why                                              |
 * |-------------------------------------------|--------------------------------------------------|
 * | CHERIoT mode: the write over code already  | eg2sim's CHERIoT ISS (MPACT-CHERIoT) runs stale  |
 * |   run (distance 0); each round uses pages  |   code after the controller rewrites code it has |
 * |   not run before, adjacent; at most 32     |   run, fence.i notwithstanding (model finding)   |
 * |   rounds                                   |                                                  |
 *
 * Built with EG2_RRAM_WRITE_THEN_FETCH_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define WRITE_THEN_FETCH_PARAMS(X) \
  X(n_rounds, 8, 1, 64)            \
  X(write_class, 3, 0, 3)          \
  X(distance, 3, 0, 3)             \
  X(icache, 3, 0, 3)               \
  X(scramble, 2, 0, 2)             \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(WRITE_THEN_FETCH_PARAMS)

enum {
  kPolls = 200000,
  kPages = 64,
  kPageWords = kEg2RramPageBytes / 4,
};

static int32_t draw_imm(void) {
  return (int32_t)eg2_rand_range(0, 4095) - 2048;
}

/** A write length in bus words (a multiple of 4) for the class. */
static uint32_t draw_len(uint32_t cls) {
  switch (cls) {
    case 0:
      return 4;
    case 1:
      return 4 * eg2_rand_range(1, 8);
    case 2:
      return 4 * eg2_rand_range(1, kPageWords / 4);
    default: {
      // Log-uniform: 4 << e words, e in 0..5 (4..128 words).
      uint32_t n = 4u << eg2_rand_range(0, 5);
      return n > kPageWords ? kPageWords : n;
    }
  }
}

/** Calls the code at `a` and checks it returns arg + k. */
static bool run_code(const char *what, uint32_t a, int32_t k) {
  uint32_t arg = eg2_rand();
  eg2_chk_trap_t t;
  uint32_t cause = eg2_chk_call(kEg2RramHostBase + a, arg, &t);
  bool ok = eg2_chk_eq(what, 0, cause);
  return eg2_chk_eq(what, arg + (uint32_t)k, t.value) && ok;
}

static bool write_ok(const char *what, uint32_t a, const uint32_t *w,
                     uint32_t n) {
  eg2_rram_result_t r;
  bool ok = eg2_chk_true(what, eg2_rram_write(kEg2RramPartData, a, w, n, &r));
  return eg2_chk_eq(what, 1, r.op_status) && ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t first = eg2_rand_range(kEg2RramScratchFirstPage,
                                  kEg2RramScratchLastPage - kPages + 1);
  uint32_t scr =
      EG2_PARAM(scramble) == 2 ? eg2_rand_range(0, 1) : EG2_PARAM(scramble);
  eg2_rram_region_set(region, first, kPages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                          (scr ? kEg2RramAttrScr : 0));
  bool icache_boot = eg2_rram_icache_set(true);
  eg2_rram_icache_set(icache_boot);

  bool ok = true;
  uint32_t rounds = 0;
  uint32_t buf[kPageWords];
  uint32_t n_rounds = EG2_PARAM(n_rounds);
  bool fresh_pages = false;
#ifdef __CHERIOT__
  if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip(
        "CHERIoT: a write over code already run (stale ISS code); adjacent "
        "pages only");
    fresh_pages = true;
    n_rounds = n_rounds > kPages / 2 ? kPages / 2 : n_rounds;
  }
#endif
  for (uint32_t i = 0; i < n_rounds; ++i) {
    uint32_t ic =
        EG2_PARAM(icache) == 3 ? eg2_rand_range(1, 2) : EG2_PARAM(icache);
    if (ic != 0) {
      eg2_rram_icache_set(ic == 2);
    }
    uint32_t d =
        EG2_PARAM(distance) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(distance);
    uint32_t len = draw_len(EG2_PARAM(write_class));
    // Code A in page P, 16-byte aligned, with room for the write after it when
    // Q = P.
    uint32_t p = first + eg2_rand_range(0, kPages - 1);
    if (fresh_pages) {
      // The model's CHERIoT ISS: no page run before is written again (see
      // model_gaps).
      d = 1;
      p = first + 2 * i;
    }
    uint32_t a = p * kEg2RramPageBytes +
                 4 * (4 * eg2_rand_range(0, (kPageWords - len) / 4));
    uint32_t q = d == 0   ? p
                 : d == 1 ? (p + 1 < first + kPages ? p + 1 : p - 1)
                          : first + (p - first + kPages / 2) % kPages;
    uint32_t b = a;
    if (d != 0) {
      b = q * kEg2RramPageBytes +
          4 * (4 * eg2_rand_range(0, (kPageWords - len) / 4));
    }
    eg2_chk_step("code A at", kEg2RramHostBase + a);
    eg2_chk_step("write at", kEg2RramHostBase + b);
    eg2_chk_step("write words", len);

    int32_t k1 = draw_imm();
    int32_t k2 = draw_imm();
    if (k2 == k1) {
      k2 = k1 ^ 1;
    }
    uint32_t insn = eg2_rram_insn_addi_a0(k1);
    eg2_rram_code_block(buf, 4, 0, &insn, 1);
    ok = write_ok("code A written", a, buf, 4) && ok;
    eg2_chk_fence_i();
    ok = run_code("code A (miss)", a, k1) && ok;
    ok = run_code("code A (hit)", a, k1) && ok;
    for (uint32_t j = 0; j < 4; ++j) {
      ok = eg2_chk_eq("code A word load", buf[j], *eg2_rram_host(a + 4 * j)) &&
           ok;
    }

    // The write: code (a0 + K2) in its first RRAM word, data after it.
    insn = eg2_rram_insn_addi_a0(k2);
    eg2_rram_code_block(buf, 4, 0, &insn, 1);
    for (uint32_t j = 4; j < len; ++j) {
      buf[j] = eg2_rand();
    }
    ok = write_ok("write done", b, buf, len) && ok;
    eg2_chk_fence_i();
    ok = run_code(d == 0 ? "code A after the write over it"
                         : "code A after a write elsewhere",
                  a, d == 0 ? k2 : k1) &&
         ok;
    ok = run_code("written code", b, k2) && ok;
    for (uint32_t j = 0; j < len; ++j) {
      ok = eg2_chk_eq("written word load", buf[j], *eg2_rram_host(b + 4 * j)) &&
           ok;
    }
    ++rounds;
  }
  eg2_rram_icache_set(icache_boot);
  eg2_rram_region_off(region);
  ok = eg2_chk_eq("rounds done", n_rounds, rounds) && ok;
  return eg2_chk_summary(n_rounds * 10) && ok;
}
