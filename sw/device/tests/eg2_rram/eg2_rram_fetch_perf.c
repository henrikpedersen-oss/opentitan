// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_fetch_perf (EG2 verification spec REQ_RRAM_06, "RRAM controller in RISC-V mode"):
 * the read buffer hits in one cycle, and with 4 entries and at most 2 outstanding host reads the
 * verify (shadow-read) stalls of the RFC's access patterns A-D stay within a bound.
 *
 * The spec names CoreMark from RRAM as the stimulus; CoreMark has no sim_dv build in this tree, so
 * the test runs the RFC's own access patterns (RFC RRAM §4.5.3 p15) as code written to scratch
 * pages, timed with mcycle:
 *   linear    straight-line code over `n_words` RRAM words (sequential fetch: pattern D on the
 *             instruction side);
 *   branchy   a chain of jumps, one instruction pair per RRAM word, each word in a different
 *             64-byte line (pattern A: N requests to N different RRAM words);
 *   data A    one load from each of `n_words` RRAM words (pattern A on the data side);
 *   data C    two loads from each word (A0, A1, B0, B1, ...: pattern C);
 *   data D    all four bus words of each word (pattern D).
 * Each pattern runs twice: cold (every RRAM word a buffer miss) and warm (the second run), each at
 * its own address (nothing in RRAM is rewritten).
 *
 * The bound. RFC §4.5.3 assumes "each RRAM read access takes four cycles" and a four-entry buffer;
 * with the shadow read every new RRAM word costs at most two macro reads (the read and its verify)
 * before the entry it displaces is free again, plus the three pipeline stages of
 * theory_of_operation.md "RRAM Read Pipeline": 2 x 4 + 3 = 11 cycles per new word above a hit. The
 * RFC adds "a final and precise analysis can only be done with the exact timings of the RRAM", so
 * the per-word budget is a knob (`budget_word`, default 24: the 11 cycles, the crossbar and the
 * core's own load and fetch latency) and so is the cost of an access that hits (`budget_access`,
 * default 8). Bound = budget_access x accesses + budget_word x new RRAM words + the call's own
 * cost (measured with an empty routine). Not checkable here: the one-cycle hit itself (the core's
 * view includes the crossbar and the core); the bench's `eg2_rram_cg` samples it at the phy.
 *
 * Knobs:
 *   pattern         0 linear, 1 branchy, 2 data A, 3 data C, 4 data D, 5 every pattern
 *   n_words         RRAM words a pattern touches (4..64; the data patterns take fewer when the
 *                   loads' 12-bit offsets do not reach)
 *   icache          0 off during the timed runs (the RRAM pattern reaches the phy), 1 on
 *   budget_word     cycles allowed per new RRAM word
 *   budget_access   cycles allowed per access or instruction
 *   model_gaps      skips what the SystemC model does not model: nothing (its time is one cycle
 *                   per instruction, so the bound holds trivially there; the results are checked)
 *
 * Built with EG2_RRAM_FETCH_PERF_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define FETCH_PERF_PARAMS(X)   \
  X(pattern, 5, 0, 5)          \
  X(n_words, 16, 4, 64)        \
  X(icache, 0, 0, 1)           \
  X(budget_word, 24, 1, 1000)  \
  X(budget_access, 8, 1, 1000) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(FETCH_PERF_PARAMS)

enum {
  kPolls = 200000,
  kPages = 48,         // the empty routine, then 8 pages per pattern
  kMaxWords = 1024,    // bus words of generated code and data (4 KiB, in the
                       // scratch pages)
  kSalt = 0x50455246,  // "PERF"
};

/** jal x0, offset (offset even, within +-1 MiB). */
static uint32_t insn_jal0(int32_t off) {
  uint32_t o = (uint32_t)off;
  return ((o >> 20) & 1) << 31 | ((o >> 1) & 0x3ff) << 21 |
         ((o >> 11) & 1) << 20 | ((o >> 12) & 0xff) << 12 | 0x6f;
}
/** lw t0, off(a1) ; add a0, a0, t0. */
static uint32_t insn_lw_t0_a1(uint32_t off) {
  return (off & 0xfff) << 20 | 11u << 15 | 2u << 12 | 5u << 7 | 0x03;
}
enum {
  kInsnAddA0T0 = 0x00550533,  // add a0, a0, t0
  // auipc a1, 0: auipcc ca1, 0 in CHERIoT mode, PCC (the executable root, which
  // may load).
  kInsnAuipcA1 = 0x00000597,
};

typedef struct pattern_out {
  uint32_t words;       // bus words of the block
  uint32_t accesses;    // instructions or loads the bound counts
  uint32_t new_words;   // distinct RRAM words the run touches
  uint32_t expect_add;  // what the code adds to a0
} pattern_out_t;

/**
 * Generates pattern `p` into `w` (from byte offset 0 of a 16-byte aligned
 * block). The code always ends in c.jr ra and leaves a0 + expect_add.
 */
static bool generate(uint32_t p, uint32_t nw, volatile uint32_t *w,
                     pattern_out_t *o) {
  for (uint32_t i = 0; i < kMaxWords; ++i) {
    w[i] = 0;
  }
  o->expect_add = 0;
  switch (p) {
    case 0: {  // linear: 4 addi per RRAM word, then the return
      uint32_t n = 4 * nw - 1;
      for (uint32_t i = 0; i < n; ++i) {
        w[i] = eg2_rram_insn_addi_a0(1);
      }
      w[n] = 0x00018082;  // c.jr ra ; c.nop
      o->words = 4 * nw;
      o->accesses = n + 1;
      o->new_words = nw;
      o->expect_add = n;
      return true;
    }
    case 1: {  // branchy: addi ; jal to the next 64-byte line, nw times
      for (uint32_t i = 0; i < nw; ++i) {
        w[16 * i] = eg2_rram_insn_addi_a0(1);
        w[16 * i + 1] = i + 1 < nw ? insn_jal0(64 - 4) : 0x00018082;
      }
      o->words = 16 * nw;
      o->accesses = 2 * nw;
      o->new_words = nw;
      o->expect_add = nw;
      return true;
    }
    default: {  // data A / C / D: code first, then the data, one RRAM word (16
                // bytes) per item
      uint32_t per = p == 2 ? 1 : p == 3 ? 2 : 4;
      // The loads reach their data from auipc with a 12-bit offset: at most
      // 2047 bytes on. Fewer words if they do not fit (logged by the caller as
      // the words the pattern touched).
      while (nw > 1) {
        uint32_t cw = (2 * per * nw + 2 + 3) & ~3u;
        if (4 * (cw + 4 * nw) <= 2048) {
          break;
        }
        --nw;
      }
      uint32_t code_words =
          (2 * per * nw + 2 + 3) & ~3u;  // auipc, (lw, add) per access, ret
      uint32_t c = 0;
      w[c++] = kInsnAuipcA1;
      for (uint32_t i = 0; i < nw; ++i) {
        for (uint32_t j = 0; j < per; ++j) {
          uint32_t off = 4 * (code_words + 4 * i + j);
          w[c++] = insn_lw_t0_a1(off);
          w[c++] = kInsnAddA0T0;
          uint32_t v = eg2_rram_pattern(kSalt, 4 * i + j);
          w[code_words + 4 * i + j] = v;
          o->expect_add += v;
        }
      }
      w[c] = 0x00018082;
      o->words = code_words + 4 * nw;
      o->accesses = c + 1 + per * nw;
      // The code's own RRAM words are fetched too.
      o->new_words = nw + code_words / 4;
      return true;
    }
  }
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
  eg2_rram_region_set(region, first, kPages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  uint32_t base = first * kEg2RramPageBytes;
  bool icache_boot = eg2_rram_icache_set(true);

  // An empty routine: the call's own cost, warm.
  uint32_t ret_words[4] = {0x00018082, 0, 0, 0};
  eg2_rram_result_t r;
  bool ok =
      eg2_chk_true("empty routine written",
                   eg2_rram_write(kEg2RramPartData, base, ret_words, 4, &r));
  uint32_t code0 = base + kEg2RramPageBytes;
  eg2_chk_fence_i();
  eg2_rram_icache_set(EG2_PARAM(icache) != 0);
  eg2_chk_trap_t t;
  uint32_t overhead = 0xffffffffu;
  for (int i = 0; i < 3; ++i) {
    uint32_t c0 = eg2_chk_cycles();
    eg2_chk_call(kEg2RramHostBase + base, 0, &t);
    uint32_t c = eg2_chk_cycles() - c0;
    overhead = c < overhead ? c : overhead;
  }
  eg2_chk_step("call overhead cycles", overhead);

  uint32_t nw = EG2_PARAM(n_words);
  // The generated block, in sec SRAM (4 KiB would not fit the CHERIoT image's
  // globals).
  volatile uint32_t *buf =
      eg2_chk_ptr(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR);
  uint32_t runs = 0;
  for (uint32_t p = 0; p < 5; ++p) {
    if (EG2_PARAM(pattern) != 5 && EG2_PARAM(pattern) != p) {
      continue;
    }
    pattern_out_t o;
    // Each pattern at its own address: nothing in RRAM is rewritten (and the
    // ISSs' decode caches of the SystemC model never meet rewritten code).
    uint32_t code = code0 + p * 8 * kEg2RramPageBytes;
    generate(p, nw, buf, &o);
    eg2_rram_icache_set(true);
    ok = eg2_chk_true("pattern written",
                      eg2_rram_write(kEg2RramPartData, code,
                                     (const uint32_t *)buf, o.words, &r)) &&
         ok;
    ok = eg2_chk_eq("pattern write op_status", 1, r.op_status) && ok;
    eg2_rram_icache_set(EG2_PARAM(icache) != 0);
    eg2_chk_step("pattern", p);
    eg2_chk_step("RRAM words touched", o.new_words);
    for (int pass = 0; pass < 2; ++pass) {
      uint32_t arg = eg2_rand();
      uint32_t c0 = eg2_chk_cycles();
      uint32_t cause = eg2_chk_call(kEg2RramHostBase + code, arg, &t);
      uint32_t c = eg2_chk_cycles() - c0;
      ok = eg2_chk_eq("pattern mcause", 0, cause) && ok;
      ok = eg2_chk_eq("pattern result", arg + o.expect_add, t.value) && ok;
      uint32_t bound = overhead + EG2_PARAM(budget_access) * o.accesses +
                       EG2_PARAM(budget_word) * o.new_words;
      eg2_chk_step(pass == 0 ? "cycles cold" : "cycles warm", c);
      eg2_chk_step("bound", bound);
      ok = eg2_chk_true("cycles within the bound", c <= bound) && ok;
    }
    ++runs;
    // Leave the block with nothing for the next pattern to fetch by mistake.
    eg2_chk_fence_i();
  }
  eg2_rram_icache_set(icache_boot);
  eg2_rram_region_off(region);
  ok = eg2_chk_eq("patterns run", EG2_PARAM(pattern) == 5 ? 5 : 1, runs) && ok;
  return eg2_chk_summary(4 * runs) && ok;
}
