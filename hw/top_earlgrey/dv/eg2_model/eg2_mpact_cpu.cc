// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_mpact_cpu.h"

#include <cstdio>
#include <cstring>
#include <utility>

#include "eg2_system.h"
#include "eg2_targets.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

namespace {

constexpr uint32_t kGranule = 8;

// The tagged memories and where their tags are in sram_ctrl_meta's RAM (the meta SRAM map).
struct TagRegion {
  uint32_t base, size, meta_offset;
};
const TagRegion kTagRegions[] = {
    // Main and sec SRAM, one region.
    {TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
     TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES + TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES,
     0x8c00},
    {TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR, TOP_EARLGREY_RRAM_CTRL_HOST_SIZE_BYTES, 0x0c00},
};
static_assert(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR ==
                  TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR +
                      TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES,
              "main and sec SRAM are one tagged region");

uint32_t rotl(uint32_t x, unsigned s) { return s ? (x << s) | (x >> (32 - s)) : x; }
uint32_t rotr(uint32_t x, unsigned s) { return s ? (x >> s) | (x << (32 - s)) : x; }
uint32_t clmul(uint32_t a, uint32_t b, int shift) {  // bits [shift+31:shift] of the product
  uint64_t r = 0;
  for (int i = 0; i < 32; ++i) {
    if ((b >> i) & 1) r ^= uint64_t(a) << i;
  }
  return uint32_t(r >> shift);
}
uint32_t brev8(uint32_t x) {
  uint32_t r = 0;
  for (int i = 0; i < 32; ++i) r |= ((x >> i) & 1) << ((i & ~7) | (7 - (i & 7)));
  return r;
}
uint32_t zip(uint32_t x) {
  uint32_t r = 0;
  for (int i = 0; i < 16; ++i) {
    r |= ((x >> i) & 1) << (2 * i) | ((x >> (i + 16)) & 1) << (2 * i + 1);
  }
  return r;
}
uint32_t unzip(uint32_t x) {
  uint32_t r = 0;
  for (int i = 0; i < 16; ++i) {
    r |= ((x >> (2 * i)) & 1) << i | ((x >> (2 * i + 1)) & 1) << (i + 16);
  }
  return r;
}
uint32_t xperm(uint32_t a, uint32_t b, unsigned w) {  // xperm4 (w 4), xperm8 (w 8)
  uint32_t r = 0, mask = (1u << w) - 1;
  for (unsigned i = 0; i < 32; i += w) {
    uint32_t idx = (b >> i) & mask;
    if (idx * w < 32) r |= ((a >> (idx * w)) & mask) << i;
  }
  return r;
}

// The result of a 32-bit Zba/Zbb/Zbs/Zbc/Zbkb/Zbkx instruction (the RISC-V bit-manipulation
// specification v1.0 and Zbkb/Zbkx of the scalar cryptography specification v1.0.1), from rs1 (a)
// and rs2 (b); false for any other instruction.
bool bitmanip(uint32_t insn, uint32_t a, uint32_t b, uint32_t *out) {
  uint32_t f3 = (insn >> 12) & 7, f7 = insn >> 25, sh = (insn >> 20) & 31, imm = insn >> 20;
  if ((insn & 0x7f) == 0x33) {
    switch (f7 << 3 | f3) {
      case 0x20 << 3 | 7: *out = a & ~b; return true;              // andn
      case 0x20 << 3 | 6: *out = a | ~b; return true;              // orn
      case 0x20 << 3 | 4: *out = ~(a ^ b); return true;            // xnor
      case 0x10 << 3 | 2: *out = (a << 1) + b; return true;        // sh1add
      case 0x10 << 3 | 4: *out = (a << 2) + b; return true;        // sh2add
      case 0x10 << 3 | 6: *out = (a << 3) + b; return true;        // sh3add
      case 0x05 << 3 | 4: *out = int32_t(a) < int32_t(b) ? a : b; return true;  // min
      case 0x05 << 3 | 5: *out = a < b ? a : b; return true;       // minu
      case 0x05 << 3 | 6: *out = int32_t(a) > int32_t(b) ? a : b; return true;  // max
      case 0x05 << 3 | 7: *out = a > b ? a : b; return true;       // maxu
      case 0x05 << 3 | 1: *out = clmul(a, b, 0); return true;      // clmul
      case 0x05 << 3 | 2: *out = clmul(a, b, 31); return true;     // clmulr
      case 0x05 << 3 | 3: *out = clmul(a, b, 32); return true;     // clmulh
      case 0x30 << 3 | 1: *out = rotl(a, b & 31); return true;     // rol
      case 0x30 << 3 | 5: *out = rotr(a, b & 31); return true;     // ror
      case 0x14 << 3 | 1: *out = a | (1u << (b & 31)); return true;   // bset
      case 0x24 << 3 | 1: *out = a & ~(1u << (b & 31)); return true;  // bclr
      case 0x34 << 3 | 1: *out = a ^ (1u << (b & 31)); return true;   // binv
      case 0x24 << 3 | 5: *out = (a >> (b & 31)) & 1; return true;    // bext
      case 0x04 << 3 | 4: *out = (b << 16) | (a & 0xffff); return true;  // pack (zext.h: rs2 0)
      case 0x04 << 3 | 7: *out = (b & 0xff) << 8 | (a & 0xff); return true;  // packh
      case 0x14 << 3 | 2: *out = xperm(a, b, 4); return true;      // xperm4
      case 0x14 << 3 | 4: *out = xperm(a, b, 8); return true;      // xperm8
    }
    return false;
  }
  if ((insn & 0x7f) != 0x13) return false;
  if (f3 == 1) {
    switch (f7) {
      case 0x30:
        switch (sh) {
          case 0: *out = a ? __builtin_clz(a) : 32; return true;   // clz
          case 1: *out = a ? __builtin_ctz(a) : 32; return true;   // ctz
          case 2: *out = __builtin_popcount(a); return true;       // cpop
          case 4: *out = uint32_t(int32_t(int8_t(a))); return true;   // sext.b
          case 5: *out = uint32_t(int32_t(int16_t(a))); return true;  // sext.h
        }
        return false;
      case 0x14: *out = a | (1u << sh); return true;               // bseti
      case 0x24: *out = a & ~(1u << sh); return true;              // bclri
      case 0x34: *out = a ^ (1u << sh); return true;               // binvi
    }
    if (imm == 0x08f) { *out = zip(a); return true; }              // zip
    return false;
  }
  if (f3 == 5) {
    if (f7 == 0x30) { *out = rotr(a, sh); return true; }           // rori
    if (f7 == 0x24) { *out = (a >> sh) & 1; return true; }         // bexti
    switch (imm) {
      case 0x287:                                                  // orc.b
        *out = 0;
        for (int i = 0; i < 32; i += 8) *out |= ((a >> i) & 0xff) ? 0xffu << i : 0;
        return true;
      case 0x698: *out = __builtin_bswap32(a); return true;        // rev8
      case 0x687: *out = brev8(a); return true;                    // brev8
      case 0x08f: *out = unzip(a); return true;                    // unzip
    }
  }
  return false;
}

}  // namespace

