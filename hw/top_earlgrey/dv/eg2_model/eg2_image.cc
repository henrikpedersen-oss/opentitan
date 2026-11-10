// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_image.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "eg2_regblock.h"
#include "eg2_targets.h"

namespace eg2_sim {

// Copy every loadable segment of a 32-bit ELF to its load address (p_paddr); collect its symbols.
bool load_elf(const std::string &path, std::vector<Eg2Memory *> &mems,
              std::map<std::string, uint32_t> *symbols, bool segments) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "eg2sim: cannot open %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd32 = [&](size_t off) {
    uint32_t v = 0;
    if (off + 4 <= elf.size()) std::memcpy(&v, &elf[off], 4);
    return v;
  };
  auto rd16 = [&](size_t off) {
    uint16_t v = 0;
    if (off + 2 <= elf.size()) std::memcpy(&v, &elf[off], 2);
    return v;
  };
  if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1) {
    std::fprintf(stderr, "eg2sim: %s is not a 32-bit ELF\n", path.c_str());
    return false;
  }
  uint32_t phoff = rd32(28);
  uint16_t phentsize = rd16(42), phnum = rd16(44);
  if (symbols) {
    uint32_t shoff = rd32(32);
    uint16_t shentsize = rd16(46), shnum = rd16(48);
    for (uint16_t i = 0; i < shnum; ++i) {
      size_t sh = shoff + size_t(i) * shentsize;
      if (rd32(sh + 4) != 2) continue;  // SHT_SYMTAB
      uint32_t off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
      size_t strsh = shoff + size_t(link) * shentsize;
      uint32_t stroff = rd32(strsh + 16), strsize = rd32(strsh + 20);
      for (uint32_t e = 0; e + 16 <= size; e += 16) {
        uint32_t name = rd32(off + e), value = rd32(off + e + 4);
        if (name == 0 || name >= strsize || stroff + name >= elf.size()) continue;
        const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
        size_t len = strnlen(n, elf.size() - (stroff + name));
        (*symbols)[std::string(n, len)] = value;
      }
    }
  }
  if (!segments) return true;  // symbols only: the image is loaded another way
  for (uint16_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd32(ph), offset = rd32(ph + 4), paddr = rd32(ph + 12),
             filesz = rd32(ph + 16);
    if (type != 1 || filesz == 0) continue;
    if (size_t(offset) + filesz > elf.size()) return false;
    bool done = false;
    for (Eg2Memory *m : mems) done = done || m->write(paddr, &elf[offset], filesz);
    if (!done) {
      std::fprintf(stderr, "eg2sim: %s: segment at 0x%08x is in no memory\n", path.c_str(),
                   paddr);
      return false;
    }
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
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return 0;
  uint32_t shoff = rd(32, 4), shentsize = rd(46, 2), shnum = rd(48, 2);
  for (uint32_t i = 0; i < shnum; ++i) {
    size_t sh = shoff + size_t(i) * shentsize;
    if (rd(sh + 4, 4) != 2) continue;  // SHT_SYMTAB
    uint32_t off = rd(sh + 16, 4), size = rd(sh + 20, 4), link = rd(sh + 24, 4);
    uint32_t stroff = rd(shoff + size_t(link) * shentsize + 16, 4);
    for (uint32_t e = 0; e + 16 <= size; e += 16) {
      uint32_t name = rd(off + e, 4);
      if (name == 0 || stroff + name >= elf.size()) continue;
      const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
      if (symbol == std::string(n, strnlen(n, elf.size() - (stroff + name)))) {
        return rd(off + e + 8, 4);
      }
    }
  }
  return 0;
}

// The initial bytes of a symbol of a 32-bit ELF (its value in the file's loadable segments): false
// if there is no such symbol or its bytes are not in the file.
bool elf_symbol_bytes(const std::string &path, const std::string &symbol, size_t len,
                      std::vector<uint8_t> *out) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return false;
  uint32_t shoff = rd(32, 4), shentsize = rd(46, 2), shnum = rd(48, 2), addr = 0;
  bool found = false;
  for (uint32_t i = 0; i < shnum && !found; ++i) {
    size_t sh = shoff + size_t(i) * shentsize;
    if (rd(sh + 4, 4) != 2) continue;  // SHT_SYMTAB
    uint32_t off = rd(sh + 16, 4), size = rd(sh + 20, 4), link = rd(sh + 24, 4);
    uint32_t stroff = rd(shoff + size_t(link) * shentsize + 16, 4);
    for (uint32_t e = 0; e + 16 <= size && !found; e += 16) {
      uint32_t name = rd(off + e, 4);
      if (name == 0 || stroff + name >= elf.size()) continue;
      const char *n = reinterpret_cast<const char *>(&elf[stroff + name]);
      if (symbol == std::string(n, strnlen(n, elf.size() - (stroff + name)))) {
        addr = rd(off + e + 4, 4);
        found = true;
      }
    }
  }
  if (!found) return false;
  uint32_t phoff = rd(28, 4), phentsize = rd(42, 2), phnum = rd(44, 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd(ph, 4), offset = rd(ph + 4, 4), va = rd(ph + 8, 4), filesz = rd(ph + 16, 4);
    if (type != 1 || addr < va || addr - va + len > filesz) continue;
    size_t at = size_t(offset) + (addr - va);
    if (at + len > elf.size()) return false;
    out->assign(elf.begin() + at, elf.begin() + at + len);
    return true;
  }
  return false;
}

// The load address of run-time address vaddr in a 32-bit ELF: a symbol of an initialised data
// section (.data) lives at its run-time address only after the start-up code has copied it from
// its load image, so a write before the run must go to the load image (p_paddr). vaddr unchanged if
// no segment with a different load address holds it.
uint32_t elf_load_address(const std::string &path, uint32_t vaddr) {
  std::ifstream f(path, std::ios::binary);
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});
  auto rd = [&](size_t off, unsigned n) {
    uint32_t v = 0;
    if (off + n <= elf.size()) std::memcpy(&v, &elf[off], n);
    return v;
  };
  if (elf.size() < 52) return vaddr;
  uint32_t phoff = rd(28, 4), phentsize = rd(42, 2), phnum = rd(44, 2);
  for (uint32_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type = rd(ph, 4), va = rd(ph + 8, 4), pa = rd(ph + 12, 4), filesz = rd(ph + 16, 4);
    if (type == 1 && va != pa && vaddr >= va && vaddr - va < filesz) return pa + (vaddr - va);
  }
  return vaddr;
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
