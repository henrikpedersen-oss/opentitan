// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_isa[_cheriot] (EG2 spec "Core and instruction set" and "Core in CHERIoT mode";
 * REQ_CIB_06).
 *
 * Random instruction streams, drawn by the software from the run's seed and executed from sec
 * SRAM, with the chip's own fetch path (bus, iCache), latencies and interrupts. The checker is the
 * scoreboard: every instruction against the reference model (Spike in RISC-V mode, the CHERIoT
 * model in CHERIoT mode). The test's own check is liveness: the blocks returned and the
 * instructions generated against those planned.
 *
 * `n_instr` instructions in blocks of up to 256, each ending in `jalr x0, 0(ra)` and run by
 * eg2_isa_run with x5-x15 preset from operand classes (`w_operand_<c>`: 0, 1, -1, the most
 * negative and positive values, a power of two and its neighbours, bits shared with another
 * operand, uniform; by the suite's weight rule). Per instruction: the group (`w_group_<g>`:
 * integer 25, multiply and divide 10, Zba/Zbb/Zbs 15, Zbc/Zbkb/Zbkx 5, compressed 15, Zcmp 5,
 * CSR 10, branch and jump 15), the instruction, its registers (destinations x5-x15; a source at
 * `w_hazard_<d>` distance 0-3 from an earlier destination, or none), its immediate by class.
 * Branches and jumps go forward inside the block. Knobs `dummy_instr`, `icache`: cpuctrlsts.
 *
 * CHERIoT mode: the CHERIoT ISA's integer part (x0-x15) and the bit-manipulation extensions the
 * core implements in CHERIoT mode; CSR accesses with PCC's SR permission (mcycle, minstret,
 * mcause, mtval) and `w_csr`'s share of SCR reads through CSpecialRW (MTCC, MTDC, MScratchC,
 * MEPCC); `cap_reg_pct` of the operand registers hold a tagged capability. Zcb and Zcmp are RISC-V
 * mode only (not in the CHERIoT ISA, and Zcmp needs the scoreboard's step g).
 *
 * Not in this test: instructions that trap (illegal encodings, CSRs without access) are
 * chip_sw_eg2_rand_traps'; loads and stores are chip_sw_eg2_rand_mem's. `u_mode_pct` is accepted
 * but streams run in M mode only (a gap: running a block in U mode needs a return path through
 * the trap handler that eg2_probe does not provide for code that writes x5-x15).
 *
 * The SystemC model's MPACT-CHERIoT keeps decoded instructions by address (eg2_mpact_cpu.h): with
 * `model_gaps` the stream is cut so that no address is written twice.
 *
 * Built with EG2_RAND_ISA_INJECT_FAIL, one more block is planned than run: the run must fail with
 * `EG2 rand_isa: <n>/<n+1> done`.
 */

#include "hw/top/sram_ctrl_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

// CHERIoT mode: the boot stage enables execution from main and sec SRAM before it locks sram_ctrl
// (eg2_cheriot_start.S step 0b, REQ_SOC_08); this test runs code there.
const volatile uint8_t eg2_cheriot_boot_sram_exec __attribute__((used)) = 1;

EG2_TEST_CONFIG();

#define RAND_ISA_PARAMS(X)       \
  X(n_instr, 0, 0, 20000)        \
  X(w_group_int, 25, 0, 100)     \
  X(w_group_muldiv, 10, 0, 100)  \
  X(w_group_zb, 15, 0, 100)      \
  X(w_group_zbk, 5, 0, 100)      \
  X(w_group_c, 15, 0, 100)       \
  X(w_group_zcmp, 5, 0, 100)     \
  X(w_group_csr, 10, 0, 100)     \
  X(w_group_branch, 15, 0, 100)  \
  X(boost, 0, 0, 255)            \
  X(u_mode_pct, 0, 0, 50)        \
  X(dummy_instr, 2, 0, 2)        \
  X(icache, 2, 0, 2)             \
  X(cap_reg_pct, 101, 0, 101)    \
  X(w_csr_scr, 30, 0, 100)       \
  X(w_csr_machine, 70, 0, 100)   \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(RAND_ISA_PARAMS);

uint32_t eg2_isa_run(void *entry, void *operands);

enum {
  kGInt = 0,
  kGMulDiv,
  kGZb,
  kGZbk,
  kGC,
  kGZcmp,
  kGCsr,
  kGBranch,
  kGCount,
  kMaxBlock = 256,
  kOpClasses = 9,
  kCodeBase = kEg2SecBase,
  kCodeSize = kEg2SecSize - 0x1000,  // the last 4 KiB hold the operands
  kOperands = kEg2SecBase + kEg2SecSize - 0x1000,
  kRet = 0x00008067,  // jalr x0, 0(x1)
};

static eg2_tally_t tally;
// The class families, on the stack of eg2_test_main (the image's globals stay under 4 KiB, F21).
static eg2_family_t *groups_p, *operands_p, *hazards_p;
static uint32_t code_next;  // the next free byte of the code area

// ------------------------------------------ Encoders -----------------------------------------

static uint32_t r_type(uint32_t f7, uint32_t rs2, uint32_t rs1, uint32_t f3, uint32_t rd,
                       uint32_t op) {
  return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t i_type(uint32_t imm, uint32_t rs1, uint32_t f3, uint32_t rd, uint32_t op) {
  return ((imm & 0xfff) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static uint32_t b_type(uint32_t off, uint32_t rs2, uint32_t rs1, uint32_t f3) {
  return (((off >> 12) & 1) << 31) | (((off >> 5) & 0x3f) << 25) | (rs2 << 20) | (rs1 << 15) |
         (f3 << 12) | (((off >> 1) & 0xf) << 8) | (((off >> 11) & 1) << 7) | 0x63;
}
static uint32_t j_type(uint32_t off, uint32_t rd) {
  return (((off >> 20) & 1) << 31) | (((off >> 1) & 0x3ff) << 21) | (((off >> 11) & 1) << 20) |
         (((off >> 12) & 0xff) << 12) | (rd << 7) | 0x6f;
}

// ------------------------------------------ Draws ---------------------------------------------

static uint32_t reg_dest(void) { return eg2_rand_range(5, 15); }
static uint32_t reg_any(void) { return eg2_pick(5) == 0 ? eg2_rand_range(0, 4) : eg2_rand_range(5, 15); }
static uint32_t reg_c(void) { return eg2_rand_range(0, 7); }  // x8-x15 as rd'/rs'

static uint32_t operand_value(uint32_t other) {
  switch (eg2_family_pick(operands_p)) {
    case 0:
      return 0;
    case 1:
      return 1;
    case 2:
      return 0xffffffffu;
    case 3:
      return 0x80000000u;
    case 4:
      return 0x7fffffffu;
    case 5:
      return 1u << eg2_pick(32);
    case 6:
      return (1u << eg2_pick(32)) + (eg2_pick(2) ? 1 : 0xffffffffu);
    case 7:
      return other ^ (1u << eg2_pick(32));
    default:
      return eg2_rand();
  }
}

static uint32_t imm12(void) {
  switch (eg2_pick(5)) {
    case 0:
      return 0;
    case 1:
      return 1;
    case 2:
      return 0xfff;  // -1
    case 3:
      return eg2_pick(2) ? 0x7ff : 0x800;
    default:
      return eg2_rand() & 0xfff;
  }
}

// --------------------------------------- Instructions -----------------------------------------

/** One instruction of group `g`; returns its length (2 or 4) and writes it to *insn. */
static uint32_t draw_insn(uint32_t g, uint32_t rd, uint32_t rs1, uint32_t *insn) {
  uint32_t rs2 = reg_any();
  uint32_t sh = eg2_pick(32);
  static const uint8_t kRf3[10] = {0, 0, 1, 2, 3, 4, 5, 5, 6, 7};
  switch (g) {
    case kGInt: {
      uint32_t k = eg2_pick(5);
      if (k == 0) {
        uint32_t i = eg2_pick(10);
        *insn = r_type(i == 1 || i == 7 ? 0x20 : 0, rs2, rs1, kRf3[i], rd, 0x33);
      } else if (k == 1) {
        static const uint8_t kIf3[6] = {0, 2, 3, 4, 6, 7};
        *insn = i_type(imm12(), rs1, kIf3[eg2_pick(6)], rd, 0x13);
      } else if (k == 2) {
        uint32_t s = eg2_pick(3);
        *insn = i_type((s == 2 ? 0x400 : 0) | sh, rs1, s == 0 ? 1 : 5, rd, 0x13);
      } else if (k == 3) {
        *insn = ((eg2_rand() & 0xfffff) << 12) | (rd << 7) | 0x37;  // lui
      } else {
        *insn = ((eg2_rand() & 0xfffff) << 12) | (rd << 7) | 0x17;  // auipc (AUIPCC)
      }
      return 4;
    }
    case kGMulDiv:
      *insn = r_type(1, rs2, rs1, eg2_pick(8), rd, 0x33);
      return 4;
    case kGZb: {
      switch (eg2_pick(10)) {
        case 0:  // sh1add, sh2add, sh3add
          *insn = r_type(0x10, rs2, rs1, 2 * eg2_rand_range(1, 3), rd, 0x33);
          return 4;
        case 1: {  // andn, orn, xnor
          static const uint8_t kF3[3] = {7, 6, 4};
          *insn = r_type(0x20, rs2, rs1, kF3[eg2_pick(3)], rd, 0x33);
          return 4;
        }
        case 2: {  // clz, ctz, cpop, sext.b, sext.h
          static const uint16_t kImm[5] = {0x600, 0x601, 0x602, 0x604, 0x605};
          *insn = i_type(kImm[eg2_pick(5)], rs1, 1, rd, 0x13);
          return 4;
        }
        case 3: {  // min, minu, max, maxu
          *insn = r_type(5, rs2, rs1, 4 + eg2_pick(4), rd, 0x33);
          return 4;
        }
        case 4:  // zext.h
          *insn = r_type(4, 0, rs1, 4, rd, 0x33);
          return 4;
        case 5:  // rol, ror
          *insn = r_type(0x30, rs2, rs1, eg2_pick(2) ? 1 : 5, rd, 0x33);
          return 4;
        case 6: {  // rori, orc.b, rev8
          uint32_t k = eg2_pick(3);
          *insn = i_type(k == 0 ? 0x600 | sh : k == 1 ? 0x287 : 0x698, rs1, 5, rd, 0x13);
          return 4;
        }
        case 7: {  // bclr, bext, binv, bset
          static const uint8_t kF7[4] = {0x24, 0x24, 0x34, 0x14};
          uint32_t k = eg2_pick(4);
          *insn = r_type(kF7[k], rs2, rs1, k == 1 ? 5 : 1, rd, 0x33);
          return 4;
        }
        default: {  // bclri, bexti, binvi, bseti
          static const uint16_t kImm[4] = {0x480, 0x480, 0x680, 0x280};
          uint32_t k = eg2_pick(4);
          *insn = i_type(kImm[k] | sh, rs1, k == 1 ? 5 : 1, rd, 0x13);
          return 4;
        }
      }
    }
    case kGZbk: {
      switch (eg2_pick(5)) {
        case 0:  // clmul, clmulr, clmulh
          *insn = r_type(5, rs2, rs1, eg2_rand_range(1, 3), rd, 0x33);
          return 4;
        case 1:  // pack, packh
          *insn = r_type(4, rs2, rs1, eg2_pick(2) ? 4 : 7, rd, 0x33);
          return 4;
        case 2:  // brev8
          *insn = i_type(0x687, rs1, 5, rd, 0x13);
          return 4;
        case 3:  // zip, unzip
          *insn = i_type(0x08f, rs1, eg2_pick(2) ? 1 : 5, rd, 0x13);
          return 4;
        default:  // xperm4, xperm8
          *insn = r_type(0x14, rs2, rs1, eg2_pick(2) ? 2 : 4, rd, 0x33);
          return 4;
      }
    }
    case kGC: {
      uint32_t imm6 = eg2_rand() & 0x3f;
      uint32_t rdc = reg_c(), rs2c = reg_c();
#ifdef __CHERIOT__
      uint32_t kinds = 7;
#else
      uint32_t kinds = 9;  // with Zcb
#endif
      switch (eg2_pick(kinds)) {
        case 0:  // c.addi
          *insn = 0x0001 | (((imm6 >> 5) & 1) << 12) | (rd << 7) | ((imm6 & 0x1f) << 2);
          return 2;
        case 1:  // c.li
          *insn = 0x4001 | (((imm6 >> 5) & 1) << 12) | (rd << 7) | ((imm6 & 0x1f) << 2);
          return 2;
        case 2:  // c.lui (nonzero immediate)
          imm6 = imm6 == 0 ? 1 : imm6;
          *insn = 0x6001 | (((imm6 >> 5) & 1) << 12) | (rd << 7) | ((imm6 & 0x1f) << 2);
          return 2;
        case 3: {  // c.srli, c.srai, c.andi (shift amounts below 32)
          uint32_t f = eg2_pick(3);
          uint32_t imm = f == 2 ? imm6 : (imm6 & 0x1f);
          *insn = 0x8001 | (((imm >> 5) & 1) << 12) | (f << 10) | (rdc << 7) | ((imm & 0x1f) << 2);
          return 2;
        }
        case 4:  // c.sub, c.xor, c.or, c.and
          *insn = 0x8c01 | (rdc << 7) | (eg2_pick(4) << 5) | (rs2c << 2);
          return 2;
        case 5:  // c.slli (shift amount below 32, nonzero)
          *insn = 0x0002 | (rd << 7) | ((eg2_rand_range(1, 31)) << 2);
          return 2;
        case 6:  // c.mv, c.add (rs2 nonzero)
          *insn = (eg2_pick(2) ? 0x9002 : 0x8002) | (rd << 7) | (eg2_rand_range(5, 15) << 2);
          return 2;
        case 7: {  // Zcb: c.zext.b, c.sext.b, c.zext.h, c.sext.h, c.not
          static const uint16_t kZcb[5] = {0x9c61, 0x9c65, 0x9c69, 0x9c6d, 0x9c75};
          *insn = kZcb[eg2_pick(5)] | (rdc << 7);
          return 2;
        }
        default:  // Zcb: c.mul
          *insn = 0x9c41 | (rdc << 7) | (rs2c << 2);
          return 2;
      }
    }
    case kGZcmp:
      // cm.mva01s (s0/s1 into a0/a1) or cm.mvsa01 (a0/a1 into s0, s1): only s0 and s1, the
      // registers the trampoline saves.
      if (eg2_pick(2)) {
        *insn = 0xac62u | (0u << 7) | (1u << 2);  // cm.mva01s s0, s1
      } else {
        *insn = 0xac22u | (eg2_pick(2) << 7) | ((eg2_pick(2) ? 0u : 1u) << 2);  // cm.mvsa01
        if (((*insn >> 7) & 7) == ((*insn >> 2) & 7)) {
          *insn ^= 1u << 2;
        }
      }
      return 2;
    case kGCsr: {
#ifdef __CHERIOT__
      if (eg2_rand_range(0, EG2_PARAM(w_csr_scr) + EG2_PARAM(w_csr_machine) - 1) <
          EG2_PARAM(w_csr_scr)) {
        // CSpecialRW rd, scr, c0: a read of MTCC (28), MTDC (29), MScratchC (30) or MEPCC (31).
        *insn = r_type(1, 28 + eg2_pick(4), 0, 0, rd, 0x5b);
        return 4;
      }
#endif
      static const uint16_t kRead[4] = {0xb00, 0xb02, 0xc00, 0xc02};
      uint32_t k = eg2_pick(3);
      if (k == 0) {
        *insn = i_type(kRead[eg2_pick(4)], 0, 2, rd, 0x73);  // csrr
      } else {
        // csrrw / csrrs / csrrc on mcause (0x342) or mtval (0x343)
        *insn = i_type(0x342 + eg2_pick(2), rs1, eg2_rand_range(1, 3), rd, 0x73);
      }
      return 4;
    }
    default:
      *insn = 0;  // branches are laid out by the block
      return 4;
  }
}

// ------------------------------------------ Blocks --------------------------------------------

/** Lays out one block of `n` instructions at the code area's next free bytes; returns its address. */
static uint32_t build_block(uint32_t n) {
  uint32_t base = code_next;
  volatile uint16_t *p = eg2_mem(base, 4 * n + 8);
  uint32_t pos = 0;  // in halfwords
  // Branch placeholders, patched once every later instruction's place is known.
  uint16_t br_at[kMaxBlock];
  uint16_t br_to[kMaxBlock];
  uint8_t br_kind[kMaxBlock];
  uint16_t at[kMaxBlock + 1];
  uint8_t rds[kMaxBlock];
  uint32_t nbr = 0;
  for (uint32_t k = 0; k < n; ++k) {
    at[k] = (uint16_t)pos;
    uint32_t g = eg2_family_pick(groups_p);
    uint32_t rd = reg_dest();
    uint32_t rs1 = reg_any();
    uint32_t h = eg2_family_pick(hazards_p);
    if (h < 4 && k > h) {
      rs1 = rds[k - 1 - h];
    }
    rds[k] = (uint8_t)rd;
    if (g == kGBranch) {
      br_at[nbr] = (uint16_t)k;
      br_to[nbr] = (uint16_t)eg2_rand_range(k + 1, n);  // n: the final return
      br_kind[nbr] = (uint8_t)eg2_pick(3);  // 0 conditional, 1 jal x0, 2 compressed
      uint32_t len = br_kind[nbr] == 2 ? 2 : 4;
      // Placeholder: a nop of the right length.
      if (len == 2) {
        p[pos++] = 0x0001;
      } else {
        p[pos++] = 0x0013;
        p[pos++] = 0x0000;
      }
      ++nbr;
      continue;
    }
    uint32_t insn;
    uint32_t len = draw_insn(g, rd, rs1, &insn);
    p[pos++] = (uint16_t)insn;
    if (len == 4) {
      p[pos++] = (uint16_t)(insn >> 16);
    }
  }
  at[n] = (uint16_t)pos;
  p[pos++] = (uint16_t)kRet;
  p[pos++] = (uint16_t)(kRet >> 16);
  for (uint32_t b = 0; b < nbr; ++b) {
    uint32_t from = at[br_at[b]], to = at[br_to[b]];
    uint32_t off = 2 * (to - from);
    uint32_t insn;
    if (br_kind[b] == 2) {
      // c.beqz / c.bnez rs1', off (9 bits signed, forward only here)
      if (off > 254) {
        off = 0;  // too far: a branch to itself would loop; use c.nop instead
        p[from] = 0x0001;
        continue;
      }
      insn = (eg2_pick(2) ? 0xe001 : 0xc001) | (reg_c() << 7) | (((off >> 8) & 1) << 12) |
             (((off >> 3) & 3) << 10) | (((off >> 6) & 3) << 5) | (((off >> 1) & 3) << 3) |
             (((off >> 5) & 1) << 2);
      p[from] = (uint16_t)insn;
      continue;
    }
    if (br_kind[b] == 1) {
      insn = j_type(off, 0);
    } else {
      static const uint8_t kBf3[6] = {0, 1, 4, 5, 6, 7};
      insn = b_type(off, reg_any(), reg_any(), kBf3[eg2_pick(6)]);
    }
    p[from] = (uint16_t)insn;
    p[from + 1] = (uint16_t)(insn >> 16);
  }
  code_next = (base + 2 * pos + 3) & ~3u;
  return base;
}

static void preset_operands(void) {
  uint32_t pct = EG2_PARAM(cap_reg_pct) == 101 ? eg2_rand_range(0, 50) : EG2_PARAM(cap_reg_pct);
  uint32_t prev = eg2_rand();
  for (uint32_t i = 0; i < 11; ++i) {
    uint32_t v = operand_value(prev);
    prev = v;
#ifdef __CHERIOT__
    if (eg2_rand_range(0, 99) < pct) {
      eg2_ms_csc(kOperands + 8 * i, eg2_mem(kEg2MsHeapBase + 8 * eg2_pick(0x6000), 64));
      continue;
    }
#else
    (void)pct;
#endif
    eg2_ms_write32(kOperands + 8 * i, v);
    eg2_ms_write32(kOperands + 8 * i + 4, 0);
  }
}

static void *entry_of(uint32_t addr) {
#ifdef __CHERIOT__
  void *c;
  __asm__ volatile("auipcc %0, 0" : "=C"(c));
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(addr));
  return c;
#else
  return (void *)(uintptr_t)addr;
#endif
}

bool eg2_test_main(void) {
  bool gaps = EG2_PARAM(model_gaps) != 0;
  eg2_family_t groups, operands, hazards;
  groups_p = &groups;
  operands_p = &operands;
  hazards_p = &hazards;
  // sec SRAM: zeroed, then executable (sram_ctrl EXEC; OTP's EN_SRAM_IFETCH or the life cycle's
  // HW_DEBUG_EN decide whether the register is obeyed).
  if (!eg2_sec_sram_init()) {
    return false;
  }
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, 0x6);
  uint32_t ctl;
  __asm__ volatile("csrr %0, 0x7c0" : "=r"(ctl));
  if (EG2_PARAM(icache) != 2) {
    ctl = (ctl & ~1u) | EG2_PARAM(icache);
  }
  uint32_t di = EG2_PARAM(dummy_instr) != 2 ? EG2_PARAM(dummy_instr) : eg2_pick(2);
  ctl = (ctl & ~4u) | (di << 2);
  __asm__ volatile("csrw 0x7c0, %0" : : "r"(ctl));

  groups.n = kGCount;
  groups.plain_mask = 0;
  groups.boosted = kGCount;
  groups.w[kGInt] = EG2_PARAM(w_group_int);
  groups.w[kGMulDiv] = EG2_PARAM(w_group_muldiv);
  groups.w[kGZb] = EG2_PARAM(w_group_zb);
  groups.w[kGZbk] = EG2_PARAM(w_group_zbk);
  groups.w[kGC] = EG2_PARAM(w_group_c);
#ifdef __CHERIOT__
  // Zcmp is RISC-V mode only. The knob is not read at all here: the CHERIoT toolchain compiles a
  // volatile load whose value is overwritten to `ct.auipcc zero; ct.clw zero, off(zero)`, which
  // traps (rtl_tickets F26).
  groups.w[kGZcmp] = 0;
#else
  groups.w[kGZcmp] = EG2_PARAM(w_group_zcmp);
#endif
  groups.w[kGCsr] = EG2_PARAM(w_group_csr);
  groups.w[kGBranch] = EG2_PARAM(w_group_branch);
  // Operand classes by the weight rule (uniform the plain class); hazards 25:20:15:10:30.
  eg2_family_draw(&operands, kOpClasses, 1u << 8, EG2_PARAM(boost));
  hazards.n = 5;
  hazards.plain_mask = 0;
  hazards.boosted = 5;
  hazards.w[0] = 25;
  hazards.w[1] = 20;
  hazards.w[2] = 15;
  hazards.w[3] = 10;
  hazards.w[4] = 30;
  eg2_family_print("group", &groups);
  eg2_family_print("operand", &operands);

  uint32_t n = eg2_draw_count(EG2_PARAM(n_instr), 1000, 10000, 20000);
  if (gaps) {
    // No address written twice for the model: what fits once.
    uint32_t fit = kCodeSize / 4 - 2 * (n / kMaxBlock + 1);
    n = n > fit ? fit : n;
  }
  if (EG2_PARAM(u_mode_pct) != 0) {
    eg2_puts("EG2 rand_isa: u_mode_pct not implemented, M mode only (gap)\r\n");
  }
  eg2_put_kv("EG2 rand_isa n_instr", n);
  eg2_put_kv(" cpuctrlsts", ctl);
  eg2_puts("\r\n");

  code_next = kCodeBase;
  uint32_t made = 0;
  while (made < n) {
    uint32_t len = n - made > kMaxBlock ? eg2_rand_range(64, kMaxBlock) : n - made;
    if (code_next + 4 * len + 8 > kCodeBase + kCodeSize) {
      code_next = kCodeBase;  // wrap (never with model_gaps: n fits)
    }
    ++tally.planned;
    uint32_t at = build_block(len);
    __asm__ volatile("fence.i" ::: "memory");
    preset_operands();
    (void)eg2_isa_run(entry_of(at), eg2_mem(kOperands, 11 * 8));
    made += len;
    ++tally.done;
  }
#ifdef EG2_RAND_ISA_INJECT_FAIL
  ++tally.planned;
#endif
  eg2_put_kv("EG2 rand_isa instructions", made);
  eg2_puts("\r\n");
  eg2_tally_print("rand_isa", &tally);
  return eg2_tally_ok(&tally) && made == n;
}
