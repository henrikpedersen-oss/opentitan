// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CIB_EG2_ISA_DECODE_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CIB_EG2_ISA_DECODE_H_

/**
 * @file
 * @brief The decode table of chip_sw_eg2_illegal_sweep (EG2 spec REQ_CIB_07), built from the
 * documents, never from the RTL:
 *
 *  - the configuration: top_earlgrey.hjson rv_core_ibex (RV32IMC, RV32B = RV32BFull, RV32ZC =
 *    RV32ZcaZcbZcmp, 16 PMP regions, DbgTriggerEn); Ibex doc instruction_decode_execute.rst
 *    (RV32BFull = Zba, Zbb, Zbc, Zbs, Zbkb, Zbkx) and integration.rst (RV32ZC);
 *  - RISC-V mode: the RISC-V unprivileged ISA (RV32I, M, C with its reserved and HINT code points,
 *    Zifencei, Zicsr, Zba/Zbb/Zbc/Zbs, Zbkb/Zbkx, Zcb, Zcmp) and the privileged ISA (M-mode
 *    SYSTEM: ECALL, EBREAK, MRET, WFI; DRET only in debug mode); the CSR map of Ibex doc
 *    cs_registers.rst (dcsr, dpc, dscratch0/1 only in debug mode; a write to an address with
 *    bits [11:10] = 11 is illegal), with mconfigptr, menvcfg and menvcfgh, which the CHERIoT-Ibex
 *    verification spec REQ_ISA_11 names as Ibex CSRs;
 *  - CHERIoT mode: the CHERIoT ISA v1.0 (archdoc "Instruction encoding summary", CHERIoT-Sail
 *    cheri_insts.sail / cheri_insts_cext.sail encodings) and the CHERIoT-Ibex verification spec
 *    REQ_ISA_11: RV32E (every register field naming x16-x31 is illegal, HINTs included), CLC/CSC
 *    in the LD/SD slots, c.clc/c.csc/c.clcsp/c.cscsp in the c.flw/c.fsw/c.flwsp/c.fswsp slots,
 *    no Zcmp, no D/F slots, the 0x5B instructions and AUICGP, CSpecialRW only on SCRs 28-31
 *    outside debug mode; mtvec, mepc, the PMP CSRs and mseccfg(h) do not exist, mshwm and
 *    mshwmb do (Ibex doc cheriot.rst, cs_registers.rst).
 *
 * FENCE and FENCE.I take any value in their reserved fields in both modes (RISC-V unprivileged
 * ISA: implementations ignore them; CHERIoT-Sail FENCE_RESERVED / FENCEI_RESERVED): those fields
 * are not register specifiers, so x16-x31 values there are legal in CHERIoT mode too.
 *
 * The same file gives the sweep's index-to-encoding map and the code slot each encoding runs in;
 * the bench's chip_sw_eg2_illegal_sweep_vseq.sv repeats both for the ROM placement, and the
 * software checks every word the bench wrote against this file before it runs a batch.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The sweep's index space: all 16-bit parcels, then opcode x funct3 x funct7 of 32-bit words. */
enum {
  kEg2Isa16Count = 65536,
  kEg2Isa32Count = 32 * 8 * 128,
  kEg2IsaCount = kEg2Isa16Count + kEg2Isa32Count,
};

/**
 * The encoding of sweep index `idx`. Index < 65536: the 16-bit parcel `idx`; a parcel whose bits
 * [1:0] are 11 is the low half of a 32-bit instruction, whose high half is 0. Index >= 65536:
 * j = idx - 65536, bits [6:2] = j[4:0], funct3 = j[7:5], funct7 = j[14:8], bits [1:0] = 11, and
 * the fixed operand fields rd, rs1, rs2 (`ops` = rd | rs1 << 5 | rs2 << 10).
 */
uint32_t eg2_isa_encoding(uint32_t idx, uint32_t ops);

/** True for a 16-bit instruction (bits [1:0] not 11). */
static inline bool eg2_isa_is16(uint32_t insn) { return (insn & 3) != 3; }

/** Coverage and report classes. */
typedef enum eg2_isa_class {
  kEg2IsaClsC0 = 0,       // 16-bit quadrant 0
  kEg2IsaClsC1 = 1,       // 16-bit quadrant 1
  kEg2IsaClsC2 = 2,       // 16-bit quadrant 2
  kEg2IsaClsLoad = 3,
  kEg2IsaClsStore = 4,
  kEg2IsaClsBranch = 5,
  kEg2IsaClsJalr = 6,
  kEg2IsaClsJal = 7,
  kEg2IsaClsMiscMem = 8,
  kEg2IsaClsSystem = 9,   // SYSTEM funct3 0 and 4
  kEg2IsaClsCsr = 10,     // SYSTEM CSR forms
  kEg2IsaClsOpImm = 11,
  kEg2IsaClsOp = 12,
  kEg2IsaClsUpper = 13,   // LUI, AUIPC / AUIPCC
  kEg2IsaClsCheri = 14,   // opcode 0x5B
  kEg2IsaClsAuicgp = 15,  // opcode 0x7B
  kEg2IsaClsAbsent = 16,  // an opcode with no instruction in the configuration
  kEg2IsaClassCount = 17,
} eg2_isa_class_t;