Eg2MpactCpu::Eg2MpactCpu(Transport transport, const Eg2MpactConfig *cfg)
    : transport_(std::move(transport)) {
  eg2_mpact_bus bus{this, &fetch_cb, &load_cb, &store_cb};
  if (cfg) {
    cfg_ = *cfg;
    board_ = true;
  }
  if (board_ && cfg_.barrier) {
    eg2_mpact_revocation rev{cfg_.heap_base, this, &revbm_cb};
    m_ = eg2_mpact_create_revocation(&bus, &rev);
  } else {
    m_ = eg2_mpact_create(&bus);
  }
}

int Eg2MpactCpu::revbm_cb(void *user, uint32_t offset, uint8_t *byte) {
  auto *self = static_cast<Eg2MpactCpu *>(user);
  return self->cfg_.revbm(offset, byte) ? 0 : 1;
}

// cpuctrlsts and secureseed (Ibex doc/03_reference/cs_registers.rst): on Earl Grey, and on a board
// whose firmware uses them (the RTOS test SoC's boot stub enables the ICache: csrsi cpuctrlsts, 1).
bool Eg2MpactCpu::ibex_csr(uint32_t insn) {
  uint32_t f3 = (insn >> 12) & 7, csr = insn >> 20;
  if ((insn & 0x7f) != 0x73 || f3 == 0 || f3 == 4 || (csr != 0x7c0 && csr != 0x7c1)) return false;
  unsigned rd = (insn >> 7) & 31, rs1 = (insn >> 15) & 31;
  if (rd >= 16 || (f3 < 4 && rs1 >= 16)) return false;  // MPACT makes it illegal (16 registers)
  uint32_t src = f3 >= 5 ? rs1 : eg2_mpact_gpr(m_, rs1);  // the immediate forms: zimm
  uint32_t old = csr == 0x7c0 ? (cpuctrlsts_ | (1u << 8)) : 0;
  uint32_t v = cpuctrlsts_;
  switch (f3 & 3) {
    case 1: v = src; break;                            // csrrw
    case 2: if (rs1) v = cpuctrlsts_ | src; break;    // csrrs
    case 3: if (rs1) v = cpuctrlsts_ & ~src; break;   // csrrc
  }
  if (csr == 0x7c0) cpuctrlsts_ = v & 0xff;
  eg2_mpact_step_emulated(m_, 4, rd, old);
  return true;
}

