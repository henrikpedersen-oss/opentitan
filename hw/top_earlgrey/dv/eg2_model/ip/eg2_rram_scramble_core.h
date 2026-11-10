// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl scrambling: the reference model of rram_scramble, the address mask and the cipher of
// the XEX construction, with the scrambler's key registers. Used by the scrambling unit bench
// (hw/ip/rram_ctrl/dv/edagen/rram_scramble, through dpi/eg2_rram_dpi.cc); eg2sim keeps the RRAM
// unscrambled (scrambling is not visible to software through rram_ctrl) and can use it for
// backdoor images.
//
// From the documentation, never from the RTL:
//   hw/ip/rram_ctrl/doc/theory_of_operation.md "RRAM Scrambling":
//     addr_tweak = GF_MULT(word_addr, addr_key); ciphertext = PRINCE(addr_tweak ^ plaintext,
//     data_key) ^ addr_tweak. "two interleaved 64-bit PRINCE cipher instances operating on the full
//     128-bit RRAM word"; "The two PRINCE cipher instances are identical, interchangeable engines
//     using the same key"; cipher_switch "swaps which instance processes the even-indexed and
//     odd-indexed data bits" (so the result does not depend on it). rram_scramble supplies the mask
//     (the tweak) and the cipher; the XORs with the tweak are its users'.
//     Key management: reset values RndCnstAddrKey / RndCnstDataKey (netlist constants: the bench
//     passes the values it builds the unit with); keys_valid latches addr_key / data_key;
//     keys_disable "any value other than MuBi4False" switches to rand_addr_key / rand_data_key
//     ("preventing the real sideloaded keys from being used", so it wins over keys_valid).
//   hw/ip/prim/doc/prim_prince.md: PRINCE as the original paper (eprint 2012/529) with the
//     improved key schedule that alternates k0 and k1 (eprint 2014/656, appendix C), the
//     primitive's default; 2 + 2 * 5 non-linear layers (the paper's 12). The paper's key is
//     k0 || k1, k0 the upper half. The C reference is OpenTitan's prince_ref.h (checked against
//     the paper's test vectors, eg2_rram_dpi_test.cc).
//
// Not in the documentation (findings drafted in status/rtl_tickets.md); the model takes:
//   - GF_MULT's field: GF(2^128) with x^128 + x^7 + x^2 + x + 1 (ScrambleIPoly of rram_ctrl_pkg,
//     a design constant, as the keys), in the polynomial basis with bit i the coefficient of x^i;
//   - word_addr: the unit's address input, zero-extended to 128 bits;
//   - "even-indexed bits": bit i of the first instance's block is word bit 2i, of the second 2i+1.

#ifndef EG2_RRAM_SCRAMBLE_CORE_H_
#define EG2_RRAM_SCRAMBLE_CORE_H_

#include <stdint.h>
#include <string.h>

#include <cstdint>

// In an unnamed namespace: each translation unit has its own copy, apart from the one
// crypto_dpi_prince.c exports.
namespace eg2_prince {
namespace {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "../../../../ip/prim/dv/prim_prince/crypto_dpi_prince/prince_ref.h"
#pragma GCC diagnostic pop
}  // namespace
}  // namespace eg2_prince

namespace eg2_model {

using rram_u128 = unsigned __int128;

struct RramScramble {
  static constexpr uint32_t kMuBi4False = 0x9;
  static constexpr rram_u128 kIPoly = 0x87;  // x^7 + x^2 + x + 1 (x^128 implied)
  static constexpr int kHalfRounds = 5;

  rram_u128 addr_key, data_key;
  uint32_t epoch = 0;  // counts key changes: an operation spanning one is not predicted

  RramScramble(rram_u128 rst_addr_key, rram_u128 rst_data_key)
      : addr_key(rst_addr_key), data_key(rst_data_key) {}

  // One clock edge of the key registers.
  void step(uint32_t keys_disable, bool keys_valid, rram_u128 ak, rram_u128 dk, rram_u128 rak,
            rram_u128 rdk) {
    rram_u128 na = addr_key, nd = data_key;
    if ((keys_disable & 0xf) != kMuBi4False) {
      na = rak;
      nd = rdk;
    } else if (keys_valid) {
      na = ak;
      nd = dk;
    }
    if (na != addr_key || nd != data_key) ++epoch;
    addr_key = na;
    data_key = nd;
  }

  static rram_u128 gf_mul(rram_u128 a, rram_u128 b) {
    rram_u128 r = 0;
    for (int i = 0; i < 128; ++i) {
      if ((b >> i) & 1) r ^= a;
      bool carry = (a >> 127) & 1;
      a <<= 1;
      if (carry) a ^= kIPoly;
    }
    return r;
  }

  rram_u128 mask(uint32_t word_addr) const { return gf_mul(rram_u128(word_addr), addr_key); }

  static uint64_t prince(uint64_t x, rram_u128 key, bool decrypt, bool old_key_schedule = false) {
    return eg2_prince::prince_enc_dec_uint64(x, uint64_t(key >> 64), uint64_t(key), decrypt ? 1 : 0,
                                             kHalfRounds, old_key_schedule ? 1 : 0);
  }

  // The 128-bit cipher: even bits through one PRINCE, odd bits through the other.
  static rram_u128 cipher(rram_u128 d, rram_u128 key, bool decrypt) {
    uint64_t lane[2] = {0, 0};
    for (int i = 0; i < 64; ++i)
      for (int k = 0; k < 2; ++k) lane[k] |= uint64_t((d >> (2 * i + k)) & 1) << i;
    rram_u128 out = 0;
    for (int k = 0; k < 2; ++k) {
      uint64_t c = prince(lane[k], key, decrypt);
      for (int i = 0; i < 64; ++i) out |= rram_u128((c >> i) & 1) << (2 * i + k);
    }
    return out;
  }

  rram_u128 op(rram_u128 d, bool decrypt) const { return cipher(d, data_key, decrypt); }

  // The whole XEX construction (the users' part included), for backdoor images.
  rram_u128 scramble(uint32_t word_addr, rram_u128 plain) const {
    rram_u128 t = mask(word_addr);
    return op(plain ^ t, false) ^ t;
  }
  rram_u128 descramble(uint32_t word_addr, rram_u128 c) const {
    rram_u128 t = mask(word_addr);
    return op(c ^ t, true) ^ t;
  }
};

}  // namespace eg2_model

#endif  // EG2_RRAM_SCRAMBLE_CORE_H_
