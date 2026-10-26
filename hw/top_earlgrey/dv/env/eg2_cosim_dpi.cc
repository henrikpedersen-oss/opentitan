// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// EG2 additions to Ibex's co-simulation DPI (hw/vendor/lowrisc_ibex/dv/cosim/cosim_dpi.cc).
//
// eg2_cosim_load_elf: copy every loadable segment of a 32-bit RISC-V ELF into the co-simulator's
// memory at its load address (p_paddr: the LMA, so initialised data lands where the image keeps
// it and the start-up code copies it, as on the chip). Spike fetches its instructions from this
// memory, so it runs the same ROM and test images the bench loads into the chip.
// Returns the number of bytes written, or -1 on an error (reported on stderr). The ranges written
// are kept for eg2_cosim_elf_covers until eg2_cosim_elf_forget (review R6, 2026-10-25), and
// their bytes for eg2_cosim_elf_byte and eg2_cosim_elf_restore (reviews R15, R19, 2026-10-26).
//
// eg2_cosim_num_pending_dside / eg2_cosim_flush_icache: Spike's queue of the chip's unmatched
// data-side accesses and its decoded-instruction cache (reviews R9 and R11, 2026-10-25).
//
// The handover to CHERIoT-Sail at the CHERIoT lock (REQ_SOC_07) takes its state from Spike, the
// RISC-V-mode model, never from the chip:
//   eg2_cosim_get_gpr / get_pc / get_csr: Spike's registers, PC and CSRs;
//   eg2_cosim_copy_mem_to_cheriot_sail: Spike's memory over [base, base + size) into CHERIoT-Sail's
//     (the images and every store made in RISC-V mode). Only non-zero bytes: CHERIoT-Sail has just
//     been initialised and its memory is zero. Returns the number of bytes copied.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

#include "cosim.h"
#include "spike_cosim.h"

// CHERIoT-Sail co-simulation (ibex_cheriot_verification, ibex/dv/cosim/cheriot_sail).
extern "C" void cheriot_sail_cosim_write_mem_byte(const uint32_t *addr, const uint32_t *data);

namespace {

template <typename T>
bool read_at(const std::vector<uint8_t> &elf, size_t off, T *out) {
  if (off + sizeof(T) > elf.size()) return false;
  std::memcpy(out, elf.data() + off, sizeof(T));
  return true;
}

// The [base, base + size) ranges eg2_cosim_load_elf wrote since the last eg2_cosim_elf_forget:
// the memory whose contents the model has from an image (eg2_cosim_elf_covers). Review R6,
// 2026-10-25.
std::vector<std::pair<uint32_t, uint32_t>> s_loaded;
// Their bytes, segment by segment (s_loaded's order): the image's value of an RRAM word for a
// controller Rewrite (eg2_cosim_elf_byte, review R15) and the image put back after the bench
// reloads the RRAM (eg2_cosim_elf_restore, review R19). 2026-10-26.
std::vector<std::vector<uint8_t>> s_loaded_bytes;

}  // namespace

// The image's byte at `addr` (the last segment loaded there), or -1 where no image covers it.
// Review R15, 2026-10-26.
extern "C" int eg2_cosim_elf_byte(uint32_t addr) {
  for (size_t i = s_loaded.size(); i-- > 0;) {
    const auto &r = s_loaded[i];
    if (addr >= r.first && uint64_t(addr) < uint64_t(r.first) + r.second) {
      return s_loaded_bytes[i][addr - r.first];
    }
  }
  return -1;
}

// Writes the images' bytes inside [base, base + size) into the model again: Spike's memory, or
// with to_sail CHERIoT-Sail's (cosim_handle unused). The bench put the image back into the chip
// (chip_sw_eg2_sec_rma_key_change_vseq, chip_sw_lc_walkthrough_vseq reload the RRAM after RMA
// wiped it). Returns the number of bytes written, -1 if Spike has no memory there. Review R19,
// 2026-10-26.
extern "C" int eg2_cosim_elf_restore(void *cosim_handle, uint32_t base, uint32_t size,
                                     int to_sail) {
  int total = 0;
  const uint64_t lo = base, hi = uint64_t(base) + size;
  for (size_t i = 0; i < s_loaded.size(); ++i) {
    uint64_t s = std::max<uint64_t>(lo, s_loaded[i].first);
    uint64_t e = std::min<uint64_t>(hi, uint64_t(s_loaded[i].first) + s_loaded[i].second);
    if (s >= e) continue;
    const uint8_t *src = s_loaded_bytes[i].data() + (s - s_loaded[i].first);
    if (to_sail) {
      for (uint64_t a = s; a < e; ++a) {
        uint32_t aa = uint32_t(a), d = src[a - s];
        cheriot_sail_cosim_write_mem_byte(&aa, &d);
      }
    } else if (!static_cast<Cosim *>(cosim_handle)
                    ->backdoor_write_mem(uint32_t(s), size_t(e - s), src)) {
      std::fprintf(stderr, "eg2_cosim_elf_restore: no co-simulator memory at 0x%08x\n",
                   uint32_t(s));
      return -1;
    }
    total += int(e - s);
  }
  return total;
}

