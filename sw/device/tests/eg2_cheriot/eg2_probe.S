// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The expected-trap handler, the probes and the stubs of the CHERIoT suite (eg2_probe.h), for
 * both execution modes. Only x0-x15 are used, as CHERIoT mode has no others.
 *
 * Every instruction is 4 bytes (`norvc`) unless marked: the stubs and snippets have exact
 * layouts, and the tests' fetch model mirrors them.
 */

// eg2_trap_rec_t offsets (eg2_probe.h; checked by _Static_assert in eg2_probe.c).
#define REC_RECOVER 0
#define REC_SAVE1 8
#define REC_SAVE2 16
#define REC_MEPCC 24
#define REC_CAP 32
#define REC_PREV_TCC 40
#define REC_ARMED 48
#define REC_MCAUSE 52
#define REC_MTVAL 56
#define REC_MSTATUS 60
#define REC_COUNT 64
#define REC_VALUE 68
#define REC_VALUE2 72
#define REC_MEPCC_HI 76
#define REC_MEPCC_TAG 80
#define REC_IRQ 84
#define REC_IRQ_MEPC 96

#define MSTATUS_MPP 0x1800

  .option norvc
  .option norelax

#ifdef __CHERIOT__

// ============================== CHERIoT mode ==============================

  .section .text.eg2_probe, "ax", @progbits

/**
 * The handler (MTCC while installed). MScratchC holds the record; it is swapped with ct0 on
 * entry and back on exit, so every register is as it was when the trap was taken.
 */
  .balign 64
  .globl eg2_probe_trap
  .type eg2_probe_trap, @function
eg2_probe_trap:
  cspecialrw ct0, mscratchc, ct0
  csc        ct1, REC_SAVE1(ct0)
  csc        ct2, REC_SAVE2(ct0)
  csrr       t1, mcause
  bltz       t1, .Ltrap_irq
  sw         t1, REC_MCAUSE(ct0)
  csrr       t1, mtval
  sw         t1, REC_MTVAL(ct0)
  csrr       t1, mstatus
  sw         t1, REC_MSTATUS(ct0)
  cspecialr  ct1, mepcc
  csc        ct1, REC_MEPCC(ct0)
  cgethigh   t2, ct1
  sw         t2, REC_MEPCC_HI(ct0)
  cgettag    t2, ct1
  sw         t2, REC_MEPCC_TAG(ct0)
  lw         t1, REC_COUNT(ct0)
  addi       t1, t1, 1
  sw         t1, REC_COUNT(ct0)
  lw         t1, REC_ARMED(ct0)
  beqz       t1, .Ltrap_unexpected
  sw         zero, REC_ARMED(ct0)
  clc        ct1, REC_RECOVER(ct0)
  cspecialw  mepcc, ct1
.Ltrap_out:
  clc        ct2, REC_SAVE2(ct0)
  clc        ct1, REC_SAVE1(ct0)
  cspecialrw ct0, mscratchc, ct0
  mret

  // An interrupt: count it, mask it in mie, return to where it hit.
.Ltrap_irq:
  andi       t1, t1, 0x1f
  addi       t2, x0, 12
  bgeu       t1, t2, .Ltrap_unexpected
  srli       t2, t1, 2              // 3 -> 0, 7 -> 1, 11 -> 2
  slli       t2, t2, 2
  cincoffset ct2, ct0, t2
  // The count in t1, not t2: t2 is ct2's address, and a load into it untagged the pointer, so the
  // store faulted inside the handler, which re-entered itself with ct0 swapped back and never
  // returned (2026-10-27: rand_traps_cheriot hung silently at its first interrupt).
  lw         t1, REC_IRQ(ct2)
  addi       t1, t1, 1
  sw         t1, REC_IRQ(ct2)
  csrr       t1, mcause
  andi       t1, t1, 0x1f
  addi       t2, x0, 1
  sll        t2, t2, t1
  csrc       mie, t2
  cspecialr  ct1, mepcc
  sw         t1, REC_IRQ_MEPC(ct0)
  j          .Ltrap_out

  // Not expected: the framework's handler reports it (`CTRAP ...`) and fails the test.
.Ltrap_unexpected:
  clc        ct2, REC_SAVE2(ct0)
  clc        ct1, REC_SAVE1(ct0)
  cspecialrw ct0, mscratchc, ct0
  j          _eg2_cheriot_trap
  .size eg2_probe_trap, . - eg2_probe_trap

// Arms the handler with the recovery point `label`. Clobbers ct0, ct1, t0.
.macro ARM label
  cspecialr  ct1, mscratchc
  cllc       ct0, \label
  csc        ct0, REC_RECOVER(ct1)
  addi       t0, x0, 1
  sw         t0, REC_ARMED(ct1)
