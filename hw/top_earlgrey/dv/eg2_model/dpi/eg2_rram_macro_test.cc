// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the RRAM macro model (ip/eg2_rram_macro.h): the two Hamming(72,64) codes, the
// documented ecc_err outcomes, the store buffer, and (with a file argument) the ECC of every row of
// a build's RRAM image (<test>_sim_dv.128.scr.vmem, util/design/gen-rram-img.py: 144-bit rows,
// {ecc_hi, ecc_lo} above the 128 data bits): an independent check of the model's encoder against
// the build tool's.
//   g++ -std=c++17 -O2 -Wall -I. dpi/eg2_rram_macro_test.cc -o /tmp/rram_macro_test
//   /tmp/rram_macro_test [image.128.scr.vmem ...]    (PASS, exit 0)
//   EG2_RRAM_MACRO_TEST_BREAK=1 /tmp/rram_macro_test  (one expectation inverted: exit 1)

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

#include "ip/eg2_rram_macro.h"

using eg2_model::RramMacro;
using eg2_model::rram_u128;

static int fails = 0, checks = 0;
static bool brk = std::getenv("EG2_RRAM_MACRO_TEST_BREAK") != nullptr;
static void check(bool ok, const std::string &what) {
  ++checks;
  if (brk && checks == 3) ok = !ok;
  if (!ok) {
    ++fails;
    if (fails < 20) std::printf("FAIL %s\n", what.c_str());
  }
}

static rram_u128 hex128(const std::string &h) {
  rram_u128 v = 0;
  for (char c : h) v = (v << 4) | rram_u128(std::stoul(std::string(1, c), nullptr, 16));
  return v;
}

int main(int argc, char **argv) {
  std::mt19937_64 rng(1);
  RramMacro m;
  // An all-zero word encodes to zero ECC, an all-ones half to 0xff (as the OTP integrity scheme
  // documents for the same code).
  check(RramMacro::ecc(0) == 0, "ecc(0) == 0");
  check(RramMacro::ecc(~rram_u128(0)) == 0xffff, "ecc(all ones) == 0xffff");
  for (int n = 0; n < 200; ++n) {
    rram_u128 d = (rram_u128(rng()) << 64) | rng();
    uint32_t a = uint32_t(rng() % RramMacro::kDataRows);
    m.backdoor_write(false, a, d, true);
    RramMacro::Rd r = m.read(false, a, true);
    check(r.data == d && r.ecc_err == RramMacro::kEccNone, "clean row reads back without error");
    // Every single-bit error, data or ECC: corrected, 001.
    unsigned b = unsigned(rng() % 144);
    m.flip(false, a, b);
    r = m.read(false, a, true);
    check(r.data == d && r.ecc_err == RramMacro::kEccCorr1, "one flipped bit " + std::to_string(b) + ": corrected, 001");
    // A second bit in the other half: both halves corrected, 010.
    unsigned b2 = (b < 64 || (b >= 128 && b < 136)) ? 64 + unsigned(rng() % 64) : unsigned(rng() % 64);
    m.flip(false, a, b2);
    r = m.read(false, a, true);
    check(r.data == d && r.ecc_err == RramMacro::kEccCorr2, "one bit in each half: corrected, 010");
    // Without ECC decoding: the stored bits, no outcome.
    r = m.read(false, a, false);
    check(r.data != d && r.ecc_err == RramMacro::kEccNone, "ecc_en low: the stored data, 000");
    // Two bits in one half: uncorrectable, 100.
    m.backdoor_write(false, a, d, true);
    unsigned c1 = unsigned(rng() % 64), c2 = (c1 + 1 + unsigned(rng() % 63)) % 64;
    m.flip(false, a, c1);
    m.flip(false, a, c2);
    r = m.read(false, a, true);
    check(r.ecc_err == RramMacro::kEccUncorr, "two bits in one half: uncorrectable, 100");
  }
  // Store buffer: a commit with nothing pushed and a push to a full buffer answer err; a commit
  // writes each pushed word at its word-in-page position of the committed page.
  check(!m.commit(true, 5 * 32), "commit with an empty store buffer: err");
  for (uint32_t i = 0; i < 32; ++i) check(m.push(i, rram_u128(0x100 + i), true), "push");
  check(!m.push(0, 1, true), "33rd push: err (store buffer full)");
  std::vector<uint32_t> rows;
  check(m.commit(true, 5 * 32 + 7, &rows) && rows.size() == 32, "commit of 32 words");
  check(m.read(true, 5 * 32 + 3, true).data == 0x103 && m.row(true, 5 * 32 + 3).programmed,
        "info page 5, word 3 holds the pushed word");
  RramMacro fresh;
  check(!fresh.row(false, 77).programmed && fresh.read(false, 77, true).ecc_err == RramMacro::kEccNone,
        "a never-written row is blank: zero data and ECC, a clean codeword");

  // A build's image: every row's ECC is the model's ECC of its data.
  for (int i = 1; i < argc; ++i) {
    std::ifstream f(argv[i]);
    std::string line;
    size_t rows_seen = 0, bad = 0;
    while (std::getline(f, line)) {
      std::istringstream ss(line);
      std::string tok;
      while (ss >> tok) {
        if (tok[0] == '@' || tok.size() != 36) continue;
        uint16_t e = uint16_t(std::stoul(tok.substr(0, 4), nullptr, 16));
        rram_u128 d = hex128(tok.substr(4));
        ++rows_seen;
        if (RramMacro::ecc(d) != e) ++bad;
      }
    }
    check(rows_seen > 0 && bad == 0, std::string(argv[i]) + ": " + std::to_string(rows_seen) +
                                         " rows, " + std::to_string(bad) + " with an ECC other than the model's");
    std::printf("%s: %zu rows, %zu ECC mismatches\n", argv[i], rows_seen, bad);
  }
  std::printf("%s %d/%d checks\n", fails ? "FAIL" : "PASS", checks - fails, checks);
  return fails ? 1 : 0;
}
