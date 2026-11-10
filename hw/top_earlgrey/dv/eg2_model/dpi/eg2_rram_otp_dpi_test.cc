// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Self-test of the rram_ctrl OTP plug reference model (ip/eg2_rram_otp_core.h) through its DPI
// functions (eg2_rram_otp_dpi.cc). Build and run (no simulator needed):
//   g++ -std=c++17 -O1 -Wall dpi/eg2_rram_otp_dpi_test.cc dpi/eg2_rram_otp_dpi.cc -o /tmp/otp_test
//   /tmp/otp_test            # must pass
//   /tmp/otp_test --inject   # must fail (one expected value corrupted)
//
// Hamming(72,64) vectors: util/design/secded_gen.py ecc_encode('hamming', 64, w) for w = 0,
// all ones, and three random words (Python's random.seed(1) getrandbits(64)).

#include <cstdint>
#include <cstdio>
#include <cstring>

extern "C" {
void *eg2_rram_otp_create();
void eg2_rram_otp_release(void *h);
void eg2_rram_otp_poke(void *h, unsigned off, unsigned v);
unsigned eg2_rram_otp_peek(void *h, unsigned off);
unsigned eg2_rram_otp_hamming(const uint32_t *d);
int eg2_rram_otp_cmd(void *h, unsigned cmd, unsigned size, unsigned addr, const uint32_t *wdata,
                     int rd_err, int wr_err, int readback_corrupt, int disabled, uint32_t *rdata,
                     uint32_t *rmask, int *mem_pred, unsigned *blk_lo, unsigned *blk_hi);
}

namespace {
int fails = 0, checks = 0;
bool inject = false;

void expect(bool ok, const char *what) {
  ++checks;
  if (!ok) {
    ++fails;
    std::printf("FAIL %s\n", what);
  }
}

unsigned ham(uint64_t w) {
  uint32_t d[2] = {uint32_t(w), uint32_t(w >> 32)};
  return eg2_rram_otp_hamming(d);
}

struct R {
  int err;
  uint64_t rdata, rmask;
  int mem_pred;
};

R cmd(void *h, unsigned c, unsigned size, unsigned addr, uint64_t wd, int rd = 0, int wr = 0,
      int rb = 0, int dis = 0) {
  uint32_t w[2] = {uint32_t(wd), uint32_t(wd >> 32)}, rd_[2], rm[2];
  unsigned lo, hi;
  R r;
  r.err = eg2_rram_otp_cmd(h, c, size, addr, w, rd, wr, rb, dis, rd_, rm, &r.mem_pred, &lo, &hi);
  r.rdata = rd_[0] | (uint64_t(rd_[1]) << 32);
  r.rmask = rm[0] | (uint64_t(rm[1]) << 32);
  return r;
}
}  // namespace