/** What executing a legal encoding does, for the harness's safety rules. */
typedef enum eg2_isa_kind {
  kEg2IsaKindPlain = 0,    // registers only, or a trap at the instruction (ECALL, EBREAK)
  kEg2IsaKindLoad = 1,     // a load at base + off
  kEg2IsaKindStore = 2,    // a store of `size` bytes at base + off
  kEg2IsaKindPush = 3,     // cm.push: stores below sp
  kEg2IsaKindPop = 4,      // cm.pop: loads above sp
  kEg2IsaKindPopRet = 5,   // cm.popret(z): loads, then a jump to the loaded ra
  kEg2IsaKindXferPc = 6,   // a PC-relative jump or branch to pc + off (taken)
  kEg2IsaKindXferReg = 7,  // a register-indirect jump to base + off
  kEg2IsaKindMret = 8,     // MRET: returns through mepc, which the harness enters by
  kEg2IsaKindWfi = 9,      // WFI: sleeps until an enabled interrupt is pending
  kEg2IsaKindCsr = 10,     // a CSR access (field `csr`)
  kEg2IsaKindCapMem = 11,  // CHERIoT: a memory access or jump through a capability register
} eg2_isa_kind_t;

typedef struct eg2_isa_info {
  bool legal;
  bool x16;            // CHERIoT mode: illegal because a register field names x16-x31
  uint8_t cls;         // eg2_isa_class_t
  uint8_t kind;        // eg2_isa_kind_t (legal encodings)
  uint8_t base;        // base register field (Load, Store, XferReg)
  uint8_t size;        // store size in bytes
  int32_t off;         // offset (Load, Store, XferPc, XferReg)
  uint16_t csr;        // CSR number (Csr)
  bool csr_write;      // the CSR form writes (or attempts to write) the CSR
} eg2_isa_info_t;

/** Decodes `insn` (a 16-bit parcel zero-extended, or a 32-bit word) for the mode. */
void eg2_isa_decode(uint32_t insn, bool cheriot, eg2_isa_info_t *out);

/** True for a CSR that exists in the mode (machine mode, outside debug mode). */
bool eg2_isa_csr_exists(uint32_t csr, bool cheriot);

/**
 * The code slot an encoding runs in: 32 bytes (8 words), purely a function of the encoding and
 * the mode, so the bench builds the same words:
 *   +0  probe0: for a CSR form `csrrs R, csr, x0` (reads the CSR's value into R); for CSpecialRW
 *       in CHERIoT mode `cspecialrw cR, scr, c0`; otherwise `nop`. A probe that traps (the CSR
 *       does not exist) is resumed at +4 by the harness.
 *   +4  probe1: CSRRW with rs1 != x0: `addi rs1, R, 0` (writes the value it read: no change);
 *       CSRRS/CSRRC with rs1 != x0: `addi rs1, x0, 0` (sets or clears no bit); CSpecialRW with
 *       cs1 != c0: `cmove cs1, cR`; otherwise `nop`. Never for a register CHERIoT mode lacks.
 *   +8  the encoding (2 or 4 bytes)
 *   then, for a CSR form that changes the CSR (CSRRW with rs1 = x0, CSRRWI, CSRRSI/CSRRCI with
 *       uimm != 0), `csrrw x0, csr, R`, which puts the value back at once;
 *   then `ebreak` (the end: the harness reads a breakpoint there as "fell through");
 *   the rest 0 (c.unimp).
 * R is the first of x5, x6, x7, x8 that is neither rd nor rs1 of the encoding.
 */
enum {
  kEg2IsaSlotBytes = 32,
  kEg2IsaSlotWords = 8,
  kEg2IsaTestOff = 8,
};
typedef struct eg2_isa_slot {
  uint32_t w[kEg2IsaSlotWords];
  uint32_t end_off;     // byte offset of the ebreak
  uint32_t probe_reg;   // R, or 0 without a probe
  uint32_t probe1_reg;  // the register probe1 writes, or 0
  bool probe1_copy;     // probe1 copies R (else writes 0)
} eg2_isa_slot_t;

void eg2_isa_slot(uint32_t insn, bool cheriot, eg2_isa_slot_t *slot);

/** Encodings the slot uses. */
enum {
  kEg2IsaNop = 0x00000013,     // addi x0, x0, 0
  kEg2IsaEbreak = 0x00100073,  // ebreak
};

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CIB_EG2_ISA_DECODE_H_
