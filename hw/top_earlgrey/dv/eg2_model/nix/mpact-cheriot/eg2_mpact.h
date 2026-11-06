// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// libeg2_mpact: MPACT-CHERIoT (google/mpact-cheriot) behind a C interface, the CHERIoT-mode
// instruction-set model of the EG2 SystemC model (eg2sim's eg2_mpact_cpu.h). MPACT is built with
// Bazel, abseil and protobuf; the C interface keeps all of that inside the library, so eg2sim links
// one shared object built with its own toolchain (nixos-20.09: glibc 2.31, libstdc++ 9).
//
// Every access goes to the caller: instruction fetches through fetch, loads and stores through
// load and store. A capability access (CLC, CSC) is 8 bytes, 8-byte aligned, with one tag
// (tags != NULL); every other access has tags == NULL, and an untagged store clears the tag of
// each granule it writes. A callback returns 0, or non-zero for a bus error: the instruction then
// takes a load or store access fault.

#ifndef EG2_MODEL_MPACT_EG2_MPACT_H_
#define EG2_MODEL_MPACT_EG2_MPACT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct eg2_mpact eg2_mpact;

typedef struct {
  void *user;
  int (*fetch)(void *user, uint32_t addr, void *data, uint32_t len);
  int (*load)(void *user, uint32_t addr, void *data, uint32_t len, uint8_t *tags);
  int (*store)(void *user, uint32_t addr, const void *data, uint32_t len, const uint8_t *tags);
} eg2_mpact_bus;

// The state Spike hands over at the CHERIoT lock (REQ_SOC_07): the next PC, the trap CSRs and
// x1-x15 (gpr[0] unused). PCC, MTCC and MEPCC become the executable root with the address pc,
// mtvec and mepc; MTDC the memory root and MScratchC the sealing root; x1-x15 hold their values
// with tag 0; x16-x31 are null. misa replaces MPACT's own (RV32IMAFDC with S), which is not the
// core's.
typedef struct {
  uint32_t pc, mtvec, mepc, mstatus, mie, mcause, mtval, misa;
  uint32_t gpr[16];
} eg2_mpact_handover_state;

eg2_mpact *eg2_mpact_create(const eg2_mpact_bus *bus);

// The load barrier's revocation bitmap. A CLC that loads a tagged capability that is not a sealing
// capability (CHERIoT ISA, LoadCapImm) is revoked if its base is at or above heap_base and the
// bitmap bit of its base's 8-byte granule is set: bit ((base - heap_base) >> 3) % 8 of the byte at
// offset (base - heap_base) >> 6 of the bitmap. MPACT reads that byte through revbm, not through
// the bus's load (Ibex reads it through a port of its own, trvk_revbm_*); a non-zero return is a
// failed lookup, which revokes, as Ibex's does (ibex_trvk.sv revbm_err_i). With eg2_mpact_create
// MPACT keeps its own defaults: heap base 0x8000_0000, the bitmap a data load at 0x8300_0000.
typedef struct {
  uint32_t heap_base;
  void *user;
  int (*revbm)(void *user, uint32_t offset, uint8_t *byte);
} eg2_mpact_revocation;

eg2_mpact *eg2_mpact_create_revocation(const eg2_mpact_bus *bus, const eg2_mpact_revocation *rev);
void eg2_mpact_destroy(eg2_mpact *m);
void eg2_mpact_handover(eg2_mpact *m, const eg2_mpact_handover_state *s);
// The interrupt lines (mip.MEIP, MTIP, MSIP), before a step.
void eg2_mpact_set_irqs(eg2_mpact *m, int meip, int mtip, int msip);
// One instruction, and the interrupt the core takes after it. 0, or -1 if MPACT refused
// (eg2_mpact_error says why).
int eg2_mpact_step(eg2_mpact *m);
// An instruction access fault at the current PC, instead of a step: the fetch had a bus error.
void eg2_mpact_fetch_fault(eg2_mpact *m);
// Ibex's non-maskable interrupt (irq_nm_i, interrupt 31), taken before the instruction at the
// current PC whatever mstatus.MIE and mie say: mcause 0x8000001F, mtval 0, mepcc the current PC,
// to MTCC (in CHERIoT mode Ibex does not vector interrupts: ibex_if_stage.sv EXC_PC_IRQ).
void eg2_mpact_nmi(eg2_mpact *m);
// An instruction MPACT-CHERIoT does not decode, executed by the caller, instead of a step: the
// instruction at the current PC (len bytes) retires writing value to integer register rd (none for
// rd 0; the capability metadata is null, the tag 0), and the core takes a pending interrupt after
// it. For FENCE.I (riscv_cheriot.bin_fmt has its encoding commented out; nothing to do in the
// model) and the Zba/Zbb/Zbs/Zbc/Zbkb/Zbkx instructions of Ibex's RV32BFull, which MPACT has none
// of.
void eg2_mpact_step_emulated(eg2_mpact *m, uint32_t len, unsigned rd, uint32_t value);
// The last step executed a WFI (MPACT executes it as a no-op).
int eg2_mpact_wfi(const eg2_mpact *m);
uint32_t eg2_mpact_pc(eg2_mpact *m);
uint32_t eg2_mpact_gpr(eg2_mpact *m, unsigned r);
// A CSR by name ("mstatus", "mie", "mip", "mcause", "mtval", ...); 0, or -1 if MPACT has none.
int eg2_mpact_csr(eg2_mpact *m, const char *name, uint32_t *value);
// The disassembly of the instruction at pc (valid until the next call).
const char *eg2_mpact_disasm(eg2_mpact *m, uint32_t pc);
const char *eg2_mpact_error(const eg2_mpact *m);

#ifdef __cplusplus
}
#endif

#endif  // EG2_MODEL_MPACT_EG2_MPACT_H_
