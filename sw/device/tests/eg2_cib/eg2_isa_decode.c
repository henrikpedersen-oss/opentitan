// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cib/eg2_isa_decode.h"

// Every rule here cites its document in eg2_isa_decode.h; no table is taken from the RTL. The
// code uses switch statements only on small dense ranges; the CHERIoT build adds
// -fno-jump-tables (BUILD), so no table of pointers is made.

uint32_t eg2_isa_encoding(uint32_t idx, uint32_t ops) {
  if (idx < kEg2Isa16Count) {
    return idx;
  }
  uint32_t j = idx - kEg2Isa16Count;
  uint32_t opc5 = j & 0x1f;
  uint32_t f3 = (j >> 5) & 7;
  uint32_t f7 = (j >> 8) & 0x7f;
  uint32_t rd = ops & 0x1f;
  uint32_t rs1 = (ops >> 5) & 0x1f;
  uint32_t rs2 = (ops >> 10) & 0x1f;
  return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | opc5 << 2 | 3;
}

static int32_t sext(uint32_t v, uint32_t bits) {
  uint32_t m = 1u << (bits - 1);
  return (int32_t)((v ^ m) - m);
}

// ------------------------------------------------------------------------------------------------
// CSRs (Ibex doc cs_registers.rst; CHERIoT changes in cheriot.rst and cs_registers.rst
// "CHERIoT Special Capability Registers"; mconfigptr, menvcfg(h): CHERIoT-Ibex spec REQ_ISA_11).

bool eg2_isa_csr_exists(uint32_t csr, bool cheriot) {
  // Present in both modes.
  if ((csr >= 0x300 && csr <= 0x301) || csr == 0x304 || csr == 0x306 || csr == 0x30a ||
      csr == 0x31a || csr == 0x320 || (csr >= 0x323 && csr <= 0x33f) || csr == 0x340 ||
      (csr >= 0x342 && csr <= 0x344) || csr == 0x5a8 || (csr >= 0x7a0 && csr <= 0x7a3) ||
      csr == 0x7a8 || csr == 0x7aa || csr == 0x7c0 || csr == 0x7c1 || csr == 0xb00 ||
      (csr >= 0xb02 && csr <= 0xb1f) || csr == 0xb80 || (csr >= 0xb82 && csr <= 0xb9f) ||
      csr == 0xc00 || (csr >= 0xc02 && csr <= 0xc1f) || csr == 0xc80 ||
      (csr >= 0xc82 && csr <= 0xc9f) || (csr >= 0xf11 && csr <= 0xf15)) {
    return true;
  }
  if (cheriot) {
    // mshwm, mshwmb: CHERIoT mode only.
    return csr == 0xbc1 || csr == 0xbc2;
  }
  // RISC-V mode only: mtvec, mepc, pmpcfg0-3, pmpaddr0-15, mseccfg, mseccfgh.
  return csr == 0x305 || csr == 0x341 || (csr >= 0x3a0 && csr <= 0x3a3) ||
         (csr >= 0x3b0 && csr <= 0x3bf) || csr == 0x747 || csr == 0x757;
}

// ------------------------------------------------------------------------------------------------
// 16-bit encodings (RISC-V unprivileged ISA "C" with its reserved and HINT code points, Zcb,
// Zcmp; CHERIoT ISA compressed capability instructions).

