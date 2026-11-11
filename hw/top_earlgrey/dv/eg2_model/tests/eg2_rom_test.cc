// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include <cstdio>
#include <fstream>
#include "eg2_rom_image.h"
#include "rom_fixture.h"
#include "ip/eg2_rom_ctrl.h"
#include <tlm_utils/simple_initiator_socket.h>

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *why) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", why); }
}
struct Bus : sc_core::sc_module {
  tlm_utils::simple_initiator_socket<Bus> socket;
  explicit Bus(sc_core::sc_module_name name) : sc_module(name), socket("socket") {}
};
}
int sc_main(int argc, char **argv) {
  if (argc == 3 && std::string(argv[1]) == "--fixture") return write_rom_fixture(argv[2]) ? 0 : 1;
  auto &system = Eg2System::get();
  auto &image = system.rom_image;
  image.customization = "ROM_CTRL";
  image.words.resize(16);
  for (unsigned i = 0; i < 8; ++i) image.words[i] = (i + 1) | (uint64_t(i + 2) << 32);
  // Independent oracle: PyCryptodome cSHAKE256, raw words encoded as five LE bytes.
  const std::array<uint32_t, 8> expected = {0x4949b184, 0x12480701, 0x5dddb9a8, 0xec24fb19,
                                           0x27ca7547, 0x128c9694, 0x4f6c13e6, 0x6d4e714a};
  for (unsigned i = 0; i < 8; ++i) image.words[8 + i] = expected[i];
  check(image.computed_digest() == expected, "cSHAKE input includes all 39 bits in logical order");
  Eg2Memory memory("rom", 0, 64);
  system.memories["rom_ctrl.rom"] = &memory;
  const eg2_regs::Block *description = nullptr;
  for (const auto &block : eg2_regs::kBlocks)
    if (std::string(block.window) == "rom_ctrl.regs") description = &block;
  if (!description) return 1;
  eg2_model::Eg2RomCtrl rom("rom_ctrl", *description, 0);
  Bus registers("registers"), data("data");
  registers.socket.bind(rom.socket); data.socket.bind(memory.socket);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  check(Eg2ModelStop::reason().empty(), "valid independent ROM image does not stop");
  const auto digest = rom.reg_index("DIGEST_0"), stored = rom.reg_index("EXP_DIGEST_0");
  check(rom.value(digest) == expected[0] && rom.value(stored) == expected[0], "computed and stored digests agree");
  image.words[8] ^= 1;
  rom.on_reset(Eg2ResetCause::kPor);
  check(rom.value(digest) == expected[0] && rom.value(stored) == (expected[0] ^ 1),
        "changing only stored digest cannot change computed digest");
  image.words[8] ^= 1; image.words[0] ^= uint64_t(1) << 38;
  rom.on_reset(Eg2ResetCause::kPor);
  check(rom.value(digest) != expected[0] && rom.value(stored) == expected[0],
        "an integrity-bit change changes the computed hash without changing expectation");
  image.words.clear();
  rom.on_reset(Eg2ResetCause::kPor);
  check(Eg2ModelStop::reason().find("not modelled: ROM checker") == 0,
        "missing checker input stops instead of inventing an expected digest");
  std::ofstream("fixture.elf", std::ios::binary) << "independent fixture";
  check(write_rom_fixture("fixture.elf"), "write test-only independent fixture");
  eg2_model::Eg2RomImage loaded;
  check(loaded.load("fixture.elf.romcheck", "fixture.elf", 49152), "load complete checker image");
  check(loaded.computed_digest()[0] == uint32_t(loaded.words[49152 - 8]), "zero-ROM independent known answer");
  std::ofstream("fixture.elf", std::ios::binary) << "changed executable";
  check(!loaded.load("fixture.elf.romcheck", "fixture.elf", 49152), "reject checker image for a different ELF");
  std::remove("fixture.elf"); std::remove("fixture.elf.romcheck");
  system.close_logs();
  std::printf("eg2_rom_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
