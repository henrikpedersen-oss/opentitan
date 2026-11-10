// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_ENTROPY_COMMON_H_
#define EG2_MODEL_IP_EG2_ENTROPY_COMMON_H_

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "../eg2_ip_model.h"
#include "hw/ip/aes/model/aes.h"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha3.hpp"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"
#include "../../eg2_rng/eg2_rng_source.h"

namespace eg2_model {

// The noise source (`eg2sim --rng <source>`, ../../eg2_rng/eg2_rng_source.h); null: the
// deterministic generator below. With a source, entropy_src takes its seeds from the source's
// 4-bit symbols as the theory of operation describes: in FIPS mode SHA3-384 over
// HEALTH_TEST_WINDOWS.FIPS_WINDOW symbols (two windows for the first seed after the block is
// enabled, the startup seed), otherwise (boot-time mode) the 96 symbols of 384 raw bits. The
// symbols are packed in arrival order, the first in the least significant nibble of the first
// byte; the seed's bytes are taken as the NIST byte string csrng uses. The packing and the byte
// order the hardware uses are not documented; the chip bench's comparison settles them.
inline eg2_rng_t *&eg2_noise_source() {
  static eg2_rng_t *src = nullptr;
  return src;
}

inline bool mubi4_true(uint32_t v) { return (v & 0xf) == 0x6; }

// A NIST byte string of n bytes, B1 (most significant) first, as words least significant first.
inline void nist_from_words(const uint32_t *w, unsigned nwords, uint8_t *b, unsigned nbytes) {
  std::memset(b, 0, nbytes);
  for (unsigned i = 0; i < nwords && 4 * (i + 1) <= nbytes; ++i) {
    unsigned at = nbytes - 4 * (i + 1);
    b[at] = uint8_t(w[i] >> 24);
    b[at + 1] = uint8_t(w[i] >> 16);
    b[at + 2] = uint8_t(w[i] >> 8);
    b[at + 3] = uint8_t(w[i]);
  }
}
inline uint32_t nist_word(const uint8_t *b, unsigned nbytes, unsigned i) {
  unsigned at = nbytes - 4 * (i + 1);
  return (uint32_t(b[at]) << 24) | (uint32_t(b[at + 1]) << 16) | (uint32_t(b[at + 2]) << 8) |
         b[at + 3];
}

// ---------------------------------------------------------------------------------------------
// CTR_DRBG (SP 800-90A 10.2.1, AES-256, no derivation function).
struct Eg2CtrDrbg {
  static constexpr unsigned kSeedLen = 48;
  uint8_t key[32] = {};
  uint8_t v[16] = {};
  uint32_t reseed_counter = 0;
  bool instantiated = false;
  bool fips = false;

  static void inc(uint8_t *v) {
    for (int i = 15; i >= 0 && ++v[i] == 0; --i) {
    }
  }
  void block(uint8_t *out) {
    inc(v);
    aes_encrypt_block(v, key, 32, out);
  }
  void update(const uint8_t *provided) {
    uint8_t temp[kSeedLen];
    for (unsigned i = 0; i < kSeedLen; i += 16) block(temp + i);
    for (unsigned i = 0; i < kSeedLen; ++i) temp[i] ^= provided[i];
    std::memcpy(key, temp, 32);
    std::memcpy(v, temp + 32, 16);
  }
  void instantiate(const uint8_t *seed, bool fips_seed) {
    std::memset(key, 0, sizeof key);
    std::memset(v, 0, sizeof v);
    update(seed);
    reseed_counter = 0;
    instantiated = true;
    fips = fips_seed;
  }
  void reseed(const uint8_t *seed, bool fips_seed) {
    update(seed);
    reseed_counter = 0;
    fips = fips_seed;
  }
  void uninstantiate() { *this = Eg2CtrDrbg(); }
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_ENTROPY_COMMON_H_