static void decode16(uint32_t p, bool ch, eg2_isa_info_t *o) {
  uint32_t q = p & 3;
  uint32_t f3 = (p >> 13) & 7;
  uint32_t b12 = (p >> 12) & 1;
  uint32_t r5 = (p >> 7) & 0x1f;   // rd / rs1 (full)
  uint32_t r2 = (p >> 2) & 0x1f;   // rs2 (full)
  uint32_t rs1p = 8 + ((p >> 7) & 7);
  bool legal = false;
  bool x16 = false;
  o->cls = (uint8_t)(kEg2IsaClsC0 + q);
  o->kind = kEg2IsaKindPlain;

  if (q == 0) {
    switch (f3) {
      case 0:  // c.addi4spn / c.cincaddr4cspn: nzuimm != 0
        legal = ((p >> 5) & 0xff) != 0;
        break;
      case 1:  // c.fld (no D); RV32 CHERIoT has no capability form here
        break;
      case 2:  // c.lw
        legal = true;
        o->kind = kEg2IsaKindLoad;
        o->base = (uint8_t)rs1p;
        o->off = (int32_t)(((p >> 10) & 7) << 3 | ((p >> 6) & 1) << 2 | ((p >> 5) & 1) << 6);
        break;
      case 3:  // c.flw (no F) ; c.clc in CHERIoT mode
        legal = ch;
        o->kind = kEg2IsaKindCapMem;
        break;
      case 4: {  // Zcb loads and stores
        uint32_t f = (p >> 10) & 7;
        uint32_t b6 = (p >> 6) & 1;
        o->base = (uint8_t)rs1p;
        if (f == 0) {  // c.lbu
          legal = true;
          o->kind = kEg2IsaKindLoad;
          o->off = (int32_t)(b6 | ((p >> 5) & 1) << 1);
        } else if (f == 1) {  // c.lhu (bit 6 = 0), c.lh (bit 6 = 1)
          legal = true;
          o->kind = kEg2IsaKindLoad;
          o->off = (int32_t)(((p >> 5) & 1) << 1);
        } else if (f == 2) {  // c.sb
          legal = true;
          o->kind = kEg2IsaKindStore;
          o->size = 1;
          o->off = (int32_t)(b6 | ((p >> 5) & 1) << 1);
        } else if (f == 3 && b6 == 0) {  // c.sh; bit 6 set is reserved
          legal = true;
          o->kind = kEg2IsaKindStore;
          o->size = 2;
          o->off = (int32_t)(((p >> 5) & 1) << 1);
        }
        break;
      }
      case 5:  // c.fsd (no D)
        break;
      case 6:  // c.sw
        legal = true;
        o->kind = kEg2IsaKindStore;
        o->size = 4;
        o->base = (uint8_t)rs1p;
        o->off = (int32_t)(((p >> 10) & 7) << 3 | ((p >> 6) & 1) << 2 | ((p >> 5) & 1) << 6);
        break;
      default:  // 7: c.fsw (no F) ; c.csc in CHERIoT mode
        legal = ch;
        o->kind = kEg2IsaKindCapMem;
        break;
    }
  } else if (q == 1) {
    switch (f3) {
      case 0:  // c.nop, c.addi and their HINTs
        legal = true;
        x16 = r5 >= 16;
        break;
      case 1:  // c.jal (RV32) ; c.cjal
      case 5:  // c.j
        legal = true;
        o->kind = kEg2IsaKindXferPc;
        o->off = sext(((p >> 12) & 1) << 11 | ((p >> 11) & 1) << 4 | ((p >> 9) & 3) << 8 |
                          ((p >> 8) & 1) << 10 | ((p >> 7) & 1) << 6 | ((p >> 6) & 1) << 7 |
                          ((p >> 3) & 7) << 1 | ((p >> 2) & 1) << 5,
                      12);
        break;
      case 2:  // c.li and its HINT (rd = x0)
        legal = true;
        x16 = r5 >= 16;
        break;
      case 3: {  // c.addi16sp / c.cincaddr16csp (rd = x2), c.lui and its HINT (rd = x0)
        uint32_t nz = b12 | ((p >> 2) & 0x1f);
        legal = nz != 0;  // nzimm = 0 is reserved in every form
        x16 = r5 >= 16;
        break;
      }
      case 4: {
        uint32_t f2 = (p >> 10) & 3;
        if (f2 <= 1) {  // c.srli, c.srai: shamt[5] = 1 is reserved (NSE) in RV32
          legal = b12 == 0;
        } else if (f2 == 2) {  // c.andi
          legal = true;
        } else if (b12 == 0) {  // c.sub, c.xor, c.or, c.and
          legal = true;
        } else {
          uint32_t f = (p >> 5) & 3;
          if (f == 2) {  // c.mul (Zcb with M)
            legal = true;
          } else if (f == 3) {  // Zcb unary: zext.b, sext.b, zext.h, sext.h, not
            uint32_t u = (p >> 2) & 7;
            legal = u <= 3 || u == 5;  // 4 is c.zext.w (RV64), 6 and 7 reserved
          }  // f 0, 1: c.subw, c.addw (RV64)
        }
        break;
      }
      default:  // 6 c.beqz, 7 c.bnez
        legal = true;
        o->kind = kEg2IsaKindXferPc;
        o->base = (uint8_t)rs1p;
        o->off = sext(((p >> 12) & 1) << 8 | ((p >> 10) & 3) << 3 | ((p >> 5) & 3) << 6 |
                          ((p >> 3) & 3) << 1 | ((p >> 2) & 1) << 5,
                      9);
        break;
    }
  } else {  // q == 2
    switch (f3) {
      case 0:  // c.slli and its HINTs: shamt[5] = 1 reserved in RV32
        legal = b12 == 0;
        x16 = r5 >= 16;
        break;
      case 1:  // c.fldsp (no D)
        break;
      case 2:  // c.lwsp: rd = x0 reserved
        legal = r5 != 0;
        x16 = r5 >= 16;
        o->kind = kEg2IsaKindLoad;
        o->base = 2;
        o->off = (int32_t)(b12 << 5 | ((p >> 4) & 7) << 2 | ((p >> 2) & 3) << 6);
        break;
      case 3:  // c.flwsp (no F) ; c.clcsp in CHERIoT mode, cd = c0 illegal
        legal = ch && r5 != 0;
        x16 = r5 >= 16;
        o->kind = kEg2IsaKindCapMem;
        break;
      case 4:
        if (b12 == 0 && r2 == 0) {  // c.jr / c.cjr: rs1 = x0 reserved
          legal = r5 != 0;
          x16 = r5 >= 16;
          o->kind = kEg2IsaKindXferReg;
          o->base = (uint8_t)r5;
        } else if (b12 == 0) {  // c.mv and its HINT (rd = x0)
          legal = true;
          x16 = r5 >= 16 || r2 >= 16;
        } else if (r2 == 0 && r5 == 0) {  // c.ebreak
          legal = true;
        } else if (r2 == 0) {  // c.jalr / c.cjalr
          legal = true;
          x16 = r5 >= 16;
          o->kind = kEg2IsaKindXferReg;
          o->base = (uint8_t)r5;
        } else {  // c.add and its HINT (rd = x0)
          legal = true;
          x16 = r5 >= 16 || r2 >= 16;
        }
        break;
      case 5:  // c.fsdsp (no D) ; Zcmp in RISC-V mode only (cheriot.rst: illegal in CHERIoT mode)
        if (!ch) {
          uint32_t hi = (p >> 8) & 0x1f;
          uint32_t rlist = (p >> 4) & 0xf;
          if (hi == 0x18 || hi == 0x1a || hi == 0x1c || hi == 0x1e) {
            legal = rlist >= 4;  // rlist 0-3 reserved
            o->kind = hi == 0x18 ? kEg2IsaKindPush
                                 : (hi == 0x1a ? kEg2IsaKindPop : kEg2IsaKindPopRet);
          } else if (((p >> 10) & 7) == 3) {
            uint32_t f = (p >> 5) & 3;
            if (f == 1) {  // cm.mvsa01: r1s' = r2s' reserved
              legal = ((p >> 7) & 7) != ((p >> 2) & 7);
            } else if (f == 3) {  // cm.mva01s
              legal = true;
            }
          }  // cm.jt / cm.jalt (Zcmt, not configured) and the rest: reserved
        }
        break;
      case 6:  // c.swsp
        legal = true;
        x16 = r2 >= 16;
        o->kind = kEg2IsaKindStore;
        o->size = 4;
        o->base = 2;
        o->off = (int32_t)(((p >> 9) & 0xf) << 2 | ((p >> 7) & 3) << 6);
        break;
      default:  // 7: c.fswsp (no F) ; c.cscsp in CHERIoT mode
        legal = ch;
        x16 = r2 >= 16;
        o->kind = kEg2IsaKindCapMem;
        break;
    }
  }
  if (!ch) {
    x16 = false;
  }
  o->x16 = legal && x16;
  o->legal = legal && !x16;
  if (ch && o->legal &&
      (o->kind == kEg2IsaKindLoad || o->kind == kEg2IsaKindStore ||
       o->kind == kEg2IsaKindXferReg)) {
    // Every register holds an untagged integer in the harness: the access or jump faults.
    o->kind = kEg2IsaKindCapMem;
  }
}

