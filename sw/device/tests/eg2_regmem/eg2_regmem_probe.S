// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * One access that may fault (eg2_regmem.h). The probe installs its own trap handler for the
 * access and puts the framework's back after it; the handler records mcause and mtval and
 * resumes at the probe's recovery point. Interrupts must be disabled. Only x0-x15 are used, as
 * CHERIoT mode has no others.
 *
 * Registers while armed: t0/ct0 the framework's handler, t2/ct2 the recovery point, a5 mcause
 * (0 = no trap), t1 mtval, a4/ca4 the loaded value.
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_rm_probe, "ax", @progbits

// uint32_t eg2_rm_probe(volatile void *addr /*ca0*/, uint32_t op /*a1*/, uint32_t wdata /*a2*/,
//                       eg2_rm_out_t *out /*ca3*/)
  .balign 4
  .globl eg2_rm_probe
  .type eg2_rm_probe, @function
eg2_rm_probe:
  cspecialr  ct0, mtcc
  cllc       ct1, .Lrm_trap
  cspecialw  mtcc, ct1
  cllc       ct2, .Lrm_recover
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 0
  beqz       a1, .Lrm_lb
  addi       a1, a1, -1
  beqz       a1, .Lrm_lh
  addi       a1, a1, -1
  beqz       a1, .Lrm_lw
  addi       a1, a1, -1
  beqz       a1, .Lrm_lbu
  addi       a1, a1, -1
  beqz       a1, .Lrm_lhu
  addi       a1, a1, -1
  beqz       a1, .Lrm_sb
  addi       a1, a1, -1
  beqz       a1, .Lrm_sh
  sw         a2, 0(ca0)
  j          .Lrm_recover
.Lrm_lb:
  lb         a4, 0(ca0)
  j          .Lrm_recover
.Lrm_lh:
  lh         a4, 0(ca0)
  j          .Lrm_recover
.Lrm_lw:
  lw         a4, 0(ca0)
  j          .Lrm_recover
.Lrm_lbu:
  lbu        a4, 0(ca0)
  j          .Lrm_recover
.Lrm_lhu:
  lhu        a4, 0(ca0)
  j          .Lrm_recover
.Lrm_sb:
  sb         a2, 0(ca0)
  j          .Lrm_recover
.Lrm_sh:
  sh         a2, 0(ca0)
.Lrm_recover:
  cspecialw  mtcc, ct0
  sw         a4, 0(ca3)
  sw         t1, 4(ca3)
  mv         a0, a5
  cret

  .balign 4
.Lrm_trap:
  csrr       a5, mcause
  csrr       t1, mtval
  cspecialw  mepcc, ct2
  mret
  .size eg2_rm_probe, . - eg2_rm_probe

// uint32_t eg2_rm_probe_cap(volatile void *addr /*ca0*/, uint32_t op /*a1*/, void *wcap /*ca2*/,
//                           eg2_rm_cap_out_t *out /*ca3*/)
  .balign 4
  .globl eg2_rm_probe_cap
  .type eg2_rm_probe_cap, @function
eg2_rm_probe_cap:
  cspecialr  ct0, mtcc
  cllc       ct1, .Lrmc_trap
  cspecialw  mtcc, ct1
  cllc       ct2, .Lrmc_recover
  addi       a5, x0, 0
  cmove      ca4, cnull
  addi       t1, x0, 0
  bnez       a1, .Lrmc_csc
  clc        ca4, 0(ca0)
  j          .Lrmc_recover
.Lrmc_csc:
  csc        ca2, 0(ca0)
.Lrmc_recover:
  cspecialw  mtcc, ct0
  csc        ca4, 0(ca3)
  sw         t1, 8(ca3)
  mv         a0, a5
  cret

  .balign 4
.Lrmc_trap:
  csrr       a5, mcause
  csrr       t1, mtval
  cspecialw  mepcc, ct2
  mret
  .size eg2_rm_probe_cap, . - eg2_rm_probe_cap

#else  // RISC-V mode

  .section .text.eg2_rm_probe, "ax", @progbits

// uint32_t eg2_rm_probe(volatile void *addr /*a0*/, uint32_t op /*a1*/, uint32_t wdata /*a2*/,
//                       eg2_rm_out_t *out /*a3*/)
  .balign 4
  .globl eg2_rm_probe
  .type eg2_rm_probe, @function
eg2_rm_probe:
  // Ibex's mtvec has vectored mode only, with a 256-byte aligned base (Ibex manual, "mtvec"):
  // exceptions enter at the base, interrupt n at base + 4n. So the handler is 256-byte aligned
  // and interrupts are off while it is installed. A 4-byte aligned handler passed in eg2sim and
  // was missed on the chip: every fault read back as cause 0 (regr-vlt 2026-10-25).
  csrrci     t3, mstatus, 8
  la         t1, .Lrm_trap
  ori        t1, t1, 1
  csrrw      t0, mtvec, t1
  la         t2, .Lrm_recover
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 0
  beqz       a1, .Lrm_lb
  addi       a1, a1, -1
  beqz       a1, .Lrm_lh
  addi       a1, a1, -1
  beqz       a1, .Lrm_lw
  addi       a1, a1, -1
  beqz       a1, .Lrm_lbu
  addi       a1, a1, -1
  beqz       a1, .Lrm_lhu
  addi       a1, a1, -1
  beqz       a1, .Lrm_sb
  addi       a1, a1, -1
  beqz       a1, .Lrm_sh
  sw         a2, 0(a0)
  j          .Lrm_recover
.Lrm_lb:
  lb         a4, 0(a0)
  j          .Lrm_recover
.Lrm_lh:
  lh         a4, 0(a0)
  j          .Lrm_recover
.Lrm_lw:
  lw         a4, 0(a0)
  j          .Lrm_recover
.Lrm_lbu:
  lbu        a4, 0(a0)
  j          .Lrm_recover
.Lrm_lhu:
  lhu        a4, 0(a0)
  j          .Lrm_recover
.Lrm_sb:
  sb         a2, 0(a0)
  j          .Lrm_recover
.Lrm_sh:
  sh         a2, 0(a0)
.Lrm_recover:
  csrw       mtvec, t0
  andi       t3, t3, 8
  csrs       mstatus, t3
  sw         a4, 0(a3)
  sw         t1, 4(a3)
  mv         a0, a5
  ret

  .balign 256
.Lrm_trap:
  csrr       a5, mcause
  csrr       t1, mtval
  csrw       mepc, t2
  mret
  .size eg2_rm_probe, . - eg2_rm_probe

#endif  // __CHERIOT__
