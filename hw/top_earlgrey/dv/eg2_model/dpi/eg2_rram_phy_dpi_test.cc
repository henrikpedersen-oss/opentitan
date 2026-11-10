// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the rram_ctrl PHY reference model (ip/eg2_rram_phy_core.h), without a simulator:
//   g++ -std=c++17 -O2 -Wall -I. dpi/eg2_rram_phy_dpi_test.cc -o /tmp/rram_phy_test && /tmp/rram_phy_test
// (from hw/top_earlgrey/dv/eg2_model). EG2_RRAM_PHY_TEST_BREAK=1 breaks one expectation on purpose:
// the run must then fail.
//
// Checks: the bus integrity against known TL-UL values (an all-zero word has the inverted parity
// bits set: 0x2a, as tlul_data_integ_enc gives for 0); write then read back, scrambled and plain, with
// and without the address XOR (the read returns the infected word, its integrity over the clean
// word); the write integrity zero-fill; the commit points (last word, 32 words, page end); the macro
// write check rejecting a wrong push; uncorrectable ECC giving an error and a rewrite clearing it.

#include <cstdio>
#include <cstdlib>

#include "../ip/eg2_rram_phy_core.h"

using eg2_model::RramPhy;
using eg2_model::rram_u128;

static int fails = 0, checks = 0;
static void expect(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++fails;
    printf("FAIL: %s\n", what);
  }
}

static uint64_t d39(uint32_t w) { return RramPhy::with_intg(w, w); }

// Feeds the model's expected macro writes back to it, as a correct rram_phy would make them.
static void drain(RramPhy &m) {
  while (!m.expected.empty()) {
    RramPhy::MacroWr e = m.expected.front();
    expect(m.macro_wr(e.last, e.addr, e.part, e.ecc_en, e.data), "drain: the model's own write");
  }
}

int main() {
  bool brk = getenv("EG2_RRAM_PHY_TEST_BREAK") && atoi(getenv("EG2_RRAM_PHY_TEST_BREAK"));
  // TL-UL integrity of 0 (prim_secded_inv_39_32_enc: the inverted parity bits 1, 3, 5 set).
  expect(RramPhy::intg(0) == 0x2a, "intg(0) == 0x2a");
  expect(RramPhy::intg(brk ? 1 : 0) == 0x2a, "intg(0) == 0x2a (broken on purpose)");

  RramPhy m(rram_u128(0x0123456789abcdefull) << 64 | 0xfedcba9876543210ull,
            rram_u128(0x1111222233334444ull) << 64 | 0x5555666677778888ull);
  // Four RRAM words at page 3, word 4 (bus words 3*128 + 16 ... ), scrambled, address XOR on.
  uint32_t base = 3u * 128 + 16;
  for (uint32_t i = 0; i < 16; ++i)
    m.ctrl_wr_word(base + i, d39(0xa5000000 | i), false, true, true, true, i == 15);
  expect(m.expected.size() == 5, "four pushes and one commit");
  expect(m.expected.back().last, "the commit after the last word");
  drain(m);
  for (uint32_t i = 0; i < 16; ++i) {
    bool err;
    uint64_t r = m.read(base + i, false, true, true, true, &err);
    uint32_t w = uint32_t(r);
    expect(!err, "no error");
    expect(w == ((0xa5000000 | i) ^ (base + i)), "read keeps the address XOR");
    expect(uint8_t(r >> 32) == RramPhy::intg(0xa5000000 | i), "integrity over the clean word");
  }
  // The packing order: bus word 2 at bits [95:64] (plain, no XOR).
  m.ctrl_wr_word(8, d39(0x11), true, false, true, false, false);
  m.ctrl_wr_word(9, d39(0x22), true, false, true, false, false);
  m.ctrl_wr_word(10, d39(0x33), true, false, true, false, false);
  m.ctrl_wr_word(11, d39(0x44), true, false, true, false, true);
  expect(uint32_t(m.expected.front().data >> 64) == 0x33, "packing: bus word 2 at bits 95:64");
  drain(m);
  // Integrity error: the word zero-filled before the XOR.
  m.ctrl_wr_word(0, d39(0x77) ^ (uint64_t(1) << 33), true, false, true, true, false);
  m.ctrl_wr_word(1, d39(0x88), true, false, true, true, false);
  m.ctrl_wr_word(2, d39(0x99), true, false, true, true, false);
  m.ctrl_wr_word(3, d39(0xaa), true, false, true, true, true);
  expect(m.wr_intg_errs == 1, "one integrity error");
  expect(uint32_t(m.expected.front().data) == (0u ^ 0u), "zero-filled word 0 (XOR with address 0)");
  // A wrong push is rejected.
  RramPhy::MacroWr e = m.expected.front();
  expect(!m.macro_wr(false, e.addr, e.part, e.ecc_en, e.data ^ 1), "a wrong push is rejected");
  m.expected.clear();
  m.store.clear();
  // Commit points: 40 words from the start of page 5 -> commit at 32 (page end), then after 8.
  for (uint32_t i = 0; i < 40 * 4; ++i)
    m.ctrl_wr_word(5u * 128 + i, d39(i), false, false, false, true, i == 40 * 4 - 1);
  int commits = 0;
  for (auto &w : m.expected) commits += w.last;
  expect(commits == 2, "two commits for 40 words from a page start");
  drain(m);
  // Uncorrectable ECC: error; a rewrite clears it.
  m.set_status(false, 5u * 32 + 3, 4);
  bool err;
  m.read(5u * 128 + 12, false, false, true, true, &err);
  expect(err, "uncorrectable gives an error");
  m.read(5u * 128 + 12, false, false, false, true, &err);
  expect(!err, "no ECC decode, no error");
  for (uint32_t i = 12; i < 16; ++i) m.ctrl_wr_word(5u * 128 + i, d39(i), false, false, true, true, i == 15);
  drain(m);
  m.read(5u * 128 + 12, false, false, true, true, &err);
  expect(!err, "a rewrite clears the ECC status");

  printf("%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