// ------------------------------------------------------------------------------------------------
// 32-bit encodings.

// OP-IMM (0x13) with funct3 1 and 5: the shift and unary encodings (RV32I, Zbb, Zbs, Zbkb).
static bool op_imm_legal(uint32_t f3, uint32_t f7, uint32_t rs2) {
  if (f3 == 1) {
    switch (f7) {
      case 0x00:  // slli (RV32: shamt[5] = insn[25] = 0)
      case 0x14:  // bseti
      case 0x24:  // bclri
      case 0x34:  // binvi
        return true;
      case 0x30:  // clz, ctz, cpop, sext.b, sext.h
        return rs2 <= 2 || rs2 == 4 || rs2 == 5;
      case 0x04:  // zip (Zbkb, RV32)
        return rs2 == 0x0f;
      default:
        return false;
    }
  }
  // f3 == 5
  switch (f7) {
    case 0x00:  // srli
    case 0x20:  // srai
    case 0x30:  // rori
    case 0x24:  // bexti
      return true;
    case 0x14:  // orc.b
      return rs2 == 0x07;
    case 0x34:  // rev8 (RV32), brev8 (Zbkb)
      return rs2 == 0x18 || rs2 == 0x07;
    case 0x04:  // unzip (Zbkb, RV32)
      return rs2 == 0x0f;
    default:
      return false;
  }
}