// Whether `addr` is inside a segment eg2_cosim_load_elf loaded (1) or not (0). eg2_iss_wrapper's
// load check: a load from such a byte is compared with the image, not taken from the chip.
extern "C" int eg2_cosim_elf_covers(uint32_t addr) {
  for (const auto &r : s_loaded) {
    if (addr >= r.first && uint64_t(addr) < uint64_t(r.first) + r.second) return 1;
  }
  return 0;
}

// Forgets the loaded ranges (a new Spike is loaded at each chip reset).
extern "C" void eg2_cosim_elf_forget(void) {
  s_loaded.clear();
  s_loaded_bytes.clear();  // R15, R19, 2026-10-26
}

extern "C" int eg2_cosim_load_elf(void *cosim_handle, const char *path) {
  auto cosim = static_cast<Cosim *>(cosim_handle);

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "eg2_cosim_load_elf: cannot open %s\n", path);
    return -1;
  }
  std::vector<uint8_t> elf((std::istreambuf_iterator<char>(f)), {});

  // ELF32 little-endian header: e_ident[0..3] = 0x7f 'E' 'L' 'F', EI_CLASS = 1 (32-bit),
  // EI_DATA = 1 (little-endian).
  if (elf.size() < 52 || std::memcmp(elf.data(), "\x7f" "ELF", 4) != 0 || elf[4] != 1 ||
      elf[5] != 1) {
    std::fprintf(stderr, "eg2_cosim_load_elf: %s is not a 32-bit little-endian ELF\n", path);
    return -1;
  }
  uint32_t phoff;
  uint16_t phentsize, phnum;
  read_at(elf, 28, &phoff);
  read_at(elf, 42, &phentsize);
  read_at(elf, 44, &phnum);

  int total = 0;
  for (uint16_t i = 0; i < phnum; ++i) {
    size_t ph = phoff + size_t(i) * phentsize;
    uint32_t type, offset, paddr, filesz;
    if (!read_at(elf, ph + 0, &type) || !read_at(elf, ph + 4, &offset) ||
        !read_at(elf, ph + 12, &paddr) || !read_at(elf, ph + 16, &filesz)) {
      std::fprintf(stderr, "eg2_cosim_load_elf: %s: truncated program header\n", path);
      return -1;
    }
    const uint32_t kPtLoad = 1;
    if (type != kPtLoad || filesz == 0) continue;
    if (size_t(offset) + filesz > elf.size()) {
      std::fprintf(stderr, "eg2_cosim_load_elf: %s: segment %u past the end of the file\n", path,
                   i);
      return -1;
    }
    if (!cosim->backdoor_write_mem(paddr, filesz, elf.data() + offset)) {
      std::fprintf(stderr, "eg2_cosim_load_elf: %s: no co-simulator memory at 0x%08x\n", path,
                   paddr);
      return -1;
    }
    s_loaded.emplace_back(paddr, filesz);  // R6, 2026-10-25
    s_loaded_bytes.emplace_back(elf.begin() + offset, elf.begin() + offset + filesz);  // R15
    total += filesz;
  }
  return total;
}

namespace {

SpikeCosim *spike(void *cosim_handle) {
  return dynamic_cast<SpikeCosim *>(static_cast<Cosim *>(cosim_handle));
}

}  // namespace

extern "C" uint32_t eg2_cosim_get_gpr(void *cosim_handle, int idx) {
  return spike(cosim_handle)->get_gpr(idx);
}

extern "C" uint32_t eg2_cosim_get_pc(void *cosim_handle) { return spike(cosim_handle)->get_pc(); }

extern "C" uint32_t eg2_cosim_get_csr(void *cosim_handle, int csr_num) {
  return spike(cosim_handle)->get_csr(csr_num);
}

// The chip's data-side accesses Spike has been told of and has not matched with one of its own
// (review R9, 2026-10-25): eg2_iss_wrapper checks that none is left behind.
extern "C" int eg2_cosim_num_pending_dside(void *cosim_handle) {
  return int(spike(cosim_handle)->get_num_pending_dside_accesses());
}

// Drops Spike's decoded instructions after memory that may hold code was written behind its back
// (review R11, 2026-10-25).
extern "C" void eg2_cosim_flush_icache(void *cosim_handle) { spike(cosim_handle)->flush_icache(); }

extern "C" int eg2_cosim_copy_mem_to_cheriot_sail(void *cosim_handle, uint32_t base,
                                                  uint32_t size) {
  auto cosim = static_cast<Cosim *>(cosim_handle);
  const uint32_t kChunk = 4096;
  std::vector<uint8_t> buf(kChunk);
  int copied = 0;
  for (uint64_t off = 0; off < size; off += kChunk) {
    uint32_t n = uint32_t(std::min<uint64_t>(kChunk, size - off));
    uint32_t addr = uint32_t(base + off);
    if (!cosim->backdoor_read_mem(addr, n, buf.data())) {
      std::fprintf(stderr, "eg2_cosim_copy_mem_to_cheriot_sail: no Spike memory at 0x%08x\n", addr);
      return -1;
    }
    for (uint32_t i = 0; i < n; ++i) {
      if (buf[i] == 0) continue;
      uint32_t a = addr + i, d = buf[i];
      cheriot_sail_cosim_write_mem_byte(&a, &d);
      ++copied;
    }
  }
  return copied;
}
