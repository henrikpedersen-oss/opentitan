// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The scrambling mask of the rram_ctrl reference model for HLS (Bambu): addr_tweak =
// GF_MULT(word_addr, addr_key) in GF(2^128) mod x^128 + x^7 + x^2 + x + 1, bit i the coefficient
// of x^i (eg2_rram_scramble_core.h, RramScramble::gf_mul and mask). Bambu has no 128-bit type: the
// key comes in as four 32-bit words (k0 least significant) and one output word, picked by `word`,
// goes out. rram_gf_mask_test.c checks this function against the model on random inputs, so the
// equivalence proof (equiv.ys) connects the model's arithmetic to the RTL.

unsigned rram_gf_mask_hls(unsigned k0, unsigned k1, unsigned k2, unsigned k3, unsigned addr,
                          unsigned char word) {
  unsigned a[4] = {k0, k1, k2, k3};
  unsigned r[4] = {0, 0, 0, 0};
  for (int i = 0; i < 17; ++i) {
    if ((addr >> i) & 1u) {
      for (int j = 0; j < 4; ++j) r[j] ^= a[j];
    }
    unsigned carry = a[3] >> 31;
    a[3] = (a[3] << 1) | (a[2] >> 31);
    a[2] = (a[2] << 1) | (a[1] >> 31);
    a[1] = (a[1] << 1) | (a[0] >> 31);
    a[0] = a[0] << 1;
    if (carry) a[0] ^= 0x87u;
  }
  return r[word & 3u];
}
