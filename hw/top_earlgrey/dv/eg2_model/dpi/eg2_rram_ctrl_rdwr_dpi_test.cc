// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the rram_ctrl controller-path reference model (ip/eg2_rram_ctrl_rdwr_core.h),
// without a simulator:
//   g++ -std=c++17 -O1 -Wall -I. dpi/eg2_rram_ctrl_rdwr_dpi_test.cc -o /tmp/rdwr_test && /tmp/rdwr_test
// (from hw/top_earlgrey/dv/eg2_model). EG2_RRAM_RDWR_TEST_BREAK=1 breaks one expectation on
// purpose: the run must then fail.
//
// Each scenario plays a documented sequence and checks that the model accepts it, and that it
// rejects the wrong variants: a read (XOR removal, all ones after an error, full word count,
// ERR_ADDR), a write (FIFO words in order, last, done only after all words, abort at the first
// error), a Rewrite (read, the same words written back, nothing to software), the rejections
// (not initialized, undefined OP, unaligned write, RMA software disable), hardware priority, no
// access before phy_init_done or after rram_disable, stray words never written.

#include <cstdio>
#include <cstdlib>

#include "../ip/eg2_rram_ctrl_rdwr_core.h"

using M = eg2_model::RramCtrlRdWr;

static int fails = 0, checks = 0;
static bool brk = false;
static void ok(const std::string &r, const char *what) {
  ++checks;
  if (!r.empty()) {
    ++fails;
    printf("FAIL: %s: %s\n", what, r.c_str());
  }
}
static void bad(const std::string &r, const char *what) {
  ++checks;
  if (r.empty()) {
    ++fails;
    printf("FAIL: %s: accepted, must be rejected\n", what);
  }
}
static void up(M &m, int n = 1, bool ctrl = true, bool dis = false, bool swdis = false) {
  for (int i = 0; i < n; i++) m.cycle(true, ctrl, dis, swdis);
}
// The PHY's word for a clean word at a bus-word address: the XOR kept, the integrity of the clean word.
static uint64_t phy_word(uint32_t clean, uint32_t addr) {
  return uint64_t(M::intg(clean)) << 32 | (clean ^ addr);
}