.endm

// Disarms; a0 = 1 if a trap was taken. Saves a2, a1, ca2 in the record. Returns through ca5.
.macro DISARM_RETURN
  cspecialr  ct1, mscratchc
  lw         t0, REC_ARMED(ct1)
  sw         zero, REC_ARMED(ct1)
  sw         a2, REC_VALUE(ct1)
  sw         a1, REC_VALUE2(ct1)
  csc        ca2, REC_CAP(ct1)
  xori       a0, t0, 1
  cmove      cra, ca5
  cret
.endm

// bool eg2_probe_call(void *op0 /*ca0*/, void *op1 /*ca1*/, void *init /*ca2*/, void *stub /*ca3*/)
  .balign 4
  .globl eg2_probe_call
  .type eg2_probe_call, @function
eg2_probe_call:
  cmove      ca5, cra
  ARM        .Lcall_recover
  cjalr      cra, 0(ca3)
.Lcall_recover:
  DISARM_RETURN
  .size eg2_probe_call, . - eg2_probe_call

// bool eg2_probe_jump(void *op0 /*ca0*/, void *op1 /*ca1*/, void *target /*ca2*/, form /*a3*/)
  .balign 4
  .globl eg2_probe_jump
  .type eg2_probe_jump, @function
eg2_probe_jump:
  cmove      ca5, cra
  mv         t2, a3
  cmove      ca3, ca2
  addi       a2, x0, 0
  ARM        .Ljump_recover
  beqz       t2, .Ljump_call
  addi       t0, x0, 1
  beq        t2, t0, .Ljump_tail
  addi       t0, x0, 2
  beq        t2, t0, .Ljump_other
  addi       t0, x0, 3
  beq        t2, t0, .Ljump_ret
  addi       t0, x0, 4
  beq        t2, t0, .Ljump_call_m2
  addi       t0, x0, 5
  beq        t2, t0, .Ljump_call_p2
  addi       t0, x0, 6
  beq        t2, t0, .Ljump_call_p4
  addi       t0, x0, 7
  beq        t2, t0, .Ljump_c_call
  addi       t0, x0, 8
  beq        t2, t0, .Ljump_c_tail
  // 9: MEPCC = target; mret.
  cspecialw  mepcc, ca3
  mret
.Ljump_call:
  cjalr      cra, 0(ca3)
  j          .Ljump_recover
.Ljump_call_m2:
  cjalr      cra, -2(ca3)
  j          .Ljump_recover
.Ljump_call_p2:
  cjalr      cra, 2(ca3)
  j          .Ljump_recover
.Ljump_call_p4:
  cjalr      cra, 4(ca3)
  j          .Ljump_recover
.Ljump_c_call:
  .option push
  .option rvc
  c.cjalr    ca3
  .option pop
  j          .Ljump_recover
  // Tail calls: cra is first made a return sentry to the recovery point.
.Ljump_tail:
  cjal       cra, .Ljump_tail_tramp
  j          .Ljump_recover
.Ljump_other:
  cjal       cra, .Ljump_other_tramp
  j          .Ljump_recover
.Ljump_c_tail:
  cjal       cra, .Ljump_c_tail_tramp
  j          .Ljump_recover
.Ljump_ret:
  cmove      cra, ca3
  cjalr      c0, 0(cra)
.Ljump_recover:
  DISARM_RETURN
.Ljump_tail_tramp:
  cjalr      c0, 0(ca3)
.Ljump_other_tramp:
  cjalr      ct2, 0(ca3)
.Ljump_c_tail_tramp:
  .option push
  .option rvc
  c.cjr      ca3
  .option pop
  .size eg2_probe_jump, . - eg2_probe_jump

// void *eg2_probe_scr_write(uint32_t scr /*a0*/, void *value /*ca1*/)
  .balign 4
  .globl eg2_probe_scr_write
  .type eg2_probe_scr_write, @function
eg2_probe_scr_write:
  addi       t0, x0, 28
  beq        a0, t0, .Lscr_mtcc
  cspecialr  ct0, mepcc
  cspecialw  mepcc, ca1
  cspecialr  ca0, mepcc
  cspecialw  mepcc, ct0
  cret
.Lscr_mtcc:
  cspecialr  ct0, mtcc
  cspecialw  mtcc, ca1
  cspecialr  ca0, mtcc
  cspecialw  mtcc, ct0
  cret
  .size eg2_probe_scr_write, . - eg2_probe_scr_write

