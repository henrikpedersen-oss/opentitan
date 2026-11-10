// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_OTBN_KECCAK_H_
#define EG2_MODEL_IP_EG2_OTBN_KECCAK_H_

#include <cstdint>
#include <cstring>
#include <vector>
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"

namespace eg2_model {

// A Keccak-f[1600] sponge (FIPS 202) for the KMAC interface, on digestpp's permutation (the KMAC
// block's DV reference, hw/ip/kmac/dv/dpi/vendor).
class OtbnKeccak {
 public:
  void reset(unsigned rate) {
    std::memset(a_, 0, sizeof a_);
    rate_ = rate;
    pos_ = 0;
  }
  void absorb(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
      xor_byte(pos_++, p[i]);
      if (pos_ == rate_) {
        digestpp::detail::sha3_functions::transform<24>(a_);
        pos_ = 0;
      }
    }
  }
  void absorb(const std::vector<uint8_t> &v) { absorb(v.data(), v.size()); }
  // bytepad(x, rate) (NIST SP 800-185 2.3.3).
  void absorb_bytepad(const std::vector<uint8_t> &x) {
    absorb(left_encode(rate_));
    absorb(x);
    while (pos_) {
      uint8_t z = 0;
      absorb(&z, 1);
    }
  }
  void finish(uint8_t domain) {
    xor_byte(pos_, domain);
    xor_byte(rate_ - 1, 0x80);
    digestpp::detail::sha3_functions::transform<24>(a_);
    pos_ = 0;
  }
  // The next byte of output, permuting at the end of each rate.
  uint8_t squeeze() {
    if (pos_ == rate_) {
      digestpp::detail::sha3_functions::transform<24>(a_);
      pos_ = 0;
    }
    uint8_t b = uint8_t(a_[pos_ / 8] >> (8 * (pos_ % 8)));
    ++pos_;
    return b;
  }
  static std::vector<uint8_t> left_encode(uint64_t x) {
    std::vector<uint8_t> b;
    do {
      b.insert(b.begin(), uint8_t(x));
      x >>= 8;
    } while (x);
    b.insert(b.begin(), uint8_t(b.size()));
    return b;
  }
  static std::vector<uint8_t> right_encode(uint64_t x) {
    std::vector<uint8_t> b;
    do {
      b.insert(b.begin(), uint8_t(x));
      x >>= 8;
    } while (x);
    b.push_back(uint8_t(b.size()));
    return b;
  }
  static std::vector<uint8_t> encode_string(const std::vector<uint8_t> &s) {
    std::vector<uint8_t> e = left_encode(uint64_t(s.size()) * 8);
    e.insert(e.end(), s.begin(), s.end());
    return e;
  }

 private:
  void xor_byte(unsigned i, uint8_t b) { a_[i / 8] ^= uint64_t(b) << (8 * (i % 8)); }
  uint64_t a_[25] = {};
  unsigned rate_ = 136, pos_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTBN_KECCAK_H_
