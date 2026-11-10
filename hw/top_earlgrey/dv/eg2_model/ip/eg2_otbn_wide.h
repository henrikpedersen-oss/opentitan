// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_OTBN_WIDE_H_
#define EG2_MODEL_IP_EG2_OTBN_WIDE_H_

#include <cstdint>

namespace eg2_model {

// A 256-bit value (a WDR or WSR), q[0] the least significant 64 bits.
struct OtbnWide {
  uint64_t q[4] = {0, 0, 0, 0};

  uint32_t word(unsigned i) const { return uint32_t(q[i / 2] >> (32 * (i % 2))); }
  void set_word(unsigned i, uint32_t v) {
    unsigned s = 32 * (i % 2);
    q[i / 2] = (q[i / 2] & ~(uint64_t(0xffffffffu) << s)) | (uint64_t(v) << s);
  }
  bool zero() const { return !(q[0] | q[1] | q[2] | q[3]); }
  bool bit(unsigned i) const { return (q[i / 64] >> (i % 64)) & 1; }
  bool operator==(const OtbnWide &o) const {
    return q[0] == o.q[0] && q[1] == o.q[1] && q[2] == o.q[2] && q[3] == o.q[3];
  }

  static OtbnWide shl(const OtbnWide &a, unsigned n) {
    OtbnWide r;
    if (n >= 256) return r;
    unsigned w = n / 64, b = n % 64;
    for (int i = 3; i >= int(w); --i) {
      r.q[i] = a.q[i - w] << b;
      if (b && i - int(w) - 1 >= 0) r.q[i] |= a.q[i - w - 1] >> (64 - b);
    }
    return r;
  }
  static OtbnWide shr(const OtbnWide &a, unsigned n) {
    OtbnWide r;
    if (n >= 256) return r;
    unsigned w = n / 64, b = n % 64;
    for (unsigned i = 0; i + w < 4; ++i) {
      r.q[i] = a.q[i + w] >> b;
      if (b && i + w + 1 < 4) r.q[i] |= a.q[i + w + 1] << (64 - b);
    }
    return r;
  }
  // a + b + cin; *cout the carry out of bit 255.
  static OtbnWide add(const OtbnWide &a, const OtbnWide &b, unsigned cin, unsigned *cout) {
    OtbnWide r;
    unsigned __int128 c = cin;
    for (unsigned i = 0; i < 4; ++i) {
      c += (unsigned __int128)a.q[i] + b.q[i];
      r.q[i] = uint64_t(c);
      c >>= 64;
    }
    if (cout) *cout = unsigned(c);
    return r;
  }
  // a - b - bin; *bout 1 when the result is negative (bit 256 of the two's complement result).
  static OtbnWide sub(const OtbnWide &a, const OtbnWide &b, unsigned bin, unsigned *bout) {
    OtbnWide r;
    unsigned borrow = bin;
    for (unsigned i = 0; i < 4; ++i) {
      uint64_t d = a.q[i] - b.q[i];
      unsigned b1 = a.q[i] < b.q[i];
      uint64_t e = d - borrow;
      unsigned b2 = d < uint64_t(borrow);
      r.q[i] = e;
      borrow = b1 | b2;
    }
    if (bout) *bout = borrow;
    return r;
  }
  static bool less(const OtbnWide &a, const OtbnWide &b) {
    for (int i = 3; i >= 0; --i) {
      if (a.q[i] != b.q[i]) return a.q[i] < b.q[i];
    }
    return false;
  }
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_OTBN_WIDE_H_
