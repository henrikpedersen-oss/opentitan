// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_PROBE_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_PROBE_H_

/**
 * @file
 * @brief Expected traps: run one operation that may trap, and record the trap if it does.
 *
 * The framework's own trap vector ends the test on any trap. The CHERIoT suite's tests trap on
 * purpose, thousands of times, so while one runs, `eg2_probe_begin()` installs this library's
 * handler (MTCC in CHERIoT mode, `mtvec` in RISC-V mode) and `eg2_probe_end()` puts the
 * framework's back.
 *
 * A probe arms the handler, calls a stub (one instruction and a return, or a jump into a code
 * snippet that ends in `ecall` or `ebreak`), and disarms it. If the stub traps, the handler records
 * `mcause`, `mtval`, `mstatus` and MEPCC (`mepc`) and resumes at the probe's recovery point with
 * every register as it was at the trap. A trap while no probe is armed is a real failure: in
 * CHERIoT mode the handler hands it to the framework's handler (`CTRAP ...`), in RISC-V mode it puts
 * OTTF's vector back and retries the instruction, which then faults into OTTF.
 *
 * Interrupts are counted per cause (software, timer, external) and masked in `mie`; the test
 * clears the source and unmasks it again.
 *
 * Stub calling convention (both modes): operands in a0/ca0 (the authority or target operand) and
 * a1/ca1 (store data, second operand); result in a2/ca2. ca3 holds the stub, ca5 the probe's
 * return address; stubs touch nothing else.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A pointer-sized slot, eight bytes in both modes, so the assembly's offsets are shared. */
typedef union eg2_probe_slot {
  void *p;
  uint64_t pad;
} eg2_probe_slot_t;

/** The trap record the handler writes (MScratchC / `mscratch` points at it while installed). */
typedef struct eg2_trap_rec {
  eg2_probe_slot_t recover;   // 0: where an armed trap resumes
  eg2_probe_slot_t save1;     // 8: the handler's register saves
  eg2_probe_slot_t save2;     // 16
  eg2_probe_slot_t mepcc;     // 24: MEPCC (`mepc`) of the last trap
  eg2_probe_slot_t cap;       // 32: ca2 after the probe
  eg2_probe_slot_t prev_tcc;  // 40: the framework's MTCC (`mtvec`)
  uint32_t armed;             // 48: 1 while a probe expects a possible trap
  uint32_t mcause;            // 52
  uint32_t mtval;             // 56
  uint32_t mstatus;           // 60
  uint32_t count;             // 64: synchronous traps taken
  uint32_t value;             // 68: a2 after the probe
  uint32_t value2;            // 72: a1 after the probe
  uint32_t mepcc_hi;          // 76: CGetHigh of MEPCC (CHERIoT)
  uint32_t mepcc_tag;         // 80: CGetTag of MEPCC (CHERIoT)
  uint32_t irq_count[3];      // 84: software (3), timer (7), external (11) interrupts
  uint32_t irq_mepc;          // 96: mepc of the last interrupt
  uint32_t mpp_m;             // 100: RISC-V mode: set MPP to M on an armed trap (U-mode probes)
} eg2_trap_rec_t;

/**
 * The record; valid between eg2_probe_begin and eg2_probe_end. Reached through a function that is
 * never inlined: the CHERIoT toolchain's link-time outliner drops the field offset of a
 * CGP-relative access to a global struct's member (lowRISC CHERIoT clang 22.1.7), so no code
 * may address a field of the record directly from CGP.
 */
eg2_trap_rec_t *eg2_rec(void);
#define eg2_trap_rec (*eg2_rec())

/** Installs the handler (and saves the framework's). */
void eg2_probe_begin(void);
/** Puts the framework's handler back. */
void eg2_probe_end(void);

/**
 * Calls `stub` with a0/ca0 = `op0`, a1/ca1 = `op1`, a2/ca2 = `init` under an armed handler. Returns
 * true if it trapped; `eg2_trap_rec` then holds the trap. The stub's a2/a1/ca2 are in
 * `eg2_trap_rec.value`, `.value2` and `.cap` either way.
 */
bool eg2_probe_call(void *op0, void *op1, void *init, void *stub);

/** The ways eg2_probe_jump enters its target (CHERIoT mode). */
typedef enum eg2_jump_form {
  kEg2JumpCall = 0,       // cjalr cra, 0(ca3)
  kEg2JumpTail = 1,       // cjalr c0, 0(ca3)
  kEg2JumpOther = 2,      // cjalr ct2, 0(ca3)
  kEg2JumpRet = 3,        // cra = target; cjalr c0, 0(cra)
  kEg2JumpCallM2 = 4,     // cjalr cra, -2(ca3)
  kEg2JumpCallP2 = 5,     // cjalr cra, 2(ca3)
  kEg2JumpCallP4 = 6,     // cjalr cra, 4(ca3)
  kEg2JumpCCall = 7,      // c.cjalr ca3
  kEg2JumpCTail = 8,      // c.cjr ca3
  kEg2JumpMret = 9,       // MEPCC = target; mret
  kEg2JumpFormCount = 10,
} eg2_jump_form_t;

/**
 * Jumps into `target` by `form` under an armed handler (CHERIoT mode; RISC-V mode: every form is
 * a `jalr ra`). The target ends in a trap (ecall, ebreak, a fault), which is recorded. Returns true
 * if a trap was taken (always, unless the target returned).
 */
