// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_host_exec (EG2 verification spec REQ_RRAM_01, "RRAM controller in RISC-V mode"):
 * core fetches and data loads through `host_tl` return the stored word, with the instruction
 * cache on and off, including code that straddles two 16-byte RRAM words.
 *
 * The test writes code and data into scratch pages through the controller (programmer's guide
 * "Issuing a Controller Write": 16-byte aligned, whole RRAM words), then fetches and loads them
 * through the host window. The expected values are what was written: the code computes
 * a0 + K1 + K2 (two `addi` whose encodings come from the RISC-V ISA, the same in CHERIoT mode),
 * the data are the words written. A 32-bit instruction at byte 14 of a 16-byte RRAM word
 * straddles two RRAM words (RFC RRAM §4.5.2; the read buffer holds whole RRAM words). Every op
 * runs twice: the first fetch or load misses the read buffer, the second hits it. The scratch
 * pages' region has its own scramble setting, the same for the write and the reads (programmer's
 * guide "Scrambling Consistency").
 *
 * Knobs:
 *   n_ops            fetch and load operations
 *   w_fetch          weight of a fetch op (code written, called twice)
 *   w_load           weight of a load op (eight words written, loaded at every width)
 *   w_addr_first     address class weights: the first RRAM word of a page,
 *   w_addr_last        the last bytes of a page,
 *   w_addr_straddle    a 32-bit instruction across two RRAM words (loads: the words on each side
 *                      of the boundary first),
 *   w_addr_random      anywhere in the scratch pages
 *   scramble         0 off, 1 on, 2 drawn per run
 *   icache           0 as booted, 1 off, 2 on, 3 toggled before each op
 *   model_gaps       skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why                                              |
 * |-------------------------------------------|--------------------------------------------------|
 * | CHERIoT mode: a fetch op's block at the    | eg2sim's CHERIoT ISS (MPACT-CHERIoT) runs stale  |
 * |   first or last RRAM word of a page; each  |   code after the controller rewrites code it has |
 * |   fetch op gets a block never run before   |   run, fence.i notwithstanding (model finding)   |
 *
 * Built with EG2_RRAM_HOST_EXEC_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define HOST_EXEC_PARAMS(X)      \
  X(n_ops, 16, 1, 128)           \
  X(w_fetch, 50, 0, 100)         \
  X(w_load, 50, 0, 100)          \
  X(w_addr_first, 25, 0, 100)    \
  X(w_addr_last, 25, 0, 100)     \
  X(w_addr_straddle, 30, 0, 100) \
  X(w_addr_random, 20, 0, 100)   \
  X(scramble, 2, 0, 2)           \
  X(icache, 3, 0, 3)             \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(HOST_EXEC_PARAMS)

enum {
  kPolls = 200000,
  kPages = 8,       // the test's scratch pages
  kBlockWords = 8,  // two RRAM words per op
};

static uint32_t first_page;

/** A 16-byte aligned block of two RRAM words, by address class; *code_off where
 * code starts. */
static uint32_t draw_block(uint32_t cls, uint32_t *code_off) {
  uint32_t page = first_page + eg2_rand_range(0, kPages - 1);
  uint32_t base = page * kEg2RramPageBytes;
  switch (cls) {
    case 0:  // the first RRAM word of the page
      *code_off = 0;
      return base;
    case 1:  // the last bytes of the page: the code ends at the page's end
      *code_off = 2 * kBlockWords * 2 - 10;
      return base + kEg2RramPageBytes - 4 * kBlockWords;
    case 2:  // the first addi at byte 14: across the two RRAM words
      *code_off = 14;
      return base +
             kEg2RramWordBytes *
                 eg2_rand_range(0, kEg2RramPageBytes / kEg2RramWordBytes - 2);
    default:
      *code_off = 2 * eg2_rand_range(0, (4 * kBlockWords - 10) / 2);
      return base +
             kEg2RramWordBytes *
                 eg2_rand_range(0, kEg2RramPageBytes / kEg2RramWordBytes - 2);
  }
}

#ifdef __CHERIOT__
static uint32_t fresh_blocks;
#endif

static bool fetch_op(uint32_t cls) {
  uint32_t off;
  uint32_t a = draw_block(cls, &off);
#ifdef __CHERIOT__
  if (EG2_PARAM(model_gaps)) {
    // Never rewrite code the model's CHERIoT ISS has run: a block of its own
    // per fetch op, the code offset (the straddle) kept.
    a = first_page * kEg2RramPageBytes +
        4 * kBlockWords * (fresh_blocks++ % (kPages * 16));
  }
#endif
  int32_t k1 = (int32_t)eg2_rand_range(0, 4095) - 2048;
  int32_t k2 = (int32_t)eg2_rand_range(0, 4095) - 2048;
  uint32_t insns[2] = {eg2_rram_insn_addi_a0(k1), eg2_rram_insn_addi_a0(k2)};
  uint32_t words[kBlockWords];
  bool ok = eg2_chk_true(
      "code fits", eg2_rram_code_block(words, kBlockWords, off, insns, 2) != 0);
  eg2_rram_result_t r;
  ok = eg2_chk_true(
           "code write done",
           eg2_rram_write(kEg2RramPartData, a, words, kBlockWords, &r)) &&
       ok;
  ok = eg2_chk_eq("code write op_status", 1, r.op_status) && ok;
  eg2_chk_fence_i();
  uint32_t target = kEg2RramHostBase + a + off;
  eg2_chk_step("fetch at", target);
  uint32_t arg = eg2_rand();
  for (int pass = 0; pass < 2; ++pass) {
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_call(target, arg, &t);
    ok = eg2_chk_eq(pass == 0 ? "fetch mcause (miss)" : "fetch mcause (hit)", 0,
                    cause) &&
         ok;
    ok = eg2_chk_eq(pass == 0 ? "fetch result (miss)" : "fetch result (hit)",
                    arg + (uint32_t)k1 + (uint32_t)k2, t.value) &&
         ok;
  }
  return ok;
}