Eg2MpactCpu::~Eg2MpactCpu() { eg2_mpact_destroy(m_); }

void Eg2MpactCpu::handover(const Eg2Handover &s) {
  eg2_mpact_handover_state h{};
  h.pc = s.pc;
  h.mtvec = s.mtvec;
  h.mepc = s.mepc;
  h.mstatus = s.mstatus;
  h.mie = s.mie;
  h.mcause = s.mcause;
  h.mtval = s.mtval;
  h.misa = s.misa;
  cpuctrlsts_ = s.cpuctrlsts & 0xff;
  std::memcpy(h.gpr, s.gpr, sizeof h.gpr);
  eg2_mpact_handover(m_, &h);
}

void Eg2MpactCpu::set_irqs(bool meip, bool mtip, bool msip) {
  eg2_mpact_set_irqs(m_, meip, mtip, msip);
}

bool Eg2MpactCpu::interrupt_pending() {
  uint32_t mip = 0, mie = 0;
  eg2_mpact_csr(m_, "mip", &mip);
  eg2_mpact_csr(m_, "mie", &mie);
  return (mip & mie) != 0;
}

bool Eg2MpactCpu::fetch(uint32_t addr, void *data, uint32_t len) {
  if (!Eg2System::get().fetch_allowed(addr)) return false;
  return transport_(tlm::TLM_READ_COMMAND, addr, len, static_cast<uint8_t *>(data));
}

// The fetch is checked before MPACT decodes: the low half first, the high half only for a 32-bit
// instruction, so a compressed instruction at the end of a fetchable region runs.
bool Eg2MpactCpu::step() {
  last_pc_ = eg2_mpact_pc(m_);
  uint16_t lo = 0, hi = 0;
  last_fetch_fault_ = !fetch(last_pc_, &lo, 2) || ((lo & 3) == 3 && !fetch(last_pc_ + 2, &hi, 2));
  last_insn_ = (lo & 3) == 3 ? lo | uint32_t(hi) << 16 : lo;
  if (last_fetch_fault_) {
    eg2_mpact_fetch_fault(m_);
    return true;
  }
  // What MPACT-CHERIoT does not decode (an illegal instruction there) and Ibex executes: FENCE.I,
  // which has nothing to do in the model, and RV32BFull's bit manipulation (CHERIoT has 16
  // registers: an operand above x15 stays with MPACT, which makes it illegal).
  last_fence_i_ = (last_insn_ & 0x707f) == 0x100f;
  last_emulated_ = last_fence_i_;
  if (last_fence_i_) {
    eg2_mpact_step_emulated(m_, 4, 0, 0);
    return true;
  }
  if ((!board_ || cfg_.ibex_csrs) && (last_insn_ & 3) == 3 && ibex_csr(last_insn_)) {
    last_emulated_ = true;
    return true;
  }
  unsigned rd = (last_insn_ >> 7) & 31, rs1 = (last_insn_ >> 15) & 31;
  unsigned rs2 = (last_insn_ >> 20) & 31;
  uint32_t value;
  bool regs_ok = rd < 16 && rs1 < 16 && ((last_insn_ & 0x7f) != 0x33 || rs2 < 16);
  if ((last_insn_ & 3) == 3 && regs_ok &&
      bitmanip(last_insn_, eg2_mpact_gpr(m_, rs1), eg2_mpact_gpr(m_, rs2), &value)) {
    last_emulated_ = true;
    eg2_mpact_step_emulated(m_, 4, rd, value);
    return true;
  }
  return eg2_mpact_step(m_) == 0;
}

