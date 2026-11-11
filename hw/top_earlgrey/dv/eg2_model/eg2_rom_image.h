// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#ifndef EG2_MODEL_EG2_ROM_IMAGE_H_
#define EG2_MODEL_EG2_ROM_IMAGE_H_

#include <array>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/sha2.hpp"
#include "hw/ip/kmac/dv/dpi/vendor/kerukuro_digestpp/algorithm/shake.hpp"

namespace eg2_model {
// Input to the ROM checker, separate from the ELF used for instruction execution.
// ROM ToO, "The startup ROM check": 39-bit words in logical address order,
// padded to 40 bits; the last eight words contain the independent expected hash.
// This input preserves all seven stored integrity bits. It does not regenerate
// ECC or the expected digest from the data being checked.
//
// tools/prepare_rom_check.py extracts the input from an existing scrambled VMEM,
// checks its decoded body against the ELF, and records the ELF's SHA-256. N and S
// are explicit configuration inputs: the IP docs name a ROM_CTRL prefix without
// specifying its N/S split. The image producer supplies that split, never RTL.
// Missing input/configuration is a model stop, not a successful ROM validation.
struct Eg2RomImage {
  std::vector<uint64_t> words;
  std::string function_name, customization, error;

  static std::string elf_hash(const std::string &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    digestpp::sha256 hash;
    hash.absorb(bytes.data(), bytes.size());
    return hash.hexdigest();
  }

  bool load(const std::string &path, const std::string &elf, size_t count) {
    words.clear();
    error.clear();
    std::ifstream input(path);
    if (!input) return fail("not modelled: ROM checker input unavailable: " + path);
    std::string magic, key, value;
    size_t length = 0;
    if (!(input >> magic) || magic != "EG2_ROM_CHECKER_V1" ||
        !(input >> key >> value) || key != "elf_sha256" || value != elf_hash(elf))
      return fail("not modelled: ROM checker input does not identify this ELF");
    if (!(input >> key >> value) || key != "function_name" || !decode(value, function_name) ||
        !(input >> key >> value) || key != "customization" || !decode(value, customization))
      return fail("undocumented: ROM cSHAKE function-name/customization configuration missing");
    if (!(input >> key >> length) || key != "words" || count < 8 || length != count)
      return fail("not modelled: ROM checker input has the wrong word count");
    for (size_t i = 0; i < count; ++i) {
      if (!(input >> value) || value.size() > 10 || value.empty())
        return fail("not modelled: truncated ROM checker input");
      uint64_t word = 0;
      for (char digit : value) {
        const int n = nibble(digit);
        if (n < 0) return fail("not modelled: invalid ROM checker word");
        word = (word << 4) | unsigned(n);
      }
      if (word >> 39) return fail("not modelled: ROM checker word exceeds 39 bits");
      words.push_back(word);
    }
    if (input >> value) return fail("not modelled: trailing ROM checker data");
    return true;
  }

  std::array<uint32_t, 8> computed_digest() const {
    digestpp::cshake256 hash;
    hash.set_function_name(function_name);
    hash.set_customization(customization);
    for (size_t i = 0; i + 8 < words.size(); ++i) {
      uint8_t bytes[5];
      for (unsigned b = 0; b < 5; ++b) bytes[b] = uint8_t(words[i] >> (8 * b));
      hash.absorb(bytes, sizeof bytes);
    }
    uint8_t bytes[32];
    hash.squeeze(bytes, sizeof bytes);
    std::array<uint32_t, 8> digest{};
    for (unsigned i = 0; i < 32; ++i) digest[i / 4] |= uint32_t(bytes[i]) << (8 * (i % 4));
    return digest;
  }

 private:
  bool fail(const std::string &why) { words.clear(); error = why; return false; }
  static int nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  }
  static bool decode(const std::string &hex, std::string &out) {
    out.clear();
    if (hex == "-") return true;
    if (hex.size() % 2 || hex.size() > 512) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
      const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
      if (hi < 0 || lo < 0) return false;
      out.push_back(char(16 * hi + lo));
    }
    return true;
  }
};
}  // namespace eg2_model
#endif  // EG2_MODEL_EG2_ROM_IMAGE_H_