static bool load_op(uint32_t cls) {
  uint32_t off;
  uint32_t a = draw_block(cls, &off);
  uint32_t words[kBlockWords];
  for (uint32_t i = 0; i < kBlockWords; ++i) {
    words[i] = eg2_rand();
  }
  eg2_rram_result_t r;
  bool ok =
      eg2_chk_true("data write done",
                   eg2_rram_write(kEg2RramPartData, a, words, kBlockWords, &r));
  ok = eg2_chk_eq("data write op_status", 1, r.op_status) && ok;
  eg2_chk_step("load at", kEg2RramHostBase + a);
  // The straddle class loads the word on each side of the RRAM-word boundary
  // first.
  uint32_t order0 = cls == 2 ? 3 : 0;
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t j = 0; j < kBlockWords; ++j) {
      uint32_t i = (order0 + j) % kBlockWords;
      volatile uint32_t *p = eg2_rram_host(a + 4 * i);
      ok = eg2_chk_eq("word load", words[i], *p) && ok;
      uint32_t lane = eg2_rand_range(0, 3);
      uint32_t op = eg2_rand_range(0, 3);  // lb, lbu, lh, lhu
      eg2_chk_trap_t t;
      uint32_t exp;
      uint32_t cause;
      if (op < 2) {
        cause = eg2_chk_access((volatile uint8_t *)p + lane,
                               op == 0 ? kEg2ChkLb : kEg2ChkLbu, 0, &t);
        uint32_t b = (words[i] >> (8 * lane)) & 0xff;
        exp = op == 0 ? (uint32_t)(int32_t)(int8_t)b : b;
      } else {
        lane &= 2;
        cause = eg2_chk_access((volatile uint8_t *)p + lane,
                               op == 2 ? kEg2ChkLh : kEg2ChkLhu, 0, &t);
        uint32_t h = (words[i] >> (8 * lane)) & 0xffff;
        exp = op == 2 ? (uint32_t)(int32_t)(int16_t)h : h;
      }
      ok = eg2_chk_eq("sub-word load mcause", 0, cause) && ok;
      ok = eg2_chk_eq("sub-word load", exp, t.value) && ok;
    }
  }
  return ok;
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
#ifdef __CHERIOT__
  if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip(
        "CHERIoT: fetch blocks at a page's first or last RRAM word (stale ISS "
        "code)");
  }
#endif
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  first_page = eg2_rand_range(kEg2RramScratchFirstPage,
                              kEg2RramScratchLastPage - kPages + 1);
  uint32_t scr =
      EG2_PARAM(scramble) == 2 ? eg2_rand_range(0, 1) : EG2_PARAM(scramble);
  uint32_t attrs = kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                   (scr ? kEg2RramAttrScr : 0);
  eg2_rram_region_set(region, first_page, kPages, attrs);
  eg2_chk_step("scratch first page", first_page);
  eg2_chk_step("scratch attrs", attrs);

  bool icache_boot = eg2_rram_icache_set(true);
  eg2_rram_icache_set(icache_boot);
  const uint32_t wop[2] = {EG2_PARAM(w_fetch), EG2_PARAM(w_load)};
  const uint32_t wcls[4] = {EG2_PARAM(w_addr_first), EG2_PARAM(w_addr_last),
                            EG2_PARAM(w_addr_straddle),
                            EG2_PARAM(w_addr_random)};
  bool ok = true;
  uint32_t fetches = 0;
  uint32_t loads = 0;
  for (uint32_t i = 0; i < EG2_PARAM(n_ops); ++i) {
    switch (EG2_PARAM(icache)) {
      case 1:
        eg2_rram_icache_set(false);
        break;
      case 2:
        eg2_rram_icache_set(true);
        break;
      case 3:
        eg2_rram_icache_set(eg2_rand_range(0, 1) != 0);
        break;
      default:
        break;
    }
    uint32_t cls = eg2_chk_pick(wcls, 4);
    if (cls == 4) {
      cls = 3;
    }
    if (eg2_chk_pick(wop, 2) == 1) {
      ok = load_op(cls) && ok;
      ++loads;
    } else {
      ok = fetch_op(cls) && ok;
      ++fetches;
    }
  }
  eg2_rram_icache_set(icache_boot);
  eg2_rram_region_off(region);
  eg2_chk_step("fetch ops", fetches);
  eg2_chk_step("load ops", loads);
  ok = eg2_chk_eq("ops done", EG2_PARAM(n_ops), fetches + loads) && ok;
  return eg2_chk_summary(EG2_PARAM(n_ops)) && ok;
}