int main(int argc, char **argv) {
  inject = argc > 1 && std::strcmp(argv[1], "--inject") == 0;
  enum { kRead = 0x7a, kWrite = 0x49, kReadRaw = 0x54, kWriteRaw = 0x67, kInit = 0x20, kZeroize = 0x3d };

  // Hamming(72,64).
  expect(ham(0) == 0x00, "hamming(0) is 0x00");
  expect(ham(~0ull) == (inject ? 0xfe : 0xff), "hamming(all ones) is 0xff");
  // Codewords from secded_gen.py (data | check bits << 64): check bits = codeword >> 64.
  // 47393446279084618229 = 0x2_91b7584a2265b1f5, 3538127296308559571679 = 0xbf_cd613e30d8f16adf,
  // 4207021764239683926980 = 0xe4_1027c4d1c386bbc4.
  expect(ham(0x91b7584a2265b1f5ull) == 0x02, "hamming vector 1");
  expect(ham(0xcd613e30d8f16adfull) == 0xbf, "hamming vector 2");
  expect(ham(0x1027c4d1c386bbc4ull) == 0xe4, "hamming vector 3");
  // Linear: hamming(a ^ b) == hamming(a) ^ hamming(b).
  for (uint64_t a = 1; a; a <<= 7) {
    uint64_t b2 = a * 0x9e3779b97f4a7c15ull;
    expect(ham(a ^ b2) == (ham(a) ^ ham(b2)), "hamming is linear");
  }

  void *h = eg2_rram_otp_create();
  // An erased region (all zero) reads as zero with a valid syndrome.
  R r = cmd(h, kInit, 0, 0, 0);
  expect(r.err == 0, "Init answers NoError");
  r = cmd(h, kRead, 3, 0, 0);
  expect(r.err == 0 && r.rdata == 0 && r.rmask == ~0ull, "erased block reads 0, NoError");
  // Write a 32-bit word, read it back, check the syndrome in the integrity page.
  r = cmd(h, kWrite, 1, 4, 0x12345678ull);  // words 4,5 = bytes 8..11: block 1
  expect(r.err == 0 && r.mem_pred, "Write NoError");
  expect(eg2_rram_otp_peek(h, 512 + 8) == 0x78 && eg2_rram_otp_peek(h, 512 + 11) == 0x12,
         "Write lands at region offset 512 + 2 * addr");
  expect(eg2_rram_otp_peek(h, 1) == ham(0x12345678ull), "Write updates the block's syndrome at offset b / 8");
  r = cmd(h, kRead, 1, 4, 0);
  expect(r.err == 0 && r.rdata == 0x12345678ull && r.rmask == 0xffffffffull, "Read back the word");
  // Read-set-write: setting more bits is fine, clearing one is MacroWriteBlankError.
  r = cmd(h, kWrite, 1, 4, 0x12345679ull);
  expect(r.err == 0, "Write that only sets bits");
  r = cmd(h, kWrite, 0, 4, 0x0000ull);
  expect(r.err == 4 && !r.mem_pred, "Write clearing a bit: MacroWriteBlankError, memory not predicted");
  r = cmd(h, kWriteRaw, 0, 4, 0x5678ull);
  expect(r.err == 4, "WriteRaw clearing a bit: MacroWriteBlankError");
  // WriteRaw leaves the integrity page alone: a later Read of the block fails its integrity check.
  r = cmd(h, kWriteRaw, 0, 8, 0x0001ull);  // block 2, syndrome stays 0
  expect(r.err == 0 && eg2_rram_otp_peek(h, 2) == 0, "WriteRaw does not update the syndrome");
  r = cmd(h, kRead, 0, 8, 0);
  expect(r.err == 3, "Read after WriteRaw changed the data: MacroEccUncorrError");
  r = cmd(h, kReadRaw, 0, 8, 0);
  expect(r.err == 0 && r.rdata == 1 && r.rmask == 0xffff, "ReadRaw ignores the integrity page");
  // A corrupted syndrome.
  eg2_rram_otp_poke(h, 0, 0x01);
  r = cmd(h, kRead, 3, 0, 0);
  expect(r.err == 3, "corrupted syndrome: MacroEccUncorrError");
  r = cmd(h, kReadRaw, 3, 0, 0);
  expect(r.err == 0 && r.rdata == 0, "corrupted syndrome: ReadRaw still NoError");
  // Zeroize sets every bit of the word and returns it.
  r = cmd(h, kZeroize, 3, 16, 0);
  expect(r.err == 0 && r.rdata == ~0ull && r.rmask == ~0ull, "Zeroize returns all ones");
  expect(eg2_rram_otp_peek(h, 4) == 0xff, "Zeroize updates the syndrome");
  r = cmd(h, kRead, 3, 16, 0);
  expect(r.err == 0 && r.rdata == ~0ull, "Read after Zeroize: all ones, NoError");
  // Errors.
  r = cmd(h, kRead, 0, 1280, 0);  // first word past the five data pages
  expect(r.err == 1 && r.mem_pred, "past the OTP pages: MacroError, nothing changes");
  r = cmd(h, kRead, 3, 2, 0);  // words 2..5 cross blocks 0 and 1
  expect(r.err == -1, "crossing a 64-bit block: not predicted");
  r = cmd(h, kRead, 0, 40, 0, 1);
  expect(r.err == 1 && !r.mem_pred, "RRAM read error: MacroError");
  r = cmd(h, kWrite, 0, 40, 0x1, 0, 1);
  expect(r.err == 1, "RRAM write error: MacroError");
  r = cmd(h, kWrite, 0, 44, 0x3, 0, 0, 1);
  expect(r.err == 3 && r.mem_pred, "Write with a corrupted readback: MacroEccUncorrError, data written");
  r = cmd(h, kWriteRaw, 0, 48, 0x3, 0, 0, 1);
  expect(r.err == 3, "WriteRaw with a corrupted readback: MacroEccUncorrError");
  r = cmd(h, kZeroize, 0, 52, 0, 0, 0, 1);
  expect(r.err == -1, "Zeroize with a corrupted readback: not predicted");
  r = cmd(h, 0x7f, 0, 0, 0);
  expect(r.err == -2 && !r.mem_pred, "undefined command: some error");
  r = cmd(h, kRead, 0, 0, 0, 0, 0, 0, 1);
  expect(r.err == -2 && r.mem_pred, "disabled: some error, nothing changes");
  // Every native word of the last data page is in range; the depth otp_ctrl uses (1032) is too.
  r = cmd(h, kReadRaw, 0, 1279, 0);
  expect(r.err == 0, "last native word in range");
  eg2_rram_otp_release(h);

  std::printf("eg2_rram_otp_dpi_test: %d checks, %d failed%s\n", checks, fails,
              inject ? " (--inject: must fail)" : "");
  return fails ? 1 : 0;
}
