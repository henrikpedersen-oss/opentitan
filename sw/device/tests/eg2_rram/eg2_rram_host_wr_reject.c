// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_host_wr_reject (EG2 verification spec REQ_RRAM_03, "RRAM controller in RISC-V
 * mode"): `host_tl` is read-only. Every core store to the RRAM host window gets `d_error`, which
 * the core takes as a store access fault (mcause 7, mtval the address; in CHERIoT mode through a
 * capability that permits the store, so the fault is the bus's and not a CHERI fault), and the
 * word reads back unchanged. The SBA half of the requirement (a debugger's write over SBA) is the
 * vseq's: after `EG2 RRAM sba` it writes over SBA to the same words, checks `sberror` and reads
 * them back, then the software reads them again.
 *
 * RFC RRAM §3.1 p6 ("host_tl ... read-only"); programmer's guide "Memory-Mapped Reads via
 * host_tl".
 *
 * Knobs:
 *   n_writes     host stores to make
 *   w_width_*    store width weights: byte, half, word
 *   w_addr_*     address class weights: the first word of a page, the last word of a page, a
 *                word inside a 16-byte RRAM word next to a word boundary, a random word; in the
 *                test's own image or in the scratch pages
 *   in_image     0: scratch pages only, 1: also the test image's own first page (its vectors)
 *   sba          1: the bench's SBA writes run (chip only; the model has no debug module)
 *   model_gaps   skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1      | Why                                                   |
 * |----------------------------------|-------------------------------------------------------|
 * | the stores and their fault and   | eg2sim's rram_ctrl.host is a plain memory: a store   |
 * |   unchanged-word checks          |   lands (model finding: the documents make host_tl    |
 * |                                  |   read-only); the model run checks the host and       |
 * |                                  |   controller reads of each drawn word agree instead   |
 *
 * Built with EG2_RRAM_HOST_WR_REJECT_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define HOST_WR_REJECT_PARAMS(X) \
  X(n_writes, 24, 1, 256)        \
  X(w_width_byte, 30, 0, 100)    \
  X(w_width_half, 30, 0, 100)    \
  X(w_width_word, 40, 0, 100)    \
  X(w_addr_first, 25, 0, 100)    \
  X(w_addr_last, 25, 0, 100)     \
  X(w_addr_edge, 25, 0, 100)     \
  X(w_addr_random, 25, 0, 100)   \
  X(in_image, 1, 0, 1)           \
  X(sba, 0, 0, 1)                \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(HOST_WR_REJECT_PARAMS)

// Set over SBA by the bench when its writes are done.
static volatile uint32_t sba_done;

/** A word address of the draw's class. */
static uint32_t draw_addr(void) {
  const uint32_t w[4] = {EG2_PARAM(w_addr_first), EG2_PARAM(w_addr_last),
                         EG2_PARAM(w_addr_edge), EG2_PARAM(w_addr_random)};
  uint32_t page = eg2_rand_range(kEg2RramScratchFirstPage, kEg2RramScratchLastPage);
  if (EG2_PARAM(in_image) && eg2_rand_range(0, 3) == 0) {
    page = 0;  // the image's own first page: the store must not reach it either
  }
  const uint32_t words_per_page = kEg2RramPageBytes / 4;
  switch (eg2_chk_pick(w, 4)) {
    case 0:
      return eg2_rram_addr(page, 0);
    case 1:
      return eg2_rram_addr(page, words_per_page - 1);
    case 2: {
      // The last bus word of an RRAM word, or the first of the next one.
      uint32_t rword = eg2_rand_range(0, kEg2RramPageBytes / kEg2RramWordBytes - 2);
      return eg2_rram_addr(page, rword * 4 + 3 + eg2_rand_range(0, 1));
    }
    default:
      return eg2_rram_addr(page, eg2_rand_range(0, words_per_page - 1));
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const uint32_t ww[3] = {EG2_PARAM(w_width_byte), EG2_PARAM(w_width_half),
                          EG2_PARAM(w_width_word)};
  static const uint32_t kOps[3] = {kEg2ChkSb, kEg2ChkSh, kEg2ChkSw};
  bool ok = true;
  uint32_t sba_addr = 0;
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  if (gaps) {
    eg2_chk_skip("host stores, their faults and the unchanged words (host window a plain memory)");
    if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(100000))) {
      return false;
    }
    // The scratch pages readable by the controller for the comparison.
    uint32_t region = eg2_rram_free_region();
    if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
      return false;
    }
    eg2_rram_region_set(region, kEg2RramScratchFirstPage,
                        kEg2RramScratchLastPage - kEg2RramScratchFirstPage + 1,
                        kEg2RramAttrRd | kEg2RramAttrEcc);
    for (uint32_t i = 0; i < EG2_PARAM(n_writes); ++i) {
      uint32_t addr = draw_addr();
      if (addr < kEg2RramScratchFirstPage * kEg2RramPageBytes) {
        continue;
      }
      uint32_t ctrl = 0;
      eg2_rram_result_t r;
      ok = eg2_chk_true("controller read done",
                        eg2_rram_read(kEg2RramPartData, addr, &ctrl, 1, &r)) && ok;
      ok = eg2_chk_eq("controller read and host read agree", *eg2_rram_host(addr), ctrl) && ok;
    }
    eg2_rram_region_off(region);
    return eg2_chk_summary(1) && ok;
  }
  for (uint32_t i = 0; i < EG2_PARAM(n_writes); ++i) {
    uint32_t addr = draw_addr();
    uint32_t width = eg2_chk_pick(ww, 3);
    if (width == 3) {
      width = 2;
    }
    // The byte or half inside the word, aligned to its width.
    uint32_t lane = width == 0 ? eg2_rand_range(0, 3) : width == 1 ? 2 * eg2_rand_range(0, 1) : 0;
    uint32_t before = *eg2_rram_host(addr);
    uint32_t wdata = eg2_rand();
    if (wdata == before) {
      wdata = ~before;
    }
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_access((volatile uint8_t *)eg2_rram_host(addr) + lane, kOps[width],
                                    wdata, &t);
    eg2_chk_step("store addr", kEg2RramHostBase + addr + lane);
    ok = eg2_chk_eq("store mcause (store access fault)", kEg2ChkCauseStoreFault, cause) && ok;
    ok = eg2_chk_eq("store mtval", kEg2RramHostBase + addr + lane, t.mtval) && ok;
    ok = eg2_chk_eq("word after the store", before, *eg2_rram_host(addr)) && ok;
    sba_addr = addr;
  }

  if (EG2_PARAM(sba)) {
    // The bench writes over SBA to the last word stored above (its address on the console and in
    // the DV log), then the word must still hold what it held.
    uint32_t before = *eg2_rram_host(sba_addr);
    eg2_chk_step("sba addr", kEg2RramHostBase + sba_addr);
    // The vseq waits for this line, takes the two addresses from it, makes its SBA writes to the
    // RRAM word and its read-backs, then writes 1 over SBA to `sba_done` in main SRAM.
    sba_done = 0;
    LOG_INFO("EG2 RRAM sba 0x%08x 0x%08x", kEg2RramHostBase + sba_addr,
             eg2_chk_addr_of(&sba_done));
    uint32_t polls = 0;
    while (sba_done == 0 && polls < 1000000) {
      ++polls;
    }
    ok = eg2_chk_eq("sba done flag", 1, sba_done) && ok;
    ok = eg2_chk_eq("word after the sba write", before, *eg2_rram_host(sba_addr)) && ok;
  }
  return eg2_chk_summary(3) && ok;
}
