// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "eg2_elf.h"
#include "eg2_regblock.h"
#include "eg2_targets.h"

namespace eg2_sim {

// Copy every loadable segment of a 32-bit ELF to its load address (p_paddr); collect its symbols.
bool load_elf(const std::string &path, std::vector<Eg2Memory *> &mems,
              std::map<std::string, uint32_t> *symbols, bool segments) {
  eg2_model::ElfImage image;
  if (!image.open(path)) {
    std::fprintf(stderr, "eg2sim: %s: %s\n", path.c_str(), image.error().c_str());
    return false;
  }
  if (symbols) {
    for (const auto &entry : image.symbols()) (*symbols)[entry.first] = entry.second.address;
  }
  if (!segments) return true;
  // Validate every destination before changing memory. BSS is deliberately not
  // loaded: boot code initializes runtime memory from these physical load images.
  std::vector<Eg2Memory *> destinations;
  for (const auto &segment : image.segments()) {
    if (!segment.file_size) continue;
    Eg2Memory *destination = nullptr;
    for (auto *memory : mems) {
      if (segment.physical >= memory->base() &&
          uint64_t(segment.physical - memory->base()) + segment.file_size <= memory->size()) {
        destination = memory;
        break;
      }
    }
    if (!destination) {
      std::fprintf(stderr, "eg2sim: %s: segment at 0x%08x is in no memory\n",
                   path.c_str(), segment.physical);
      return false;
    }
    destinations.push_back(destination);
  }
  size_t index = 0;
  for (const auto &segment : image.segments()) {
    if (segment.file_size)
      destinations[index++]->write(segment.physical, image.data(segment), segment.file_size);
  }
  return true;
}

// Load a raw RRAM image (the bench's <test>_rom_ext_assembled.img: ROM_EXT and the signed
// application, as the secure-boot runs place them) from the RRAM's base.
bool load_rram_image(const std::string &path, Eg2Memory *rram) {
  std::ifstream f(path, std::ios::binary);
  if (!f || !rram) {
    std::fprintf(stderr, "eg2sim: cannot load %s into RRAM\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> img((std::istreambuf_iterator<char>(f)), {});
  if (img.empty() || img.size() > rram->size() || !rram->write(rram->base(), img.data(), img.size())) {
    std::fprintf(stderr, "eg2sim: %s (%zu bytes) does not fit the RRAM (%u bytes)\n", path.c_str(),
                 img.size(), rram->size());
    return false;
  }
  return true;
}

// Load an OTP image (vmem, "@<word address> <24-bit word>": data in bits 15:0) into otp_ctrl's
// software read window.
bool load_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks) {
  std::ifstream f(path);
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot open %s\n", path.c_str());
    return false;
  }
  Eg2RegBlock *otp = nullptr;
  for (Eg2RegBlock *b : blocks) {
    if (std::string(b->desc().window) == "otp_ctrl.core") otp = b;
  }
  if (!otp) return false;
  std::string line;
  size_t words = 0;
  while (std::getline(f, line)) {
    unsigned addr, word;
    if (std::sscanf(line.c_str(), "@%x %x", &addr, &word) != 2) continue;
    uint8_t b[2] = {uint8_t(word), uint8_t(word >> 8)};
    if (otp->win_write("SW_CFG_WINDOW", 2 * addr, b, 2)) ++words;
  }
  std::printf("eg2sim: OTP image %s: %zu words in otp_ctrl.core SW_CFG_WINDOW\n", path.c_str(),
              words);
  return words > 0;
}

// --otp-out: the OTP array as it is at the end of the run, in the image format --otp reads
// (gen-otp-img.py: one 16-bit word per line with its 6 ECC bits, OTP's extended Hamming code of
// hw/ip/lc_ctrl/data/lc_ctrl_state.hjson "secded"), so that a life-cycle walkthrough can go on in
// the next run, and the chip bench can load it.
uint32_t otp_ecc_word(uint16_t d) {
  static const std::vector<std::vector<int>> kMatrix = {
      {0, 1, 3, 4, 6, 8, 10, 11, 13, 15}, {0, 2, 3, 5, 6, 9, 10, 12, 13},
      {1, 2, 3, 7, 8, 9, 10, 14, 15},     {4, 5, 6, 7, 8, 9, 10},
      {11, 12, 13, 14, 15},               {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10,
                                           11, 12, 13, 14, 15, 16, 17, 18, 19, 20}};
  uint32_t w = d;
  for (size_t j = 0; j < kMatrix.size(); ++j) {
    uint32_t b = 0;
    for (int k : kMatrix[j]) b ^= (w >> k) & 1;
    w |= b << (16 + j);
  }
  return w;
}

bool save_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks) {
  Eg2RegBlock *otp = nullptr;
  for (Eg2RegBlock *b : blocks) {
    if (std::string(b->desc().window) == "otp_ctrl.core") otp = b;
  }
  int w = otp ? otp->win_index("SW_CFG_WINDOW") : -1;
  FILE *f = w >= 0 ? std::fopen(path.c_str(), "w") : nullptr;
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot write %s\n", path.c_str());
    return false;
  }
  const std::vector<uint8_t> &m = otp->win_mem(uint16_t(w));
  const size_t words = 0x810 / 2;  // the OTP array (otp_ctrl_mmap.md), kEg2OtpSize
  std::fprintf(f, "//\n// OTP MEM file with %zu x 24bit layout (eg2sim --otp-out)\n", words);
  for (size_t i = 0; i < words && 2 * i + 1 < m.size(); ++i) {
    std::fprintf(f, "@%06zx %06x\n", i, otp_ecc_word(uint16_t(m[2 * i] | (m[2 * i + 1] << 8))));
  }
  std::fclose(f);
  std::printf("eg2sim: OTP written to %s\n", path.c_str());
  return true;
}

// The size of a symbol of a 32-bit ELF (st_size), or 0 if there is no such symbol.
uint32_t elf_symbol_size(const std::string &path, const std::string &symbol) {
  eg2_model::ElfImage image;
  if (!image.open(path)) return 0;
  const auto found = image.symbols().find(symbol);
  return found == image.symbols().end() ? 0 : found->second.size;
}

bool elf_symbol_bytes(const std::string &path, const std::string &symbol, size_t len,
                      std::vector<uint8_t> *out) {
  eg2_model::ElfImage image;
  return image.open(path) && image.symbol_bytes(symbol, len, out);
}

// Initialized data may have different runtime and physical load addresses.
uint32_t elf_load_address(const std::string &path, uint32_t vaddr) {
  eg2_model::ElfImage image;
  return image.open(path) ? image.load_address(vaddr) : vaddr;
}

// Write a 32-bit little-endian word at a symbol of the test image, as the bench's
// sw_symbol_backdoor_overwrite does.
bool write_symbol(const std::map<std::string, uint32_t> &syms, std::vector<Eg2Memory *> &mems,
                  const std::string &name, uint32_t value) {
  auto it = syms.find(name);
  if (it == syms.end()) {
    std::fprintf(stderr, "eg2sim: the test image has no symbol %s\n", name.c_str());
    return false;
  }
  uint8_t b[4] = {uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24)};
  for (Eg2Memory *m : mems) {
    if (m->write(it->second, b, 4)) {
      std::printf("eg2sim: %s = %u (0x%08x) at 0x%08x\n", name.c_str(), value, value, it->second);
      return true;
    }
  }
  std::fprintf(stderr, "eg2sim: %s at 0x%08x is in no memory\n", name.c_str(), it->second);
  return false;
}

}  // namespace eg2_sim