bool Eg2MpactCpu::take_back(Eg2Handover *s) {
  s->pc = eg2_mpact_pc(m_);
  for (unsigned r = 1; r < 16; ++r) s->gpr[r] = eg2_mpact_gpr(m_, r);
  // The caller filled *s with its own values: what MPACT does not give stays.
  eg2_mpact_csr(m_, "mstatus", &s->mstatus);
  eg2_mpact_csr(m_, "mie", &s->mie);
  eg2_mpact_csr(m_, "mcause", &s->mcause);
  eg2_mpact_csr(m_, "mtval", &s->mtval);
  s->cpuctrlsts = cpuctrlsts_;
  return true;
}

bool Eg2MpactCpu::wfi_done() const { return !last_fetch_fault_ && eg2_mpact_wfi(m_); }

uint32_t Eg2MpactCpu::pc() { return eg2_mpact_pc(m_); }

std::string Eg2MpactCpu::error() const { return eg2_mpact_error(m_); }

std::string Eg2MpactCpu::trace() {
  char buf[200];
  if (last_fetch_fault_) {
    std::snprintf(buf, sizeof buf, "core   0: 0x%08x instruction access fault (MPACT)", last_pc_);
  } else if (last_emulated_) {
    bool csr = (last_insn_ & 0x7f) == 0x73;
    std::snprintf(buf, sizeof buf, "core   0: 0x%08x (0x%08x) %s (MPACT, executed by eg2sim)",
                  last_pc_, last_insn_,
                  last_fence_i_ ? "fence.i" : csr ? "Ibex custom CSR" : "bit manipulation");
  } else {
    std::snprintf(buf, sizeof buf, "core   0: 0x%08x (0x%08x) %s (MPACT)", last_pc_, last_insn_,
                  eg2_mpact_disasm(m_, last_pc_));
  }
  return buf;
}

uint8_t *Eg2MpactCpu::tag_byte(uint32_t addr, unsigned *bit) {
  if (board_ && cfg_.meta) {
    for (const Eg2MpactConfig::TagRegion &r : cfg_.tag_regions) {
      if (addr < r.base || addr - r.base >= r.size) continue;
      uint32_t g = (addr - r.base) / kGranule;
      uint32_t off = r.meta_offset + g / 8;
      if (off >= cfg_.meta->size()) return nullptr;
      *bit = g % 8;
      return &(*cfg_.meta)[off];
    }
    return nullptr;
  }
  if (!meta_) {
    auto it = Eg2System::get().memories.find("sram_ctrl_meta.ram");
    if (it == Eg2System::get().memories.end()) return nullptr;
    meta_ = it->second;
  }
  for (const TagRegion &r : kTagRegions) {
    if (addr < r.base || addr - r.base >= r.size) continue;
    uint32_t g = (addr - r.base) / kGranule;
    uint32_t off = r.meta_offset + g / 8;
    if (off >= meta_->size()) return nullptr;
    *bit = g % 8;
    return &meta_->data()[off];
  }
  return nullptr;
}

int Eg2MpactCpu::fetch_cb(void *user, uint32_t addr, void *data, uint32_t len) {
  return static_cast<Eg2MpactCpu *>(user)->fetch(addr, data, len) ? 0 : 1;
}

int Eg2MpactCpu::load_cb(void *user, uint32_t addr, void *data, uint32_t len, uint8_t *tags) {
  auto *self = static_cast<Eg2MpactCpu *>(user);
  if (!self->transport_(tlm::TLM_READ_COMMAND, addr, len, static_cast<uint8_t *>(data))) return 1;
  for (uint32_t i = 0; tags && i < len / kGranule; ++i) {
    unsigned bit;
    uint8_t *t = self->tag_byte(addr + i * kGranule, &bit);
    tags[i] = t ? (*t >> bit) & 1 : 0;
  }
  return 0;
}

int Eg2MpactCpu::store_cb(void *user, uint32_t addr, const void *data, uint32_t len,
                          const uint8_t *tags) {
  auto *self = static_cast<Eg2MpactCpu *>(user);
  if (!self->transport_(tlm::TLM_WRITE_COMMAND, addr, len,
                        const_cast<uint8_t *>(static_cast<const uint8_t *>(data)))) {
    return 1;
  }
  // A capability store sets each granule's tag to the capability's; any other store clears the
  // tag of every granule it writes.
  for (uint32_t a = addr & ~(kGranule - 1), i = 0; a < addr + len; a += kGranule, ++i) {
    unsigned bit;
    if (uint8_t *t = self->tag_byte(a, &bit)) {
      bool set = tags && tags[i];
      *t = uint8_t((*t & ~(1u << bit)) | (set ? 1u << bit : 0));
    }
  }
  return 0;
}
