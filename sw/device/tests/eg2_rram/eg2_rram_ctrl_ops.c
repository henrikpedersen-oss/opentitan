// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_ctrl_ops (EG2 verification spec REQ_RRAM_13, "RRAM controller in RISC-V mode"):
 * controller write, read and Rewrite across lengths and alignments, with the FIFOs, `op_done` and
 * the errors the documents define.
 *
 * Expected behaviour, from rram_ctrl's documents:
 *   - Write (programmer's guide "Issuing a Controller Write", hjson ADDR, CONTROL.NUM): whole RRAM
 *     words; ADDR "truncate[d] to the closest, lower RRAM-word (128b)"; NUM + 1 bus words, at most
 *     1024; overwrite allowed (RFC RRAM §1.1: RRAM needs no erase).
 *   - Read: ADDR truncated to the lower bus word; any length 1..1024; the controller stalls when
 *     the 16-entry read FIFO is full (STATUS.rd_full, CURR_FIFO_LVL.RD = 16).
 *   - Rewrite ("Issuing a Rewrite Operation"): one RRAM word, contents unchanged.
 *   - Each operation ends with OP_STATUS.done and the op_done interrupt; an undefined CONTROL.OP
 *     (3) sets ERR_CODE.op_err and OP_STATUS.err, and the recov_err alert ("RRAM Errors and
 *     Faults": any ERR_CODE error "also triggers the recov_err alert once the operation
 *     completes").
 * Expected data come from the test's own copy of what it wrote (in sec SRAM: a 4 KiB window of
 * the scratch pages), written whole at the start.
 *
 * Not testable on this RTL: REQ_RRAM_13's "a write while power is not stable is rejected" (RFC
 * RRAM §3.1 p7: the AST's power-stable signal to rram_macro). Neither rram_ctrl nor rram_macro
 * has such an input (proposed ticket). A write NUM that is not a multiple of 4 has no documented
 * outcome ("must be a multiple of 4"), so it is not checked (proposed documentation ticket);
 * REQ_RRAM_13's "a violation sets ERR_CODE and recov_err" is checked for the violation the
 * documents define, the undefined operation.
 *
 * Knobs:
 *   n_ops          operations after the initial write of the window
 *   w_write        operation weights: write,
 *   w_read           read,
 *   w_rewrite        Rewrite,
 *   w_unaligned      write with an unaligned ADDR (truncated),
 *   w_bad_op         undefined CONTROL.OP
 *   len_class      0 one RRAM word, 1 up to a page, 2 several pages, 3 drawn per op
 *   fifo_full      1: one read of more than 16 words waits for the read FIFO to fill first
 *   scramble       the window's region: 0 off, 1 on, 2 drawn
 *   model_gaps     skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1           | Why                                                  |
 * |---------------------------------------|------------------------------------------------------|
 * | the read FIFO's fill level stopping   | eg2sim's controller reads every word at once into an |
 * |   at 16 (the controller stalls)       |   unbounded queue: STATUS and CURR_FIFO_LVL are      |
 * |                                       |   capped at 16, but the check would be trivial       |
 *
 * Built with EG2_RRAM_CTRL_OPS_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define CTRL_OPS_PARAMS(X)   \
  X(n_ops, 16, 1, 128)       \
  X(w_write, 30, 0, 100)     \
  X(w_read, 30, 0, 100)      \
  X(w_rewrite, 15, 0, 100)   \
  X(w_unaligned, 15, 0, 100) \
  X(w_bad_op, 10, 0, 100)    \
  X(len_class, 3, 0, 3)      \
  X(fifo_full, 1, 0, 1)      \
  X(scramble, 2, 0, 2)       \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(CTRL_OPS_PARAMS)

enum {
  kPolls = 200000,
  kWinPages = 8,
  kWinWords = kWinPages * kEg2RramPageBytes / 4,  // 1024
  kSecBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
};

static volatile uint32_t
    *shadow;          // the window's expected contents, in sec SRAM
static uint32_t win;  // the window's byte address in the data partition
static bool ok_all = true;
static uint32_t ops_done;

static void chk(bool c) { ok_all = c && ok_all; }

/** A length in bus words, between 1 and `max`, by the class. */
static uint32_t draw_len(uint32_t max) {
  uint32_t cls =
      EG2_PARAM(len_class) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(len_class);
  uint32_t n;
  switch (cls) {
    case 0:
      n = 4;
      break;
    case 1:
      n = eg2_rand_range(1, kEg2RramPageBytes / 4);
      break;
    default:
      n = 1u << eg2_rand_range(7, 10);  // 128..1024 words: one to eight pages
      n += eg2_rand_range(0, n - 1);
      break;
  }
  return n > max ? max : n;
}

static void do_write(uint32_t addr, uint32_t n) {
  // The data, into the shadow first (the hardware writes from the truncated
  // address).
  uint32_t start = (addr & ~(uint32_t)(kEg2RramWordBytes - 1)) - win;
  eg2_rram_result_t r;
  r.words = 0;
  eg2_rram_start(kEg2RramOpWrite, kEg2RramPartData, addr, n);
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v = eg2_rand();
    shadow[start / 4 + i] = v;
    if (!eg2_rram_push(v, kPolls)) {
      chk(eg2_chk_true("write FIFO room", false));
      break;
    }
  }
  chk(eg2_chk_true("write done", eg2_rram_wait_done(&r, kPolls)));
  chk(eg2_chk_eq("write op_status", 1, r.op_status));
  chk(eg2_chk_eq("write ERR_CODE", 0, r.err_code));
}