// bool eg2_probe_call_umode(...): RISC-V mode only.
  .balign 4
  .globl eg2_probe_call_umode
  .type eg2_probe_call_umode, @function
eg2_probe_call_umode:
  addi       a0, x0, 0
  cret
  .size eg2_probe_call_umode, . - eg2_probe_call_umode

// ------------------------------- Stubs -------------------------------

.macro STUB name
  .balign 8
  .globl eg2_stub_\name
  .type eg2_stub_\name, @function
eg2_stub_\name:
.endm
.macro END name
  .size eg2_stub_\name, . - eg2_stub_\name
.endm

  .section .text.eg2_stubs, "ax", @progbits
STUB lb
  lb         a2, 0(ca0)
  cret
END lb
STUB lbu
  lbu        a2, 0(ca0)
  cret
END lbu
STUB lh
  lh         a2, 0(ca0)
  cret
END lh
STUB lhu
  lhu        a2, 0(ca0)
  cret
END lhu
STUB lw
  lw         a2, 0(ca0)
  cret
END lw
STUB sb
  sb         a1, 0(ca0)
  cret
END sb
STUB sh
  sh         a1, 0(ca0)
  cret
END sh
STUB sw
  sw         a1, 0(ca0)
  cret
END sw
STUB clc
  clc        ca2, 0(ca0)
  cret
END clc
STUB csc
  csc        ca1, 0(ca0)
  cret
END csc
  .option push
  .option rvc
STUB c_lw
  c.lw       a2, 0(ca0)
  c.cjr      cra
END c_lw
STUB c_sw
  c.sw       a1, 0(ca0)
  c.cjr      cra
END c_sw
STUB c_clc
  c.clc      ca2, 0(ca0)
  c.cjr      cra
END c_clc
STUB c_csc
  c.csc      ca1, 0(ca0)
  c.cjr      cra
END c_csc
  .option pop

STUB csr_mstatus
  csrr       a2, mstatus
  cret
END csr_mstatus
STUB csr_mcycle
  csrr       a2, mcycle
  cret
END csr_mcycle
STUB csr_cycle
  csrr       a2, cycle
  cret
END csr_cycle
STUB csr_hpm3
  csrr       a2, hpmcounter3
  cret
END csr_hpm3
STUB scr_mtdc
  cspecialr  ca2, mtdc
  cret
END scr_mtdc
STUB scr_mtcc
  cspecialr  ca2, mtcc
  cret
END scr_mtcc
STUB mret
  mret
END mret

// AUIPCC and AUICGP with each immediate class: 0, 1, -1, the largest and the smallest.
.macro AUIPCC_STUB name, imm
STUB auipcc_\name
  auipcc     ca2, \imm
  cret
END auipcc_\name
.endm
AUIPCC_STUB 0, 0
AUIPCC_STUB 1, 1
AUIPCC_STUB m1, 0xfffff
AUIPCC_STUB max, 0x7ffff
AUIPCC_STUB min, 0x80000

// CGP is swapped for ca0 around the instruction (no trap can come in between).
.macro AUICGP_STUB name, imm
STUB auicgp_\name
  cmove      ct0, cgp
  cmove      cgp, ca0
  auicgp     ca2, \imm
  cmove      cgp, ct0
  cret
END auicgp_\name
.endm
AUICGP_STUB 0, 0
AUICGP_STUB 1, 1
AUICGP_STUB m1, 0xfffff
AUICGP_STUB max, 0x7ffff
AUICGP_STUB min, 0x80000

// Arrival: records the link register the jump left (its type in a2, high word in a1).
STUB arrive
  cgettype   a2, cra
  cgethigh   a1, cra
  ecall
END arrive
  .option push
  .option rvc
STUB arrive_c
  c.ebreak
END arrive_c
  .option pop

STUB illegal_zero
  .4byte 0x00000000
  ecall
END illegal_zero
STUB illegal_ones
  .4byte 0xffffffff
  ecall
END illegal_ones
STUB illegal_cgetoffset
  .insn r 0x5b, 0, 0x7f, a2, a0, x6   // CGetOffset a2, ca0 (CHERI-RISC-V v8 encoding)
  ecall
END illegal_cgetoffset
STUB illegal_uccsr
  csrr       a2, 0x8c0
  ecall
END illegal_uccsr
STUB illegal_mccsr
  csrr       a2, 0xbc0
  ecall
END illegal_mccsr
STUB illegal_x16
  .4byte 0x00000813                   // addi x16, x0, 0
  ecall
END illegal_x16
STUB illegal_cheri_in_rv
  .4byte 0x10a5055b                   // csetbounds ca0, ca0, a0 (custom-2)
  ecall