bool eg2_probe_jump(void *op0, void *op1, void *target, eg2_jump_form_t form);

/** RISC-V mode: runs `stub` in U mode (mret with MPP = U); it ends in `ecall` from U. */
bool eg2_probe_call_umode(void *op0, void *op1, void *stub);

/**
 * CHERIoT mode: swaps `value` into MTCC (28) or MEPCC (31), reads it back, and restores the
 * original, with no trap possible in between. Returns what the register held after the write.
 */
void *eg2_probe_scr_write(uint32_t scr, void *value);

// ---------------------------------------------------------------------------------------------
// Stubs (eg2_probe.S). Each is `<one instruction>` then a return (`cret` / `ret`), unless noted.
// Loads put the value in a2/ca2; stores take a1/ca1; the address is a0/ca0 (offset 0).

#define EG2_PROBE_LOAD_STORE_STUBS(X) \
  X(lb)                               \
  X(lbu)                              \
  X(lh)                               \
  X(lhu)                              \
  X(lw)                               \
  X(sb)                               \
  X(sh)                               \
  X(sw)                               \
  X(c_lw)                             \
  X(c_sw)

#define EG2_PROBE_DECLARE_(name) extern char eg2_stub_##name[];
EG2_PROBE_LOAD_STORE_STUBS(EG2_PROBE_DECLARE_)
#undef EG2_PROBE_DECLARE_

// CHERIoT mode only.
extern char eg2_stub_clc[], eg2_stub_csc[], eg2_stub_c_clc[], eg2_stub_c_csc[];
extern char eg2_stub_csr_mstatus[];    // csrr a2, mstatus
extern char eg2_stub_csr_mcycle[];     // csrr a2, mcycle (0xB00)
extern char eg2_stub_csr_cycle[];      // csrr a2, cycle (0xC00)
extern char eg2_stub_csr_hpm3[];       // csrr a2, hpmcounter3 (0xC03)
extern char eg2_stub_csr_mscratch_w[]; // csrw mscratch, a1
extern char eg2_stub_scr_mtdc[];       // cspecialr ca2, mtdc
extern char eg2_stub_scr_mtcc[];       // cspecialr ca2, mtcc
extern char eg2_stub_mret[];           // mret (only ever run without SR)
extern char eg2_stub_auipcc_0[], eg2_stub_auipcc_1[], eg2_stub_auipcc_m1[],
    eg2_stub_auipcc_max[], eg2_stub_auipcc_min[];
extern char eg2_stub_auicgp_0[], eg2_stub_auicgp_1[], eg2_stub_auicgp_m1[],
    eg2_stub_auicgp_max[], eg2_stub_auicgp_min[];

// Arrival stubs: the jump has landed when these trap.
extern char eg2_stub_arrive[];         // cgettype a2, cra ; cgethigh a1, cra ; ecall (CHERIoT)
                                       // ecall (RISC-V)
extern char eg2_stub_arrive_c[];       // c.ebreak

// Illegal-instruction stubs: each is one encoding then an `ecall` that is never reached.
extern char eg2_stub_illegal_zero[];   // 0x00000000 (c.unimp)
extern char eg2_stub_illegal_ones[];   // 0xffffffff
extern char eg2_stub_illegal_cgetoffset[];  // CHERIoT: CGetOffset, absent from CHERIoT
extern char eg2_stub_illegal_uccsr[];  // CHERIoT: csrr a2, 0x8c0 (uccsr, absent)
extern char eg2_stub_illegal_mccsr[];  // CHERIoT: csrr a2, 0xbc0 (mccsr, absent)
extern char eg2_stub_illegal_x16[];    // CHERIoT: addi x16, x0, 0 (RV32E has 16 registers)
extern char eg2_stub_illegal_cheri_in_rv[];  // RISC-V: csetbounds a0, a0, a0 (custom-2)
extern char eg2_stub_illegal_csr_absent[];  // RISC-V: csrr a2, 0x7c0 (no such CSR)
extern char eg2_stub_ecall[];          // ecall
extern char eg2_stub_ebreak[];         // ebreak (4 bytes)
extern char eg2_stub_wfi[];            // wfi ; ret

// Code snippets for PCC bounds edges (CHERIoT) and ePMP execute edges (RISC-V), 64-byte
// aligned. Their layout is in eg2_probe.S and mirrored in the tests' fetch model.
extern char eg2_snip_w[];       // +0 nop (4) ; +4 ecall
extern char eg2_snip_c[];       // +0 c.nop ; +2 c.ebreak
extern char eg2_snip_cw[];      // +0 c.nop ; +2 ecall (4 bytes at +2)
extern char eg2_snip_cjal[];    // +0 j +8 ; +4 unimp ; +8 ecall
extern char eg2_snip_beq[];     // +0 beq x0, x0, +8 ; +4 unimp ; +8 ecall
extern char eg2_snip_back[];    // +0 ecall ; +4 j -4 (entered at +4)
extern char eg2_snip_fall[];    // +0 nop ; +4 nop ; +8 ecall
extern char eg2_snip_cj[];      // +0 c.j +4 ; +2 c.unimp ; +4 c.ebreak
extern char eg2_snip_nops[];    // RISC-V: 15 nops then ecall at +60

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_PROBE_H_
