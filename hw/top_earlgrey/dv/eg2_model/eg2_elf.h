// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_EG2_ELF_H_
#define EG2_MODEL_EG2_ELF_H_

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace eg2_model {

// The image format shared by the standalone and DPI loaders. Decode fields
// explicitly as little endian, validate the complete file before exposing any
// segments, and bound names by their string table. No SystemC dependency.
// ELF32 layout: ELF gABI, header / program header / section header / symbol table.
class ElfImage {
 public:
  struct Segment {
    uint32_t offset, physical, virtual_address, file_size, memory_size;
  };
  struct Symbol {
    uint32_t address, size;
  };

  bool open(const std::string &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("cannot open " + path);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    if (file.bad()) return fail("cannot read " + path);
    return parse(std::move(bytes));
  }

  bool parse(std::vector<uint8_t> bytes) {
    bytes_ = std::move(bytes);
    segments_.clear();
    symbols_.clear();
    error_.clear();
    if (!fits(0, 52) || std::memcmp(bytes_.data(), "\177ELF", 4) ||
        bytes_[4] != 1 || bytes_[5] != 1 || bytes_[6] != 1 ||
        word(20) != 1 || half(40) != 52 || half(18) != 243 ||
        (half(16) != 2 && half(16) != 3)) {
      return fail("expected a little-endian RISC-V ELF32 executable");
    }
    const uint32_t phoff = word(28), shoff = word(32);
    const uint16_t phsize = half(42), phnum = half(44);
    const uint16_t shsize = half(46), shnum = half(48);
    // Extended numbering is not emitted by the supported firmware toolchains.
    if (phnum == 0xffff || (phnum && (phsize != 32 || !phoff)) ||
        !fits(phoff, uint64_t(phsize) * phnum)) {
      return fail("invalid program header table");
    }
    if ((shnum && (shsize != 40 || !shoff)) || (!shnum && shoff) ||
        !fits(shoff, uint64_t(shsize) * shnum)) {
      return fail("invalid section header table");
    }
    for (unsigned i = 0; i < phnum; ++i) {
      const size_t at = phoff + size_t(i) * phsize;
      if (word(at) != 1) continue;  // PT_LOAD
      Segment segment{word(at + 4), word(at + 12), word(at + 8),
                      word(at + 16), word(at + 20)};
      if (segment.file_size > segment.memory_size ||
          !fits(segment.offset, segment.file_size) ||
          uint64_t(segment.physical) + segment.memory_size > (uint64_t(1) << 32) ||
          uint64_t(segment.virtual_address) + segment.memory_size > (uint64_t(1) << 32)) {
        return fail("invalid loadable segment");
      }
      segments_.push_back(segment);
    }
    for (unsigned i = 0; i < shnum; ++i) {
      const size_t at = shoff + size_t(i) * shsize;
      const uint32_t type = word(at + 4), offset = word(at + 16), size = word(at + 20);
      if (type != 8 && !fits(offset, size)) return fail("section outside file"); // SHT_NOBITS
      if (type != 2) continue;  // SHT_SYMTAB
      const uint32_t link = word(at + 24);
      if (word(at + 36) != 16 || size % 16 || link >= shnum) {
        return fail("invalid symbol table");
      }
      const size_t strings = shoff + size_t(link) * shsize;
      const uint32_t string_offset = word(strings + 16), string_size = word(strings + 20);
      if (word(strings + 4) != 3 || !fits(string_offset, string_size)) {
        return fail("invalid symbol string table");
      }
      for (size_t pos = offset; pos < uint64_t(offset) + size; pos += 16) {
        const uint32_t name = word(pos);
        if (!name || half(pos + 14) == 0) continue;  // unnamed / undefined
        if (name >= string_size) return fail("symbol name outside string table");
        const char *start = reinterpret_cast<const char *>(bytes_.data() + string_offset + name);
        const void *end = std::memchr(start, 0, string_size - name);
        if (!end) return fail("unterminated symbol name");
        symbols_[std::string(start, static_cast<const char *>(end) - start)] =
            {word(pos + 4), word(pos + 8)};
      }
    }
    return true;
  }

  const std::string &error() const { return error_; }
  const std::vector<Segment> &segments() const { return segments_; }
  const std::map<std::string, Symbol> &symbols() const { return symbols_; }
  const uint8_t *data(const Segment &segment) const { return bytes_.data() + segment.offset; }

  bool symbol_bytes(const std::string &name, size_t length, std::vector<uint8_t> *out) const {
    const auto found = symbols_.find(name);
    if (found == symbols_.end() || !out || length > found->second.size) return false;
    for (const auto &segment : segments_) {
      if (found->second.address < segment.virtual_address) continue;
      const uint64_t offset = uint64_t(found->second.address) - segment.virtual_address;
      if (offset <= segment.file_size && length <= segment.file_size - offset) {
        const auto *start = data(segment) + offset;
        out->assign(start, start + length);
        return true;
      }
    }
    return false;
  }

  uint32_t load_address(uint32_t address) const {
    for (const auto &segment : segments_) {
      if (address >= segment.virtual_address && address - segment.virtual_address < segment.file_size)
        return segment.physical + (address - segment.virtual_address);
    }
    return address;
  }

 private:
  bool fits(uint64_t offset, uint64_t length) const {
    return offset <= bytes_.size() && length <= bytes_.size() - offset;
  }
  uint16_t half(size_t at) const { return uint16_t(bytes_[at]) | uint16_t(bytes_[at + 1]) << 8; }
  uint32_t word(size_t at) const { return uint32_t(half(at)) | uint32_t(half(at + 2)) << 16; }
  bool fail(const std::string &why) {
    error_ = why;
    segments_.clear();
    symbols_.clear();
    return false;
  }

  std::vector<uint8_t> bytes_;
  std::vector<Segment> segments_;
  std::map<std::string, Symbol> symbols_;
  std::string error_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_EG2_ELF_H_
