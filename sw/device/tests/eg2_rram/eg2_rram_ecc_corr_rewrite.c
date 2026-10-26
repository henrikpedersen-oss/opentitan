// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_ecc_corr_rewrite (EG2 verification spec REQ_RRAM_09): a
 * correctable error is corrected; corr1_err / corr2_err reach the PLIC;
 * CORRx_ERR_LOC and CORRx_ERR_CNT update; a Rewrite restores a clean word.
 *
 * Documents: RFC RRAM §4.6.1-4.6.2 p17-18; rram_ctrl theory_of_operation.md
 * "RRAM ECC Error Handling" (one error per half of the 128-bit word is
 * corrected; one corrected half is corr1, both halves corr2, the corrected data
 * is returned and the operation is not aborted, a corrected error is reported
 * once per RRAM read), programmers_guide.md "Correctable ECC Errors" (corr2:
 * check the page's checksum before the Rewrite), "Issuing a Rewrite Operation";
 * README "rram_macro" (two Hamming(72,64) codes, one per 64-bit half).
 *
 * Each of n_errors rounds:
 *   1. a page of the scratch region (RD, WR, ECC; SCR by `scramble`) is written
 * with the seed's pattern; with path = fetch the drawn RRAM word holds `addi
 * a0, a0, K ; c.jr ra`;
 *   2. `EG2 RRAM ecc off=0x<o> bits=<1|2>`: the vseq flips one data bit in one
 * 64-bit half (1) or one in each half (2) of that row by backdoor, without
 * touching the ECC bits;
 *   3. the word is read by a host load or fetched by a call (the word was never
 * read since the write, which invalidated the page's read-buffer entries;
 * fence.i empties the iCache): the value is the written one, the call returns
 * a0 + K;
 *   4. INTR_STATE has corr1_err (1 bit) or corr2_err (2 bits) and not the
 * other; the PLIC's pending bit for that source is set and a claim returns its
 * ID; CORRx_ERR_CNT went up by one; CORRx_ERR_LOC holds the RRAM word's address
 * (bits 20:4) and the data partition;
 *   5. corr2 only: the page read through the controller has the checksum of the
 * page as written (the miscorrection check of the programmer's guide);
 *   6. a Rewrite of the word ends without an error; `EG2 RRAM ecc rewritten
 * off=0x<o>`: the vseq checks the row is a clean codeword again (its ECC bits
 * are those of its data);
 *   7. the word read again gives the value and no new correctable error.
 *
 * Knobs:
 *   n_errors    rounds, 1-8
 *   w_bits_1, w_bits_2   weights of a single- and a double-bit (one per half)
 * error path        0 load, 1 fetch, 2 drawn per round loc_class   0 the first
 * RRAM word of a page, 1 the last, 2 random, 3 drawn per round scramble    the
 * region scrambled (1) or not (0) model_gaps  skips what the SystemC model does
 * not model:
 *
 * | Skipped with model_gaps = 1                       | Why |
 * |---------------------------------------------------|---------------------------------------|
 * | the interrupt, PLIC, LOC and CNT checks of step 4 | no bench injects, and
 * eg2sim's        | |   and the clean-row check of step 6               |
 * rram_ctrl models no ECC             | | |   (ip/eg2_mem.h): the model run
 * checks| |                                                   |   the data, the
 * fetch, the checksum   | |                                                   |
 * and the Rewrite's completion        |
 *
 * Built with EG2_RRAM_ECC_CORR_REWRITE_INJECT_FAIL the first value check
 * expects bit 0 flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/rv_plic_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define ECC_CORR_PARAMS(X) \
  X(n_errors, 4, 1, 8)     \
  X(w_bits_1, 50, 0, 100)  \
  X(w_bits_2, 50, 0, 100)  \
  X(path, 2, 0, 2)         \
  X(loc_class, 3, 0, 3)    \
  X(scramble, 1, 0, 1)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(ECC_CORR_PARAMS)

enum {
  kPolls = 200000,
  kPageWords = kEg2RramPageBytes / 4,
  kWait = 2000,  // cycles the software waits after a sync line, for the bench's
                 // backdoor access
  kCorr1 = 1u << RRAM_CTRL_INTR_STATE_CORR1_ERR_BIT,
  kCorr2 = 1u << RRAM_CTRL_INTR_STATE_CORR2_ERR_BIT,
};

static uint32_t page_buf[kPageWords];

static uint32_t checksum(const uint32_t *w, uint32_t n) {
  uint32_t s = 0x811c9dc5;
  for (uint32_t i = 0; i < n; ++i) {
    s = (s ^ w[i]) * 0x01000193;
  }
  return s;
}

/** PLIC: source `id` at priority 1, enabled for target 0. */
static void plic_arm(uint32_t id, bool on) {
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * id,
                  on ? 1 : 0);
  uint32_t ie_off = RV_PLIC_IE0_0_REG_OFFSET + 4 * (id / 32);
  uint32_t ie = eg2_dev_read32(kEg2DevRvPlic, ie_off);
  ie = on ? ie | (1u << (id % 32)) : ie & ~(1u << (id % 32));
  eg2_dev_write32(kEg2DevRvPlic, ie_off, ie);
}

// Clear the controller's corr interrupts and complete every claim the PLIC still holds. A
// Rewrite reads the corrupted word, which reports it again (theory_of_operation.md: a corrected
// error is reported once per RRAM read of the word); the PLIC keeps that interrupt pending after
// INTR_STATE is cleared, and the next round's claim then returned it, the lower ID first: claim
// 167 (corr1, stale) where 168 (corr2) was expected (2026-10-26).
static void clear_intrs_and_plic(void) {
  eg2_rram_clear_intrs();
  for (int i = 0; i < 8; ++i) {
    uint32_t c = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
    if (c == 0) {
      break;
    }
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, c);
  }
}

static bool plic_pending(uint32_t id) {
  return (eg2_dev_read32(kEg2DevRvPlic,
                         RV_PLIC_IP_0_REG_OFFSET + 4 * (id / 32)) >>
          (id % 32)) &
         1;
}

static bool round_(uint32_t idx, bool gaps) {
  bool ok = true;
  const uint32_t wb[2] = {EG2_PARAM(w_bits_1), EG2_PARAM(w_bits_2)};
  uint32_t bits = eg2_chk_pick(wb, 2) == 1 ? 2 : 1;
  uint32_t path = EG2_PARAM(path) > 1 ? eg2_rand_range(0, 1) : EG2_PARAM(path);
  uint32_t loc =
      EG2_PARAM(loc_class) > 2 ? eg2_rand_range(0, 2) : EG2_PARAM(loc_class);
  uint32_t page = kEg2RramScratchFirstPage + idx;
  uint32_t rwords = kEg2RramPageBytes / kEg2RramWordBytes;
  uint32_t rword = loc == 0   ? 0
                   : loc == 1 ? rwords - 1
                              : eg2_rand_range(0, rwords - 1);
  uint32_t woff =
      eg2_rram_addr(page, 4 * rword);  // the RRAM word's byte offset
  int32_t k = (int32_t)eg2_rand_range(1, 2000);

  for (uint32_t i = 0; i < kPageWords; ++i) {
    page_buf[i] = eg2_rram_pattern(0xecc0 + idx, i);
  }
  if (path == 1) {
    page_buf[4 * rword] = eg2_rram_insn_addi_a0(k);
    page_buf[4 * rword + 1] = kEg2ChkStubRetNop;
  }
  uint32_t sum = checksum(page_buf, kPageWords);
  eg2_rram_result_t r;
  ok = eg2_chk_true("page write done",
                    eg2_rram_write(kEg2RramPartData, eg2_rram_addr(page, 0),
                                   page_buf, kPageWords, &r)) &&
       ok;
  ok = eg2_chk_eq("page write error code", 0, r.err_code) && ok;
  eg2_rram_reg_wr(RRAM_CTRL_CORR1_ERR_CNT_REG_OFFSET, 0);
  eg2_rram_reg_wr(RRAM_CTRL_CORR2_ERR_CNT_REG_OFFSET, 0);
  clear_intrs_and_plic();
  eg2_chk_step("round", idx);
  eg2_chk_step("word offset", woff);
  eg2_chk_step("bits", bits);
  eg2_chk_step("path (0 load, 1 fetch)", path);

  LOG_INFO("EG2 RRAM ecc off=0x%x bits=%d", woff, (int)bits);
  eg2_chk_delay(kWait);
  eg2_chk_fence_i();

  uint32_t lane = eg2_rand_range(0, 3);
  if (path == 1) {
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_call(kEg2RramHostBase + woff, 7, &t);
    ok = eg2_chk_eq("corrected fetch: no trap", 0, cause) && ok;
    ok =
        eg2_chk_eq("corrected fetch: the code ran", 7 + (uint32_t)k, t.value) &&
        ok;
    lane = 2 + (lane & 1);  // the data after the code: the same RRAM word
  } else {
    ok = eg2_chk_eq("corrected load", page_buf[4 * rword + lane],
                    *eg2_rram_host(woff + 4 * lane)) &&
         ok;
  }

  uint32_t want = bits == 1 ? kCorr1 : kCorr2;
  uint32_t other = bits == 1 ? kCorr2 : kCorr1;
  uint32_t plic_id = bits == 1 ? kTopEarlgreyPlicIrqIdRramCtrlCorr1Err
                               : kTopEarlgreyPlicIrqIdRramCtrlCorr2Err;
  if (gaps) {
    eg2_chk_skip(
        "corr interrupt, PLIC, CORRx_ERR_LOC/CNT (no ECC in the model)");
  } else {
    uint32_t st = eg2_rram_reg(RRAM_CTRL_INTR_STATE_REG_OFFSET);
    ok = eg2_chk_eq("corr interrupt set", want, st & want) && ok;
    ok = eg2_chk_eq("the other corr interrupt clear", 0, st & other) && ok;
    ok = eg2_chk_true("the PLIC's pending bit", plic_pending(plic_id)) && ok;
    uint32_t claim = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
    ok = eg2_chk_eq("PLIC claim", plic_id, claim) && ok;
    uint32_t cnt = eg2_rram_reg(bits == 1 ? RRAM_CTRL_CORR1_ERR_CNT_REG_OFFSET
                                          : RRAM_CTRL_CORR2_ERR_CNT_REG_OFFSET);
    ok = eg2_chk_eq("CORRx_ERR_CNT", 1, cnt) && ok;
    uint32_t loc_reg =
        eg2_rram_reg(bits == 1 ? RRAM_CTRL_CORR1_ERR_LOC_REG_OFFSET
                               : RRAM_CTRL_CORR2_ERR_LOC_REG_OFFSET);
    ok = eg2_chk_eq("CORRx_ERR_LOC RRAM word", woff >> 4,
                    (loc_reg & 0x1fffff) >> 4) &&
         ok;
    ok = eg2_chk_eq("CORRx_ERR_LOC partition (data)", 0, (loc_reg >> 24) & 1) &&
         ok;
    eg2_rram_reg_wr(RRAM_CTRL_INTR_STATE_REG_OFFSET, want | other);
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, claim);
  }

  if (bits == 2) {
    // The programmer's guide: check the page before rewriting a double-bit
    // correction.
    ok = eg2_chk_true("page read done",
                      eg2_rram_read(kEg2RramPartData, eg2_rram_addr(page, 0),
                                    page_buf, kPageWords, &r)) &&
         ok;
    ok = eg2_chk_eq("page checksum after the correction", sum,
                    checksum(page_buf, kPageWords)) &&
         ok;
    eg2_rram_reg_wr(RRAM_CTRL_INTR_STATE_REG_OFFSET, kCorr1 | kCorr2);
    if (!gaps) {
      // The page read met the corrected word again: acknowledge its PLIC claim.
      uint32_t c = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
      if (c != 0) {
        eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, c);
      }
    }
  }
  ok = eg2_chk_true("rewrite done",
                    eg2_rram_rewrite(kEg2RramPartData, woff, &r)) &&
       ok;
  ok = eg2_chk_eq("rewrite error code", 0, r.err_code) && ok;
  clear_intrs_and_plic();
  LOG_INFO("EG2 RRAM ecc rewritten off=0x%x", woff);
  eg2_chk_delay(kWait);
  // The rewrite invalidated the page's read-buffer entries: this read comes
  // from the macro.
  ok = eg2_chk_eq("word after the rewrite", page_buf[4 * rword + 3],
                  *eg2_rram_host(woff + 12)) &&
       ok;
  if (!gaps) {
    ok = eg2_chk_eq("no correctable error after the rewrite", 0,
                    eg2_rram_reg(RRAM_CTRL_INTR_STATE_REG_OFFSET) &
                        (kCorr1 | kCorr2)) &&
         ok;
  }
  clear_intrs_and_plic();
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls));
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t n = EG2_PARAM(n_errors);
  eg2_rram_region_set(region, kEg2RramScratchFirstPage, n,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                          (EG2_PARAM(scramble) ? kEg2RramAttrScr : 0));
  eg2_rram_reg_wr(RRAM_CTRL_INTR_ENABLE_REG_OFFSET, kCorr1 | kCorr2);
  plic_arm(kTopEarlgreyPlicIrqIdRramCtrlCorr1Err, true);
  plic_arm(kTopEarlgreyPlicIrqIdRramCtrlCorr2Err, true);
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_THRESHOLD0_REG_OFFSET, 0);
  for (uint32_t i = 0; i < n; ++i) {
    ok = round_(i, gaps) && ok;
  }
  plic_arm(kTopEarlgreyPlicIrqIdRramCtrlCorr1Err, false);
  plic_arm(kTopEarlgreyPlicIrqIdRramCtrlCorr2Err, false);
  eg2_rram_reg_wr(RRAM_CTRL_INTR_ENABLE_REG_OFFSET, 0);
  eg2_rram_region_off(region);
  return eg2_chk_summary(5 * n) && ok;
}
