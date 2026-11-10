// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <vector>
#include "ip/eg2_otp_crypto.h"

int main() {
  using Crypto = eg2_model::OtpCrypto;
  unsigned checks = 0, failures = 0;
  auto check = [&](bool ok, const char *what) {
    ++checks;
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
  };
  // Published PRESENT-128 known-answer vectors (also in util/design/lib/Present.py).
  check(Crypto::encrypt(0, {0, 0}) == 0x96db702a2e6900afull, "PRESENT zero key/plaintext");
  check(Crypto::encrypt(~uint64_t(0), {~uint64_t(0), ~uint64_t(0)}) == 0x628d9fbd4218e5b4ull,
        "PRESENT all-one key/plaintext");
  check(Crypto::decrypt(0x96db702a2e6900afull, {0, 0}) == 0, "PRESENT decrypt known ciphertext");
  // Independent oracle: util/design/lib/Present.py with the image generator's
  // _present_64bit_digest construction and the public simulation constants.
  const size_t lengths[] = {0, 8, 16, 24, 64, 80};
  const uint64_t expected[] = {0x888ad8b273b33b29ull, 0x8498ab707618ec51ull,
      0x1af82a6916e33120ull, 0xd732be27bc259c46ull, 0x8831fafaeedcc6feull, 0xe08f302f0281e4d2ull};
  std::vector<uint8_t> bytes(80);
  for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = uint8_t(i);
  for (unsigned i = 0; i < 6; ++i)
    check(Crypto::digest(bytes.data(), lengths[i]) == expected[i], "partition digest known answer");
  bool rejected = false;
  try { Crypto::digest(bytes.data(), 7); } catch (const std::invalid_argument &) { rejected = true; }
  check(rejected, "reject incomplete block");
  bytes[0] ^= 1;
  check(Crypto::digest(bytes.data(), 64) != expected[4], "changed partition fails digest comparison");
  std::printf("eg2_otp_crypto_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