END illegal_cheri_in_rv
STUB ecall
  ecall
END ecall
STUB ebreak
  ebreak
END ebreak
STUB wfi
  wfi
  cret
END wfi

// ------------------------------ Snippets ------------------------------
// Entered by eg2_probe_jump with a PCC whose bounds the test chose; each ends in a trap that
// marks where it arrived (ecall, ebreak) unless a fetch fails first.

  .section .text.eg2_snippets, "ax", @progbits
.macro SNIP name
  .balign 64
  .globl eg2_snip_\name
  .type eg2_snip_\name, @function
eg2_snip_\name:
.endm
.macro SEND name
  .size eg2_snip_\name, . - eg2_snip_\name
.endm

SNIP w
  addi       x0, x0, 0
  ecall
SEND w
  .option push
  .option rvc
SNIP c
  c.nop
  c.ebreak
SEND c
SNIP cw
  c.nop
  .option norvc
  ecall
SEND cw
  .option pop
SNIP cjal
  j          1f
  .4byte 0xffffffff
1:
  ecall
SEND cjal
SNIP beq
  beq        x0, x0, 1f
  .4byte 0xffffffff
1:
  ecall
SEND beq
SNIP back
1:
  ecall
  j          1b
SEND back
SNIP fall
  addi       x0, x0, 0
  addi       x0, x0, 0
  ecall
SEND fall
  .option push
  .option rvc
SNIP cj
  c.j        1f
  .2byte 0x0000
1:
  c.ebreak
SEND cj
  .option pop
SNIP nops
  .rept 15
  addi       x0, x0, 0
  .endr
  ecall
SEND nops

#else  // RISC-V mode

// ============================== RISC-V mode ==============================

  .section .text.eg2_probe, "ax", @progbits

/**
 * Ibex's mtvec is vectored only, with a 256-byte aligned base (Ibex manual, "mtvec"): an exception
 * enters at the base, interrupt n at base + 4n, the NMI at +0x7c. So the handler is a 32-entry
 * table at a 256-byte boundary, every entry jumping to one body. A 64-byte aligned handler passed
 * in eg2sim, but on the chip a trap entered below it (regr-vlt 2026-10-25).
 */
  .balign 256
  .globl eg2_probe_trap
  .type eg2_probe_trap, @function
eg2_probe_trap:
  .rept 32
  j          .Lprobe_trap_body
  .endr
.Lprobe_trap_body:
  csrrw      t0, mscratch, t0
  sw         t1, REC_SAVE1(t0)
  sw         t2, REC_SAVE2(t0)
  csrr       t1, mcause
  bltz       t1, .Ltrap_irq
  sw         t1, REC_MCAUSE(t0)
  csrr       t1, mtval
  sw         t1, REC_MTVAL(t0)
  csrr       t1, mstatus
  sw         t1, REC_MSTATUS(t0)
  csrr       t1, mepc
  sw         t1, REC_MEPCC(t0)
  lw         t1, REC_COUNT(t0)
  addi       t1, t1, 1
  sw         t1, REC_COUNT(t0)
  lw         t1, REC_ARMED(t0)
  beqz       t1, .Ltrap_unexpected
  sw         zero, REC_ARMED(t0)
  lw         t1, REC_RECOVER(t0)
  csrw       mepc, t1
  li         t1, MSTATUS_MPP        // back to M mode after a U-mode probe
  csrs       mstatus, t1
.Ltrap_out:
  lw         t2, REC_SAVE2(t0)
  lw         t1, REC_SAVE1(t0)
  csrrw      t0, mscratch, t0
  mret

.Ltrap_irq:
  andi       t1, t1, 0x1f
  addi       t2, x0, 12
  bgeu       t1, t2, .Ltrap_unexpected
  srli       t2, t1, 2
  slli       t2, t2, 2
  add        t2, t2, t0
  lw         t1, REC_IRQ(t2)
  addi       t1, t1, 1
  sw         t1, REC_IRQ(t2)
  csrr       t1, mcause
  andi       t1, t1, 0x1f
  addi       t2, x0, 1
  sll        t2, t2, t1
  csrc       mie, t2
  csrr       t1, mepc
  sw         t1, REC_IRQ_MEPC(t0)
  j          .Ltrap_out

  // Not expected: OTTF's vector back, and the instruction retried, so that it faults into OTTF.
.Ltrap_unexpected:
  lw         t1, REC_PREV_TCC(t0)
  csrw       mtvec, t1
  lw         t2, REC_SAVE2(t0)
  lw         t1, REC_SAVE1(t0)
  csrrw      t0, mscratch, t0
  mret
  .size eg2_probe_trap, . - eg2_probe_trap