int main() {
  brk = getenv("EG2_RRAM_RDWR_TEST_BREAK") != nullptr;
  ok(M::intg(0) == 0x2a ? "" : "intg(0) != 0x2a", "bus integrity of 0 (tlul_data_integ_enc)");

  {  // read of 3 words from byte 0x106 (truncated to the word 0x41), clean
    M m;
    up(m, 3);
    ok(m.request(M::kSw, M::kRead, 0, 2, 0x106, 0), "read request");
    up(m, 2);
    bad(M(m).rd_req(0x42, false, M::kSelSw, 0, 0), "read starting one word late");
    ok(m.rd_req(0x41, false, M::kSelSw, 0, 0), "read 0");
    bad(M(m).rd_req(0x42, false, M::kSelSw, 0, 0), "second outstanding read");
    ok(m.rd_rsp(phy_word(0x1234, 0x41), false, false), "answer 0");
    bad(M(m).deliver(M::kSw, M::with_intg(0x1234 ^ 0x41), true), "word with the XOR left in");
    M m2 = m;
    ok(m2.deliver(M::kSw, M::with_intg(0x1234), true), "word 0 clean");
    m = m2;
    ok(m.rd_req(0x42, false, M::kSelSw, 0, 0), "read 1");
    ok(m.rd_rsp(phy_word(0x5678, 0x42), false, false), "answer 1");
    ok(m.deliver(M::kSw, M::with_intg(0x5678), true), "word 1");
    bad(M(m).done(M::kSw, 0, 0), "done after 2 of 3 words");
    ok(m.rd_req(0x43, false, M::kSelSw, 0, 0), "read 2");
    ok(m.rd_rsp(phy_word(0x9abc, 0x43), false, false), "answer 2");
    ok(m.deliver(M::kSw, M::with_intg(brk ? 0x9abd : 0x9abc), true), "word 2");
    ok(m.done(M::kSw, 0, 0), "done after 3 words");
    bad(M(m).done(M::kSw, 0, 0), "a second done");
  }
  {  // read error on word 1 of 4: all ones from there, no new access, rd_err and ERR_ADDR
    M m;
    up(m, 3);
    ok(m.request(M::kSw, M::kRead, 1, 3, 0x200, 0), "read request info");
    up(m, 2);
    ok(m.rd_req(0x80, false, M::kSelSw, 1, 0), "read 0");
    ok(m.rd_rsp(phy_word(7, 0x80), false, false), "answer 0");
    ok(m.deliver(M::kSw, M::with_intg(7), true), "word 0");
    ok(m.rd_req(0x81, false, M::kSelSw, 1, 0), "read 1");
    ok(m.rd_rsp(phy_word(8, 0x81), true, false), "answer 1 with rd_err");
    bad(M(m).deliver(M::kSw, M::with_intg(8), true), "the failing word as data");
    M m2 = m;
    ok(m2.deliver(M::kSw, M::ones39(), true), "the failing word as all ones");
    bad(M(m2).rd_req(0x82, false, M::kSelSw, 1, 0), "a read after the error");
    ok(m2.deliver(M::kSw, M::ones39(), true), "word 2 all ones");
    bad(M(m2).done(M::kSw, M::kRdErr, 0x81), "done before word 3");
    ok(m2.deliver(M::kSw, M::ones39(), true), "word 3 all ones");
    M m3 = m2;
    bad(M(m3).done(M::kSw, 0, 0), "done without rd_err");
    m3 = m2;
    bad(M(m3).done(M::kSw, M::kRdErr, 0x80), "ERR_ADDR of word 0");
    ok(m2.done(M::kSw, M::kRdErr, 0x81), "done with rd_err at word 1");
  }
  {  // write of 8 words: the words pushed after START, in order, last on the 8th
    M m;
    up(m, 3);
    m.push(M::kSw, M::with_intg(0xdead));  // stray: no request yet
    ok(m.request(M::kSw, M::kWrite, 0, 7, 0x1000, 0), "write request");
    for (uint32_t i = 0; i < 3; i++) m.push(M::kSw, M::with_intg(100 + i));
    up(m, 2);
    bad(M(m).wr_req(0x400, false, M::with_intg(0xdead), false, M::kSelSw, 0, 0), "the stray word written");
    for (uint32_t i = 0; i < 8; i++) {
      if (i >= 3) m.push(M::kSw, M::with_intg(100 + i));
      if (i == 7) {
        M b = m;
        bad(b.wr_req(0x407, false, M::with_intg(107), false, M::kSelSw, 0, 0), "last word without last");
      }
      if (i == 2) {
        M b = m;
        bad(b.done(M::kSw, 0, 0), "write done before its words are programmed");
      }
      ok(m.wr_req(0x400 + i, false, M::with_intg(100 + i), i == 7, M::kSelSw, 0, 0), "write word");
      ok(m.wr_rsp(false, false), "write answer");
    }
    ok(m.done(M::kSw, 0, 0), "write done");
  }
  {  // write error on word 1: no further write, done with wr_err, ERR_ADDR word 1
    M m;
    up(m, 3);
    ok(m.request(M::kSw, M::kWrite, 0, 3, 0x40, 0), "write request");
    for (uint32_t i = 0; i < 4; i++) m.push(M::kSw, M::with_intg(i));
    up(m, 2);
    ok(m.wr_req(0x10, false, M::with_intg(0), false, M::kSelSw, 0, 0), "write 0");
    ok(m.wr_rsp(false, false), "answer 0");
    ok(m.wr_req(0x11, false, M::with_intg(1), false, M::kSelSw, 0, 0), "write 1");
    ok(m.wr_rsp(false, true), "answer 1 integrity error");
    bad(M(m).wr_req(0x12, false, M::with_intg(2), false, M::kSelSw, 0, 0), "write after the error");
    bad(M(m).done(M::kSw, M::kWrErr, 0x10), "ERR_ADDR word 0");
    ok(m.done(M::kSw, M::kWrErr, 0x11), "done with wr_err");
  }
  {  // Rewrite: 4 reads, the same clean words written back, nothing delivered to software
    M m;
    up(m, 3);
    ok(m.request(M::kSw, M::kRewrite, 0, 0, 0x30, 0), "Rewrite request (NUM ignored)");
    up(m, 2);
    bad(M(m).rd_req(0xc, false, M::kSelSw, 0, 0), "Rewrite under SwSel");
    for (uint32_t i = 0; i < 4; i++) {
      ok(m.rd_req(0xc + i, false, M::kSelLoop, 0, 0), "Rewrite read");
      ok(m.rd_rsp(phy_word(50 + i, 0xc + i), false, false), "Rewrite answer");
      if (i == 0) bad(M(m).deliver(M::kSw, M::with_intg(50), true), "Rewrite data to software");
      if (i == 2) bad(M(m).wr_req(0xc, false, M::with_intg(50), false, M::kSelLoop, 0, 0), "write before the read completed");
    }
    for (uint32_t i = 0; i < 4; i++) {
      if (i == 1) bad(M(m).wr_req(0xd, false, M::with_intg(99), false, M::kSelLoop, 0, 0), "other data written back");
      ok(m.wr_req(0xc + i, false, M::with_intg(50 + i), i == 3, M::kSelLoop, 0, 0), "Rewrite write");
      ok(m.wr_rsp(false, false), "Rewrite write answer");
    }
    ok(m.done(M::kSw, 0, 0), "Rewrite done");
  }
  {  // rejections: no access, done with op_err only
    M m;
    up(m, 3, false);
    ok(m.request(M::kSw, M::kRead, 0, 0, 0, 0), "read before init");
    up(m, 1, false);
    bad(M(m).rd_req(0, false, M::kSelSw, 0, 0), "access before init");
    bad(M(m).done(M::kSw, M::kOpErr | M::kRdErr, 0), "rejection with other bits");
    ok(m.done(M::kSw, M::kOpErr), "rejected before init");
    up(m, 1);
    ok(m.request(M::kSw, 3, 0, 0, 0, 0), "undefined OP");
    up(m, 1);
    bad(M(m).rd_req(0, false, M::kSelSw, 0, 0), "undefined OP accesses");
    ok(m.done(M::kSw, M::kOpErr), "undefined OP rejected");
    ok(m.request(M::kSw, M::kWrite, 0, 3, 0x4, 0), "unaligned write");
    up(m, 1);
    ok(m.done(M::kSw, M::kOpErr), "unaligned write rejected");
    ok(m.request(M::kSw, M::kWrite, 0, 4, 0x0, 0), "5-word write");
    up(m, 1);
    ok(m.done(M::kSw, M::kOpErr), "5-word write rejected");
    ok(m.request(M::kSw, M::kRewrite, 0, 0, 0x8, 0), "unaligned Rewrite");
    up(m, 1);
    ok(m.done(M::kSw, M::kOpErr), "unaligned Rewrite rejected");
    ok(m.request(M::kSw, M::kRead, 0, 0, 0x8, 0), "aligned read");
    up(m, 1);
    bad(M(m).done(M::kSw, M::kOpErr), "a valid read rejected");
    up(m, 1, true, false, true);
    ok(m.done(M::kSw, M::kOpErr), "read rejected under RMA software disable");
  }
  {  // hardware priority; lcmgr phase passed; OTP read delivered without data
    M m;
    up(m, 3);
    ok(m.request(M::kSw, M::kRead, 0, 0, 0, 0), "software read");
    ok(m.request(M::kOtp, M::kRead, 0, 3, 0x7fc00, 0), "OTP read");
    up(m, 3);
    bad(M(m).rd_req(0, false, M::kSelSw, 0, 0), "software first while OTP waits");
    for (uint32_t i = 0; i < 4; i++) {
      ok(m.rd_req(0x1ff00 + i, false, M::kSelOtp, 0, 0), "OTP read word");
      ok(m.rd_rsp(phy_word(i, 0x1ff00 + i), false, false), "OTP answer");
      ok(m.deliver(M::kOtp, 0, false), "OTP word");
    }
    ok(m.done(M::kOtp, 0, 0), "OTP done");
    ok(m.request(M::kLc, M::kRead, 1, 0, 0x500, 1), "lcmgr read, phase RMA");
    up(m, 3);
    bad(M(m).rd_req(0x140, false, M::kSelLc, 1, 0), "lcmgr phase changed");
    ok(m.rd_req(0x140, false, M::kSelLc, 1, 1), "lcmgr read");
    ok(m.rd_rsp(phy_word(3, 0x140), false, true), "lcmgr mp_err");
    ok(m.deliver(M::kLc, M::ones39(), true), "lcmgr all ones");
    ok(m.done(M::kLc, M::kMpErr, 0), "lcmgr done with mp_err");
    ok(m.rd_req(0, false, M::kSelSw, 0, 0), "software after the plugs");
  }
  {  // phy_init_done and rram_disable gate every access
    M m;
    m.cycle(false, true, false, false);
    ok(m.request(M::kOtp, M::kRead, 0, 3, 0, 0), "OTP before phy init");
    bad(M(m).rd_req(0, false, M::kSelOtp, 0, 0), "access before phy_init_done");
    up(m, 1, true, true);
    up(m, 3, true, true);
    bad(M(m).rd_req(0, false, M::kSelOtp, 0, 0), "access after rram_disable");
    ok(m.end_check(), "a request left after rram_disable is not an error (counted)");
    ok(m.n_unanswered_after_dis == 1 ? "" : "not counted", "unanswered after disable counted");
  }
  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
