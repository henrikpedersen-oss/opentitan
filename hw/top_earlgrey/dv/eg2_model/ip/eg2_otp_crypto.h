// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_OTP_CRYPTO_H_
#define EG2_MODEL_IP_EG2_OTP_CRYPTO_H_

#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace eg2_model {

// PRESENT-128, 31 rounds plus whitening. Shared by the SystemC controller and
// the plain C++ core; neither obtains expected data from observed DUT traffic.
class OtpCrypto {
 public:
  struct Key {
    uint64_t hi, lo;
  };
  static Key next_key(Key k, unsigned round) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t hi = ((k.hi & 7) << 61) | (k.lo >> 3);
    uint64_t lo = ((k.lo & 7) << 61) | (k.hi >> 3);
    hi = (hi & 0x00ffffffffffffffull) | (uint64_t(kSbox[hi >> 60]) << 60) |
         (uint64_t(kSbox[(hi >> 56) & 0xf]) << 56);
    hi ^= uint64_t(round) >> 2;
    lo ^= uint64_t(round) << 62;
    return {hi, lo};
  }
  static uint64_t inverse_sbox(uint64_t data) {
    static constexpr uint8_t kInv[16] = {0x5, 0xe, 0xf, 0x8, 0xc, 0x1, 0x2, 0xd,
                                         0xb, 0x4, 0x6, 0x3, 0x0, 0x7, 0x9, 0xa};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kInv[(data >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t inverse_permutation(uint64_t data) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) {
      unsigned to = i == 63 ? 63 : (4 * i) % 63;
      out |= ((data >> i) & 1) << to;
    }
    return out;
  }
  static uint64_t substitute(uint64_t data) {
    static constexpr uint8_t kSbox[16] = {0xc, 0x5, 0x6, 0xb, 0x9, 0x0, 0xa, 0xd,
                                          0x3, 0xe, 0xf, 0x8, 0x4, 0x7, 0x1, 0x2};
    uint64_t out = 0;
    for (unsigned i = 0; i < 16; ++i) out |= uint64_t(kSbox[(data >> (4 * i)) & 0xf]) << (4 * i);
    return out;
  }
  static uint64_t permutation(uint64_t data) {
    uint64_t out = 0;
    for (unsigned i = 0; i < 64; ++i) {
      unsigned to = i == 63 ? 63 : (16 * i) % 63;
      out |= ((data >> i) & 1) << to;
    }
    return out;
  }

  static uint64_t encrypt(uint64_t data, Key key) {
    for (unsigned round = 1; round <= 31; ++round) {
      data = permutation(substitute(data ^ key.hi));
      key = next_key(key, round);
    }
    return data ^ key.hi;
  }

  static uint64_t decrypt(uint64_t data, Key key) {
    Key keys[32];
    keys[0] = key;
    for (unsigned i = 1; i < 32; ++i) keys[i] = next_key(keys[i - 1], i);
    data ^= keys[31].hi;
    for (int i = 30; i >= 0; --i) data = inverse_sbox(inverse_permutation(data)) ^ keys[i].hi;
    return data;
  }

  // Public simulation constants, not production secrets. Source:
  // hw/top_earlgrey/data/autogen/top_earlgrey.secrets.testing.gen.hjson,
  // RndCnstScrmblKey{0,1,2}, RndCnstDigestIV0 and RndCnstDigestConst0.
  // otp_ctrl_mmap.hjson assigns CnstyDigest (index 0) to partition digests.
  static Key partition_key(unsigned secret_index) {
    static constexpr Key keys[] = {
        {0xf9089cf2787c6c14ull, 0xb7d1a6c0618e831full},
        {0xc8129e7a4149ba28ull, 0x31fe2e9fb3b2a1d2ull},
        {0x5254f861ba80f82bull, 0x303da30635aa3d3aull},
    };
    if (secret_index >= 3) throw std::out_of_range("OTP secret partition key");
    return keys[secret_index];
  }

  // otp_ctrl theory_of_operation.md, "Digest Calculation": Davies-Meyer
  // compression, duplicate a trailing 64-bit block, then finalize. Data is the
  // stored (scrambled for secret partitions) byte representation, little endian.
  static uint64_t digest(const uint8_t *bytes, size_t length) {
    if (length % 8 || (length && !bytes))
      throw std::invalid_argument("OTP digest requires complete 64-bit blocks");
    uint64_t state = 0x87f0d5ee2b8b9db7ull;
    for (size_t offset = 0; offset < length; offset += 16) {
      const uint64_t lo = read64(bytes + offset);
      const uint64_t hi = length - offset >= 16 ? read64(bytes + offset + 8) : lo;
      state ^= encrypt(state, {hi, lo});
    }
    return state ^ encrypt(state, {0xd82dedcd5992c873ull, 0x952d50c51d21f7f8ull});
  }

 private:
  static uint64_t read64(const uint8_t *bytes) {
    uint64_t word = 0;
    for (unsigned i = 0; i < 8; ++i) word |= uint64_t(bytes[i]) << (8 * i);
    return word;
  }
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTP_CRYPTO_H_