.macro ARM label
  csrr       t1, mscratch
  la         t0, \label
  sw         t0, REC_RECOVER(t1)
  addi       t0, x0, 1
  sw         t0, REC_ARMED(t1)
.endm

.macro DISARM_RETURN
  csrr       t1, mscratch
  lw         t0, REC_ARMED(t1)
  sw         zero, REC_ARMED(t1)
  sw         a2, REC_VALUE(t1)
  sw         a1, REC_VALUE2(t1)
  sw         a2, REC_CAP(t1)
  xori       a0, t0, 1
  mv         ra, a5
  ret
.endm

  .balign 4
  .globl eg2_probe_call
  .type eg2_probe_call, @function
eg2_probe_call:
  mv         a5, ra
  ARM        .Lcall_recover
  jalr       ra, 0(a3)
.Lcall_recover:
  DISARM_RETURN
  .size eg2_probe_call, . - eg2_probe_call

// Every form is a plain call in RISC-V mode.
  .balign 4
  .globl eg2_probe_jump
  .type eg2_probe_jump, @function
eg2_probe_jump:
  mv         a5, ra
  mv         a3, a2
  addi       a2, x0, 0
  ARM        .Ljump_recover
  jalr       ra, 0(a3)
.Ljump_recover:
  DISARM_RETURN
  .size eg2_probe_jump, . - eg2_probe_jump

// bool eg2_probe_call_umode(void *op0 /*a0*/, void *op1 /*a1*/, void *stub /*a2*/)
  .balign 4
  .globl eg2_probe_call_umode
  .type eg2_probe_call_umode, @function
eg2_probe_call_umode:
  mv         a5, ra
  mv         a3, a2
  ARM        .Lu_recover
  csrw       mepc, a3
  li         t0, MSTATUS_MPP
  csrc       mstatus, t0
  la         ra, .Lu_ecall
  mret
.Lu_ecall:
  ecall
.Lu_recover:
  DISARM_RETURN
  .size eg2_probe_call_umode, . - eg2_probe_call_umode

  .balign 4
  .globl eg2_probe_scr_write
  .type eg2_probe_scr_write, @function
eg2_probe_scr_write:
  mv         a0, a1
  ret
  .size eg2_probe_scr_write, . - eg2_probe_scr_write

.macro STUB name
  .balign 8
  .globl eg2_stub_\name
  .type eg2_stub_\name, @function
eg2_stub_\name:
.endm
.macro END name
  .size eg2_stub_\name, . - eg2_stub_\name
.endm

  .section .text.eg2_stubs, "ax", @progbits
STUB lb
  lb         a2, 0(a0)
  ret
END lb
STUB lbu
  lbu        a2, 0(a0)
  ret
END lbu
STUB lh
  lh         a2, 0(a0)
  ret
END lh
STUB lhu
  lhu        a2, 0(a0)
  ret
END lhu
STUB lw
  lw         a2, 0(a0)
  ret
END lw
STUB sb
  sb         a1, 0(a0)
  ret
END sb
STUB sh
  sh         a1, 0(a0)
  ret
END sh
STUB sw
  sw         a1, 0(a0)
  ret
END sw
  .option push
  .option rvc
STUB c_lw
  c.lw       a2, 0(a0)
  c.jr       ra
END c_lw
STUB c_sw
  c.sw       a1, 0(a0)
  c.jr       ra
END c_sw
  .option pop
STUB arrive
  ecall
END arrive
  .option push
  .option rvc
STUB arrive_c
  c.ebreak
END arrive_c
  .option pop
STUB illegal_zero
  .4byte 0x00000000
  ecall
END illegal_zero
STUB illegal_ones
  .4byte 0xffffffff
  ecall
END illegal_ones
STUB illegal_uccsr
  csrr       a2, 0x8c0
  ecall
END illegal_uccsr
STUB illegal_cheri_in_rv
  .4byte 0x10a5055b
  ecall
END illegal_cheri_in_rv
STUB csr_mstatus
  csrr       a2, mstatus
  ret
END csr_mstatus
STUB ecall
  ecall
END ecall
STUB ebreak
  ebreak
END ebreak
STUB wfi
  wfi
  ret
END wfi

  .section .text.eg2_snippets, "ax", @progbits
  .balign 64
  .globl eg2_snip_nops
  .type eg2_snip_nops, @function
eg2_snip_nops:
  .rept 15
  addi       x0, x0, 0
  .endr
  ecall
  .size eg2_snip_nops, . - eg2_snip_nops

#endif  // __CHERIOT__
