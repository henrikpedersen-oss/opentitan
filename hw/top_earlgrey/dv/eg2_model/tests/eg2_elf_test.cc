// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include "eg2_elf.h"

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *what) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
void put(std::vector<uint8_t> &bytes, size_t at, uint32_t value, unsigned length = 4) {
  for (unsigned i = 0; i < length; ++i) bytes[at + i] = uint8_t(value >> (8 * i));
}
std::vector<uint8_t> fixture() {
  std::vector<uint8_t> bytes(256, 0);
  std::memcpy(bytes.data(), "\177ELF\1\1\1", 7);
  put(bytes, 16, 2, 2); put(bytes, 18, 243, 2); put(bytes, 20, 1);
  put(bytes, 28, 52); put(bytes, 32, 96);
  put(bytes, 40, 52, 2); put(bytes, 42, 32, 2); put(bytes, 44, 1, 2);
  put(bytes, 46, 40, 2); put(bytes, 48, 3, 2);
  put(bytes, 52, 1); put(bytes, 56, 84);  // PT_LOAD, file offset
  put(bytes, 60, 0x2000); put(bytes, 64, 0x1000);
  put(bytes, 68, 4); put(bytes, 72, 8); put(bytes, 84, 0x12345678);
  put(bytes, 140, 2); put(bytes, 152, 216); put(bytes, 156, 16);
  put(bytes, 160, 2); put(bytes, 172, 16);  // SYMTAB -> section 2
  put(bytes, 180, 3); put(bytes, 192, 232); put(bytes, 196, 8);
  put(bytes, 216, 1); put(bytes, 220, 0x2000); put(bytes, 224, 4);
  put(bytes, 230, 1, 2);  // defined symbol
  std::memcpy(bytes.data() + 233, "value", 6);
  return bytes;
}
}  // namespace

int main() {
  eg2_model::ElfImage image;
  auto bytes = fixture();
  check(image.parse(bytes), "valid ELF with different physical and virtual addresses");
  check(image.segments().size() == 1, "one loadable segment");
  check(image.load_address(0x2002) == 0x1002, "symbol maps to load image");
  check(image.load_address(0x2004) == 0x2004, "BSS has no file bytes");
  std::vector<uint8_t> value;
  check(image.symbol_bytes("value", 4, &value) && value == std::vector<uint8_t>({0x78,0x56,0x34,0x12}),
        "symbol bytes are little endian");
  check(!image.symbol_bytes("value", 5, &value), "symbol read cannot spill into its neighbour");
  auto bad = bytes; bad[5] = 2;
  check(!image.parse(bad), "reject big endian");
  bad = bytes; put(bad, 18, 62, 2);
  check(!image.parse(bad), "reject non-RISC-V executable");
  bad = bytes; put(bad, 28, 0xfffffff0);
  check(!image.parse(bad), "reject program table outside file");
  bad = bytes; put(bad, 42, 0, 2);
  check(!image.parse(bad), "reject zero program-header stride");
  bad = bytes; put(bad, 56, 255);
  check(!image.parse(bad), "reject truncated segment");
  bad = bytes; put(bad, 72, 3);
  check(!image.parse(bad), "file size cannot exceed memory size");
  bad = bytes; put(bad, 64, 0xfffffffc);
  check(!image.parse(bad), "reject wrapping segment address");
  bad = bytes; put(bad, 160, 3);
  check(!image.parse(bad), "reject symbol link outside section table");
  bad = bytes; put(bad, 172, 0);
  check(!image.parse(bad), "reject zero symbol stride");
  bad = bytes; put(bad, 196, 6);
  check(!image.parse(bad), "terminator must lie inside string table");
  bad = bytes; put(bad, 216, 0xffffffff);
  check(!image.parse(bad), "reject overflowing name offset");
  check(image.segments().empty() && image.symbols().empty(), "invalid input exposes no partial image");
  bytes.resize(88); put(bytes, 32, 0); put(bytes, 48, 0, 2);
  check(image.parse(bytes), "stripped executable without section table");
  for (size_t length = 0; length < 52; ++length) {
    check(!image.parse(std::vector<uint8_t>(length)), "truncated ELF header");
  }
  std::printf("eg2_elf_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
