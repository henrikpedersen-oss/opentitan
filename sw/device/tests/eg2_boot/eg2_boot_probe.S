// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * One load, store or fetch that may fault (eg2_boot.h). The probe installs its own trap handler
 * for the access and puts the framework's back after it; the handler records mcause and mtval
 * and resumes at the probe's recovery point. Interrupts must be disabled.
 *
 * RISC-V mode: Ibex's `mtvec` is vectored only, with a 256-byte aligned base, and exceptions
 * enter at the base: the handler is aligned to 256 bytes. CHERIoT mode: MTCC, direct.
 *
 * Registers while armed: t0/ct0 the framework's handler, t2/ct2 the recovery point, a5 mcause
 * (0 = no trap), a4 mtval, a2 the loaded value, a3/ca3 the return address. Only x0-x15 are used,
 * as CHERIoT mode has no others.
 *
 * uint32_t eg2_boot_probe_asm(volatile void *ptr (a0), uint32_t op (a1),
 *                             uint32_t wdata (a2), eg2_boot_out_t *out (a3))
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_boot_probe, "ax", @progbits
  .balign 4
  .globl eg2_boot_probe_asm
  .type eg2_boot_probe_asm, @function
eg2_boot_probe_asm:
  // out to ca5 is not possible (a5 is mcause): keep it on the stack.
  cincoffsetimm csp, csp, -16
  csc        ca3, 0(csp)
  csc        cra, 8(csp)
  cspecialr  ct0, mtcc
  cllc       ct1, .Lbp_trap
  cspecialw  mtcc, ct1
  cllc       ct2, .Lbp_recover
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 1
  beq        a1, t1, .Lbp_store
  addi       t1, x0, 2
  beq        a1, t1, .Lbp_fetch
  lw         a2, 0(ca0)
  j          .Lbp_recover
.Lbp_store:
  sw         a2, 0(ca0)
  addi       a2, x0, 0
  j          .Lbp_recover
.Lbp_fetch:
  // The target holds 0, an illegal instruction: a fetch the hardware allows traps with cause 2.
  addi       a2, x0, 0
  cjalr      cra, ca0
.Lbp_recover:
  cspecialw  mtcc, ct0
  clc        ca3, 0(csp)
  clc        cra, 8(csp)
  cincoffsetimm csp, csp, 16
  sw         a2, 0(ca3)
  sw         a4, 4(ca3)
  mv         a0, a5
  cret

  .balign 4
.Lbp_trap:
  csrr       a5, mcause
  csrr       a4, mtval
  addi       a2, x0, 0
  cspecialw  mepcc, ct2
  mret
  .size eg2_boot_probe_asm, . - eg2_boot_probe_asm

#else  // RISC-V mode

  .section .text.eg2_boot_probe, "ax", @progbits
  .balign 4
  .globl eg2_boot_probe_asm
  .type eg2_boot_probe_asm, @function
eg2_boot_probe_asm:
  addi       sp, sp, -16
  sw         a3, 0(sp)
  sw         ra, 4(sp)
  la         t1, eg2_boot_probe_trap
  csrrw      t0, mtvec, t1
  la         t2, .Lbp_recover
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 1
  beq        a1, t1, .Lbp_store
  addi       t1, x0, 2
  beq        a1, t1, .Lbp_fetch
  lw         a2, 0(a0)
  j          .Lbp_recover
.Lbp_store:
  sw         a2, 0(a0)
  addi       a2, x0, 0
  j          .Lbp_recover
.Lbp_fetch:
  addi       a2, x0, 0
  jalr       ra, 0(a0)
.Lbp_recover:
  csrw       mtvec, t0
  lw         a3, 0(sp)
  lw         ra, 4(sp)
  addi       sp, sp, 16
  sw         a2, 0(a3)
  sw         a4, 4(a3)
  mv         a0, a5
  ret
  .size eg2_boot_probe_asm, . - eg2_boot_probe_asm

  // Ibex: vectored mtvec only; base 256-byte aligned; exceptions enter at the base. The
  // framework's mtvec has mode bits 01, and `csrrw` above writes ours with the same mode
  // (bit 0 of the label's address is 0, Ibex forces the mode to vectored).
  .balign 256
  .globl eg2_boot_probe_trap
  .type eg2_boot_probe_trap, @function
eg2_boot_probe_trap:
  csrr       a5, mcause
  csrr       a4, mtval
  addi       a2, x0, 0
  csrw       mepc, t2
  mret
  // The NMI, and the internal interrupt of a load integrity error (mcause 0xFFFFFFE0), enter at
  // base + 0x7C (Ibex manual "Internal Interrupts"): without this entry they ran the code after
  // the handler (found in eg2_regmem_probe.S, regr-vlt 2026-10-26). 2026-10-26.
  .org eg2_boot_probe_trap + 0x7C
  j          eg2_boot_probe_trap
  .size eg2_boot_probe_trap, . - eg2_boot_probe_trap

#endif  // __CHERIOT__
