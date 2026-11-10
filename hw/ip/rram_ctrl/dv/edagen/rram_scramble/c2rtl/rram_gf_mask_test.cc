// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_gf_mask_hls() (the HLS input) against the reference model's mask (eg2_rram_scramble_core.h)
// on 100,000 random keys and addresses and the corner cases. RRAM_GF_TEST_BREAK=1 flips one
// expected bit: the run must then fail.
#include <cstdio>
#include <cstdlib>

#include "../../../../../../top_earlgrey/dv/eg2_model/ip/eg2_rram_scramble_core.h"

extern "C" unsigned rram_gf_mask_hls(unsigned k0, unsigned k1, unsigned k2, unsigned k3,
                                     unsigned addr, unsigned char word);

int main() {
  using eg2_model::RramScramble;
  using eg2_model::rram_u128;
  const bool brk = std::getenv("RRAM_GF_TEST_BREAK") != nullptr;
  unsigned long long s = 0x9e3779b97f4a7c15ull;
  auto rnd = [&s]() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return unsigned(s);
  };
  int fails = 0, checks = 0;
  for (int n = 0; n < 100000; ++n) {
    unsigned k[4] = {rnd(), rnd(), rnd(), rnd()};
    if (n < 4) for (unsigned &w : k) w = n & 1 ? ~0u : 0u;
    unsigned addr = n < 8 ? (n < 4 ? 0x1ffffu : 1u << n) : rnd() & 0x1ffffu;
    rram_u128 key = 0;
    for (int i = 3; i >= 0; --i) key = (key << 32) | k[i];
    RramScramble m(key, 0);
    rram_u128 exp = m.mask(addr);
    if (brk && n == 7) exp ^= 1;
    for (unsigned w = 0; w < 4; ++w) {
      ++checks;
      if (rram_gf_mask_hls(k[0], k[1], k[2], k[3], addr, (unsigned char)w) != unsigned(exp >> (32 * w))) ++fails;
    }
  }
  std::printf("%s: %d checks, %d failures\n", fails ? "FAILED" : "PASSED", checks, fails);
  return fails ? 1 : 0;
}
