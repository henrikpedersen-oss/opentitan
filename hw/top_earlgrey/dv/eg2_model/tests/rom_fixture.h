// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#ifndef EG2_MODEL_TESTS_ROM_FIXTURE_H_
#define EG2_MODEL_TESTS_ROM_FIXTURE_H_
#include <fstream>
#include "eg2_rom_image.h"

// Synthetic, independently hashed checker input for tests with an empty ELF.
// Oracle: PyCryptodome cSHAKE256(bytes((49152-8)*5), custom=b"ROM_CTRL").
// This is a test fixture only; executable ROMs use prepare_rom_check.py instead.
inline bool write_rom_fixture(const std::string &elf) {
  const uint32_t expected[] = {0x59e50157, 0x9ba22b9c, 0x616d3471, 0x19a7bcdc,
                              0x942fe27c, 0x74f09410, 0x7d961c8d, 0x57d80348};
  std::ofstream out(elf + ".romcheck");
  out << "EG2_ROM_CHECKER_V1\nelf_sha256 " << eg2_model::Eg2RomImage::elf_hash(elf)
      << "\nfunction_name -\ncustomization 524f4d5f4354524c\nwords 49152\n";
  for (unsigned i = 0; i < 49152 - 8; ++i) out << "0\n";
  for (uint32_t word : expected) out << std::hex << word << '\n';
  return bool(out);
}
#endif