static void do_read(uint32_t addr, uint32_t n, bool fill_first) {
  uint32_t start = (addr & ~3u) - win;
  eg2_rram_result_t r;
  eg2_rram_start(kEg2RramOpRead, kEg2RramPartData, addr, n);
  if (fill_first && n > kEg2RramFifoDepth) {
    // Without pops the controller stalls with the FIFO full (programmer's
    // guide).
    uint32_t i = 0;
    while (i < kPolls && !(eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) &
                           (1u << RRAM_CTRL_STATUS_RD_FULL_BIT))) {
      ++i;
    }
    chk(eg2_chk_true("read FIFO full while not drained", i < kPolls));
    if (EG2_PARAM(model_gaps)) {
      eg2_chk_skip("the controller stalling at a full read FIFO");
    } else {
      chk(eg2_chk_eq("CURR_FIFO_LVL.RD at full", kEg2RramFifoDepth,
                     (eg2_rram_reg(RRAM_CTRL_CURR_FIFO_LVL_REG_OFFSET) >>
                      RRAM_CTRL_CURR_FIFO_LVL_RD_OFFSET) &
                         RRAM_CTRL_CURR_FIFO_LVL_RD_MASK));
      chk(eg2_chk_eq("OP_STATUS.done while stalled", 0,
                     eg2_rram_reg(RRAM_CTRL_OP_STATUS_REG_OFFSET) & 1));
    }
  }
  uint32_t bad = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t v;
    if (!eg2_rram_pop(&v, kPolls)) {
      chk(eg2_chk_true("read FIFO word", false));
      break;
    }
    if (v != shadow[start / 4 + i] && bad++ < 4) {
      chk(eg2_chk_eq("read word", shadow[start / 4 + i], v));
    }
  }
  chk(eg2_chk_eq("read words that differ", 0, bad));
  chk(eg2_chk_true("read done", eg2_rram_wait_done(&r, kPolls)));
  chk(eg2_chk_eq("read op_status", 1, r.op_status));
  chk(eg2_chk_eq("read ERR_CODE", 0, r.err_code));
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
                                  kEg2RramScratchLastPage - kWinPages + 1);
  uint32_t scr =
      EG2_PARAM(scramble) == 2 ? eg2_rand_range(0, 1) : EG2_PARAM(scramble);
  eg2_rram_region_set(region, first, kWinPages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                          (scr ? kEg2RramAttrScr : 0));
  win = first * kEg2RramPageBytes;
  shadow = eg2_chk_ptr(kSecBase);
  eg2_chk_step("window at", kEg2RramHostBase + win);

  // The whole window, in one operation of the maximum length.
  do_write(win, kEg2RramMaxOpWords);
  do_read(win, kEg2RramMaxOpWords, false);

  const uint32_t w[5] = {EG2_PARAM(w_write), EG2_PARAM(w_read),
                         EG2_PARAM(w_rewrite), EG2_PARAM(w_unaligned),
                         EG2_PARAM(w_bad_op)};
  bool fifo_done = false;
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  for (uint32_t i = 0; i < EG2_PARAM(n_ops); ++i) {
    uint32_t op = eg2_chk_pick(w, 5);
    switch (op) {
      case 0:
      case 3: {  // write, aligned or not
        uint32_t n = draw_len(kWinWords) & ~3u;
        if (n == 0) {
          n = 4;
        }
        uint32_t a =
            win + kEg2RramWordBytes * eg2_rand_range(0, (kWinWords - n) / 4);
        if (op == 3) {
          a += eg2_rand_range(
              1, kEg2RramWordBytes - 1);  // truncated to the RRAM word
        }
        eg2_chk_step(op == 3 ? "write (unaligned ADDR) at" : "write at", a);
        eg2_chk_step("words", n);
        do_write(a, n);
        // Read back what was just written, from the truncated address.
        do_read(a & ~(uint32_t)(kEg2RramWordBytes - 1), n, false);
        break;
      }
      case 1: {  // read, any word alignment; the ADDR's low bits are truncated
        uint32_t n = draw_len(kWinWords);
        uint32_t a =
            win + 4 * eg2_rand_range(0, kWinWords - n) + eg2_rand_range(0, 3);
        bool fill = EG2_PARAM(fifo_full) && !fifo_done && n > kEg2RramFifoDepth;
        fifo_done = fifo_done || fill;
        eg2_chk_step("read at", a);
        eg2_chk_step("words", n);
        do_read(a, n, fill);
        break;
      }
      case 2: {  // Rewrite: contents unchanged
        uint32_t a = win + 4 * eg2_rand_range(0, kWinWords - 1);
        eg2_chk_step("rewrite at", a);
        eg2_rram_result_t r;
        chk(eg2_chk_true("rewrite done",
                         eg2_rram_rewrite(kEg2RramPartData, a, &r)));
        chk(eg2_chk_eq("rewrite op_status", 1, r.op_status));
        chk(eg2_chk_eq("rewrite ERR_CODE", 0, r.err_code));
        do_read(a & ~(uint32_t)(kEg2RramWordBytes - 1), 4, false);
        break;
      }
      default: {  // CONTROL.OP = 3, undefined
        eg2_chk_step("undefined op", 3);
        eg2_rram_result_t r;
        eg2_rram_start(3, kEg2RramPartData, win, 4);
        chk(eg2_chk_true("undefined op ends", eg2_rram_wait_done(&r, kPolls)));
        chk(eg2_chk_eq("undefined op OP_STATUS (done, err)", 3, r.op_status));
        chk(eg2_chk_eq("undefined op ERR_CODE (op_err)", 1u << 0, r.err_code));
        if (recov >= 0) {
          eg2_status_expect_alert((size_t)recov);
          chk(eg2_chk_true("recov_err alert after the undefined op",
                           eg2_status_wait_alert((size_t)recov, 10000)));
          eg2_status_clear_expected();
        }
        break;
      }
    }
    ++ops_done;
  }
  eg2_rram_region_off(region);
  chk(eg2_chk_eq("ops done", EG2_PARAM(n_ops), ops_done));
  return eg2_chk_summary(8 + 3 * EG2_PARAM(n_ops)) && ok_all;
}