// OP (0x33): RV32I, M, Zba, Zbb, Zbc, Zbs, Zbkb, Zbkx.
static bool op_legal(uint32_t f3, uint32_t f7) {
  switch (f7) {
    case 0x00:  // add sll slt sltu xor srl or and
    case 0x01:  // M
      return true;
    case 0x20:  // sub, sra; xnor, orn, andn
      return f3 == 0 || f3 == 5 || f3 == 4 || f3 == 6 || f3 == 7;
    case 0x05:  // clmul, clmulr, clmulh (Zbc); min, minu, max, maxu (Zbb)
      return f3 != 0;
    case 0x10:  // sh1add, sh2add, sh3add
      return f3 == 2 || f3 == 4 || f3 == 6;
    case 0x14:  // bset; xperm4, xperm8 (Zbkx)
      return f3 == 1 || f3 == 2 || f3 == 4;
    case 0x24:  // bclr, bext
      return f3 == 1 || f3 == 5;
    case 0x34:  // binv
      return f3 == 1;
    case 0x30:  // rol, ror
      return f3 == 1 || f3 == 5;
    case 0x04:  // pack (zext.h when rs2 = x0), packh (Zbkb)
      return f3 == 4 || f3 == 7;
    default:
      return false;
  }
}

// CHERIoT opcode 0x5B, funct3 0 (CHERIoT ISA; CHERIoT-Sail cheri_insts.sail encdec).
static bool cheri_f3_0_legal(uint32_t f7, uint32_t rs2, bool *uses_rs2) {
  *uses_rs2 = false;
  switch (f7) {
    case 0x7f:  // the one-source group, by rs2
      // CGetPerm 00, CGetType 01, CGetBase 02, CGetLen 03, CGetTag 04, CRRL 08, CRAM 09,
      // CMove 0A, CClearTag 0B, CGetAddr 0F, CGetHigh 17, CGetTop 18. CGetOffset (06) is not in
      // the CHERIoT ISA.
      return rs2 <= 4 || (rs2 >= 8 && rs2 <= 0xb) || rs2 == 0xf || rs2 == 0x17 || rs2 == 0x18;
    case 0x01:  // CSpecialRW: SCRs 28-31 outside debug mode
      return rs2 >= 28;
    case 0x08:  // CSetBounds
    case 0x09:  // CSetBoundsExact
    case 0x0a:  // CSetBoundsRoundDown
    case 0x0b:  // CSeal
    case 0x0c:  // CUnseal
    case 0x0d:  // CAndPerm
    case 0x10:  // CSetAddr
    case 0x11:  // CIncAddr
    case 0x14:  // CSub
    case 0x16:  // CSetHigh
    case 0x20:  // CTestSubset
    case 0x21:  // CSetEqualExact
      *uses_rs2 = true;
      return true;
    default:
      return false;
  }
}

static void decode32(uint32_t w, bool ch, eg2_isa_info_t *o) {
  uint32_t op = w & 0x7f;
  uint32_t rd = (w >> 7) & 0x1f;
  uint32_t f3 = (w >> 12) & 7;
  uint32_t rs1 = (w >> 15) & 0x1f;
  uint32_t rs2 = (w >> 20) & 0x1f;
  uint32_t f7 = w >> 25;
  bool legal = false;
  // The register fields this encoding uses as registers (CHERIoT RV32E check).
  bool u_rd = false, u_rs1 = false, u_rs2 = false;
  o->kind = kEg2IsaKindPlain;
  o->cls = kEg2IsaClsAbsent;

  switch (op) {
    case 0x03:  // LOAD; CLC (funct3 3) in CHERIoT mode
      o->cls = kEg2IsaClsLoad;
      legal = f3 <= 2 || f3 == 4 || f3 == 5 || (ch && f3 == 3);
      u_rd = u_rs1 = true;
      o->kind = kEg2IsaKindLoad;
      o->base = (uint8_t)rs1;
      o->off = sext(w >> 20, 12);
      break;
    case 0x0f:  // MISC-MEM: FENCE, FENCE.I (reserved fields ignored)
      o->cls = kEg2IsaClsMiscMem;
      legal = f3 <= 1;
      break;
    case 0x13:  // OP-IMM
      o->cls = kEg2IsaClsOpImm;
      legal = (f3 == 1 || f3 == 5) ? op_imm_legal(f3, f7, rs2) : true;
      u_rd = u_rs1 = true;
      break;
    case 0x17:  // AUIPC ; AUIPCC
    case 0x37:  // LUI
      o->cls = kEg2IsaClsUpper;
      legal = true;
      u_rd = true;
      break;
    case 0x23:  // STORE; CSC (funct3 3) in CHERIoT mode
      o->cls = kEg2IsaClsStore;
      legal = f3 <= 2 || (ch && f3 == 3);
      u_rs1 = u_rs2 = true;
      o->kind = kEg2IsaKindStore;
      o->base = (uint8_t)rs1;
      o->size = (uint8_t)(1u << (f3 & 3));
      o->off = sext(f7 << 5 | rd, 12);
      break;
    case 0x33:  // OP
      o->cls = kEg2IsaClsOp;
      legal = op_legal(f3, f7);
      u_rd = u_rs1 = u_rs2 = true;
      break;
    case 0x5b:  // CHERIoT capability instructions
      o->cls = kEg2IsaClsCheri;
      if (ch) {
        if (f3 == 1 || f3 == 2) {  // CIncAddrImm, CSetBoundsImm
          legal = true;
          u_rd = u_rs1 = true;
        } else if (f3 == 0) {
          bool uses_rs2;
          legal = cheri_f3_0_legal(f7, rs2, &uses_rs2);
          u_rd = u_rs1 = true;
          u_rs2 = uses_rs2;
        }
      }
      break;
    case 0x63:  // BRANCH
      o->cls = kEg2IsaClsBranch;
      legal = f3 != 2 && f3 != 3;
      u_rs1 = u_rs2 = true;
      o->kind = kEg2IsaKindXferPc;
      o->off = sext((w >> 31) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3f) << 5 |
                        ((w >> 8) & 0xf) << 1,
                    13);
      break;
    case 0x67:  // JALR ; CJALR
      o->cls = kEg2IsaClsJalr;
      legal = f3 == 0;
      u_rd = u_rs1 = true;
      o->kind = kEg2IsaKindXferReg;
      o->base = (uint8_t)rs1;
      o->off = sext(w >> 20, 12);
      break;
    case 0x6f:  // JAL ; CJAL
      o->cls = kEg2IsaClsJal;
      legal = true;
      u_rd = true;
      o->kind = kEg2IsaKindXferPc;
      o->off = sext((w >> 31) << 20 | ((w >> 12) & 0xff) << 12 | ((w >> 20) & 1) << 11 |
                        ((w >> 21) & 0x3ff) << 1,
                    21);
      break;
    case 0x73:  // SYSTEM
      if (f3 == 0 || f3 == 4) {
        o->cls = kEg2IsaClsSystem;
        // ECALL, EBREAK, MRET, WFI exactly; DRET only in debug mode; no S/U-mode returns,
        // no fences of the hypervisor or supervisor.
        legal = w == 0x00000073 || w == 0x00100073 || w == 0x30200073 || w == 0x10500073;
        if (w == 0x30200073) {
          o->kind = kEg2IsaKindMret;
        } else if (w == 0x10500073) {
          o->kind = kEg2IsaKindWfi;
        }
      } else {
        o->cls = kEg2IsaClsCsr;
        uint32_t csr = w >> 20;
        bool write = f3 == 1 || f3 == 5 || rs1 != 0;  // CSRRS/C(I) with x0 / 0 do not write
        legal = eg2_isa_csr_exists(csr, ch) && !(write && (csr >> 10) == 3);
        u_rd = true;
        u_rs1 = f3 <= 3;  // the immediate forms carry uimm there
        o->kind = kEg2IsaKindCsr;
        o->csr = (uint16_t)csr;
        o->csr_write = write;
      }
      break;
    case 0x7b:  // AUICGP (CHERIoT)
      o->cls = kEg2IsaClsAuicgp;
      legal = ch;
      u_rd = true;
      break;
    default:  // no instruction in the configuration (F, D, A, V, RV64, custom, longer than 32 bits)
      break;
  }
  bool x16 = ch && ((u_rd && rd >= 16) || (u_rs1 && rs1 >= 16) || (u_rs2 && rs2 >= 16));
  o->x16 = legal && x16;
  o->legal = legal && !x16;
  if (ch && o->legal &&
      (o->kind == kEg2IsaKindLoad || o->kind == kEg2IsaKindStore ||
       o->kind == kEg2IsaKindXferReg)) {
    o->kind = kEg2IsaKindCapMem;
  }
}

void eg2_isa_decode(uint32_t insn, bool cheriot, eg2_isa_info_t *out) {
  *out = (eg2_isa_info_t){0};
  if (eg2_isa_is16(insn)) {
    decode16(insn & 0xffff, cheriot, out);
  } else {
    decode32(insn, cheriot, out);
  }
}

// ------------------------------------------------------------------------------------------------
// The code slot (see the header).

void eg2_isa_slot(uint32_t insn, bool cheriot, eg2_isa_slot_t *s) {
  for (uint32_t i = 0; i < kEg2IsaSlotWords; ++i) {
    s->w[i] = 0;
  }
  s->probe_reg = 0;
  s->probe1_reg = 0;
  s->probe1_copy = false;
  uint32_t probe0 = kEg2IsaNop;
  uint32_t probe1 = kEg2IsaNop;
  uint32_t restore = 0;
  bool is16 = eg2_isa_is16(insn);
  if (!is16) {
    uint32_t op = insn & 0x7f;
    uint32_t rd = (insn >> 7) & 0x1f;
    uint32_t f3 = (insn >> 12) & 7;
    uint32_t rs1 = (insn >> 15) & 0x1f;
    uint32_t f7 = insn >> 25;
    uint32_t r = 5;
    while (r == rd || r == rs1) {
      ++r;
    }
    bool rs1_ok = rs1 != 0 && !(cheriot && rs1 >= 16);
    if (op == 0x73 && f3 != 0 && f3 != 4) {
      uint32_t csr = insn >> 20;
      s->probe_reg = r;
      probe0 = csr << 20 | 2u << 12 | r << 7 | 0x73;  // csrrs R, csr, x0
      if (f3 == 1 && rs1_ok) {
        probe1 = r << 15 | rs1 << 7 | 0x13;  // addi rs1, R, 0
        s->probe1_reg = rs1;
        s->probe1_copy = true;
      } else if ((f3 == 2 || f3 == 3) && rs1_ok) {
        probe1 = rs1 << 7 | 0x13;  // addi rs1, x0, 0
        s->probe1_reg = rs1;
      }
      if ((f3 == 1 && rs1 == 0) || f3 == 5 || ((f3 == 6 || f3 == 7) && rs1 != 0)) {
        restore = csr << 20 | r << 15 | 1u << 12 | 0x73;  // csrrw x0, csr, R
      }
    } else if (cheriot && op == 0x5b && f3 == 0 && f7 == 0x01) {
      uint32_t scr = (insn >> 20) & 0x1f;
      s->probe_reg = r;
      probe0 = 0x01u << 25 | scr << 20 | r << 7 | 0x5b;  // cspecialrw cR, scr, c0
      if (rs1_ok) {
        probe1 = 0x7fu << 25 | 0x0au << 20 | r << 15 | rs1 << 7 | 0x5b;  // cmove cs1, cR
        s->probe1_reg = rs1;
        s->probe1_copy = true;
      }
    }
  }
  s->w[0] = probe0;
  s->w[1] = probe1;
  // The rest as halfwords from byte 8.
  uint16_t h[12] = {0};
  uint32_t n = 0;
  h[n++] = (uint16_t)insn;
  if (!is16) {
    h[n++] = (uint16_t)(insn >> 16);
  }
  if (restore != 0) {
    h[n++] = (uint16_t)restore;
    h[n++] = (uint16_t)(restore >> 16);
  }
  s->end_off = kEg2IsaTestOff + 2 * n;
  h[n++] = (uint16_t)kEg2IsaEbreak;
  h[n++] = (uint16_t)(kEg2IsaEbreak >> 16);
  for (uint32_t i = 0; i < 12; ++i) {
    s->w[2 + i / 2] |= (uint32_t)h[i] << (16 * (i & 1));
  }
}
