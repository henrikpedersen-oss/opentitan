// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The harness of chip_sw_eg2_illegal_sweep (eg2_illegal_sweep.c): runs one code slot
 * (eg2_isa_decode.h) with every register holding the data address D, and records the trap that
 * ends it.
 *
 *   void eg2_isa_run(uint32_t slot, uint32_t d, volatile void *ctx);   (a0, a1, a2 / ca2)
 *
 * The harness saves its caller's callee-saved registers in `ctx`, installs its own trap vector
 * (mtvec / MTCC), sets mepc / MEPCC to the slot, MPP = M and MPIE = 0, sets x1-x31 (x1-x15 in
 * CHERIoT mode, as untagged integers) to D and enters the slot with `mret`, so no register
 * differs from D when the slot starts. Every trap goes to the handler, which stores all
 * registers, mcause, mepc and mtval into `ctx`:
 *   - a trap at the slot's first word (its CSR or SCR probe, eg2_isa_decode.h) is resumed at the
 *     next word with every register as it was;
 *   - any other trap ends the run: the caller's registers come back from `ctx` and the call
 *     returns. The slot ends in `ebreak`, so a slot whose encoding fell through ends with a
 *     breakpoint at its end.
 * Interrupts stay off (MIE = MPIE = 0). Only x0-x15 are used by the code that runs in CHERIoT
 * mode.
 *
 * ctx layout (eg2_illegal_sweep.c kCtx*): the registers at 0 (RISC-V: 4 bytes each, x0-x31;
 * CHERIoT: 8 bytes each, c0-c15); 0x80 mcause, 0x84 mepc, 0x88 mtval, 0x8c the slot address,
 * 0x90 probe traps resumed; 0xa0 the caller's registers; 0xe0 / 0xd0 the caller's trap vector;
 * 0xd8 (CHERIoT) the caller's MScratchC.
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_isa_harness, "ax", @progbits
  .balign 256
  // Every synchronous trap (offset 0) and every interrupt reach the handler.
.Lvec:
  .rept 32
  j          .Lhandler
  .endr
.Lhandler:
  cspecialrw ct0, mscratchc, ct0     // ct0 = ctx, MScratchC = the slot's ct0
  csc        c1, 8(ct0)
  csc        c2, 16(ct0)
  csc        c3, 24(ct0)
  csc        c4, 32(ct0)
  csc        c6, 48(ct0)
  csc        c7, 56(ct0)
  csc        c8, 64(ct0)
  csc        c9, 72(ct0)
  csc        c10, 80(ct0)
  csc        c11, 88(ct0)
  csc        c12, 96(ct0)
  csc        c13, 104(ct0)
  csc        c14, 112(ct0)
  csc        c15, 120(ct0)
  cspecialr  c1, mscratchc
  csc        c1, 40(ct0)
  cspecialw  mscratchc, ct0          // ctx again, for the next trap
  csrr       x1, mcause
  sw         x1, 0x80(ct0)
  cspecialr  c2, mepcc
  cgetaddr   x2, c2
  sw         x2, 0x84(ct0)
  csrr       x1, mtval
  sw         x1, 0x88(ct0)
  lw         x1, 0x8c(ct0)           // the slot: a trap there is its probe's
  bne        x1, x2, .Lend
  lw         x1, 0x90(ct0)
  addi       x1, x1, 1
  sw         x1, 0x90(ct0)
  addi       x2, x2, 4               // the next word, from PCC (the executable root)
  auipcc     c1, 0
  csetaddr   c1, c1, x2
  cspecialw  mepcc, c1
  clc        c1, 8(ct0)
  clc        c2, 16(ct0)
  clc        c3, 24(ct0)
  clc        c4, 32(ct0)
  clc        c6, 48(ct0)
  clc        c7, 56(ct0)
  clc        c8, 64(ct0)
  clc        c9, 72(ct0)
  clc        c10, 80(ct0)
  clc        c11, 88(ct0)
  clc        c12, 96(ct0)
  clc        c13, 104(ct0)
  clc        c14, 112(ct0)
  clc        c15, 120(ct0)
  clc        ct0, 40(ct0)
  mret
.Lend:
  clc        cra, 0xa0(ct0)
  clc        csp, 0xa8(ct0)
  clc        cgp, 0xb0(ct0)
  clc        ctp, 0xb8(ct0)
  clc        cs0, 0xc0(ct0)
  clc        cs1, 0xc8(ct0)
  clc        ct1, 0xd0(ct0)
  cspecialw  mtcc, ct1
  clc        ct1, 0xd8(ct0)
  cspecialw  mscratchc, ct1
  cllc       ct1, .Lreturn
  cspecialw  mepcc, ct1
  mret
.Lreturn:
  cret

  .balign 4
  .globl eg2_isa_run
  .type eg2_isa_run, @function
eg2_isa_run:
  csc        cra, 0xa0(ca2)
  csc        csp, 0xa8(ca2)
  csc        cgp, 0xb0(ca2)
  csc        ctp, 0xb8(ca2)
  csc        cs0, 0xc0(ca2)
  csc        cs1, 0xc8(ca2)
  sw         a0, 0x8c(ca2)
  sw         x0, 0x90(ca2)
  cspecialr  ct0, mtcc
  csc        ct0, 0xd0(ca2)
  cllc       ct0, .Lvec
  cspecialw  mtcc, ct0
  cspecialr  ct0, mscratchc
  csc        ct0, 0xd8(ca2)
  cspecialw  mscratchc, ca2
  auipcc     ct0, 0                  // the executable root (PCC's bounds)
  csetaddr   ct0, ct0, a0
  cspecialw  mepcc, ct0
  li         t1, 0x1800              // MPP = M
  csrs       mstatus, t1
  li         t1, 0x80                // MPIE = 0
  csrc       mstatus, t1
  // Every register an untagged integer D.
  addi       x1, x11, 0
  addi       x2, x11, 0
  addi       x3, x11, 0
  addi       x4, x11, 0
  addi       x5, x11, 0
  addi       x6, x11, 0
  addi       x7, x11, 0
  addi       x8, x11, 0
  addi       x9, x11, 0
  addi       x10, x11, 0
  addi       x12, x11, 0
  addi       x13, x11, 0
  addi       x14, x11, 0
  addi       x15, x11, 0
  addi       x11, x11, 0
  mret
  .size eg2_isa_run, . - eg2_isa_run

#else  // RISC-V mode

#define EG2_ISA_CTX 0x10020400  // eg2_illegal_sweep.c kCtx

  .section .text.eg2_isa_harness, "ax", @progbits
  .balign 256
.Lvec:
  .rept 32
  j          .Lhandler
  .endr
.Lhandler:
  csrw       mscratch, t0
  li         t0, EG2_ISA_CTX
  sw         x1, 4(t0)
  sw         x2, 8(t0)
  sw         x3, 12(t0)
  sw         x4, 16(t0)
  sw         x6, 24(t0)
  sw         x7, 28(t0)
  sw         x8, 32(t0)
  sw         x9, 36(t0)
  sw         x10, 40(t0)
  sw         x11, 44(t0)
  sw         x12, 48(t0)
  sw         x13, 52(t0)
  sw         x14, 56(t0)
  sw         x15, 60(t0)
  sw         x16, 64(t0)
  sw         x17, 68(t0)
  sw         x18, 72(t0)
  sw         x19, 76(t0)
  sw         x20, 80(t0)
  sw         x21, 84(t0)
  sw         x22, 88(t0)
  sw         x23, 92(t0)
  sw         x24, 96(t0)
  sw         x25, 100(t0)
  sw         x26, 104(t0)
  sw         x27, 108(t0)
  sw         x28, 112(t0)
  sw         x29, 116(t0)
  sw         x30, 120(t0)
  sw         x31, 124(t0)
  csrr       x1, mscratch
  sw         x1, 20(t0)
  csrr       x1, mcause
  sw         x1, 0x80(t0)
  csrr       x2, mepc
  sw         x2, 0x84(t0)
  csrr       x1, mtval
  sw         x1, 0x88(t0)
  lw         x1, 0x8c(t0)            // the slot: a trap there is its probe's
  bne        x1, x2, .Lend
  lw         x1, 0x90(t0)
  addi       x1, x1, 1
  sw         x1, 0x90(t0)
  addi       x2, x2, 4
  csrw       mepc, x2
  lw         x1, 4(t0)
  lw         x2, 8(t0)
  lw         x3, 12(t0)
  lw         x4, 16(t0)
  lw         x6, 24(t0)
  lw         x7, 28(t0)
  lw         x8, 32(t0)
  lw         x9, 36(t0)
  lw         x10, 40(t0)
  lw         x11, 44(t0)
  lw         x12, 48(t0)
  lw         x13, 52(t0)
  lw         x14, 56(t0)
  lw         x15, 60(t0)
  lw         x16, 64(t0)
  lw         x17, 68(t0)
  lw         x18, 72(t0)
  lw         x19, 76(t0)
  lw         x20, 80(t0)
  lw         x21, 84(t0)
  lw         x22, 88(t0)
  lw         x23, 92(t0)
  lw         x24, 96(t0)
  lw         x25, 100(t0)
  lw         x26, 104(t0)
  lw         x27, 108(t0)
  lw         x28, 112(t0)
  lw         x29, 116(t0)
  lw         x30, 120(t0)
  lw         x31, 124(t0)
  lw         t0, 20(t0)
  mret
.Lend:
  lw         ra, 0xa0(t0)
  lw         sp, 0xa4(t0)
  lw         gp, 0xa8(t0)
  lw         tp, 0xac(t0)
  lw         s0, 0xb0(t0)
  lw         s1, 0xb4(t0)
  lw         s2, 0xb8(t0)
  lw         s3, 0xbc(t0)
  lw         s4, 0xc0(t0)
  lw         s5, 0xc4(t0)
  lw         s6, 0xc8(t0)
  lw         s7, 0xcc(t0)
  lw         s8, 0xd0(t0)
  lw         s9, 0xd4(t0)
  lw         s10, 0xd8(t0)
  lw         s11, 0xdc(t0)
  lw         t1, 0xe0(t0)
  csrw       mtvec, t1
  lw         t1, 0xe4(t0)
  csrw       mscratch, t1
  la         t1, .Lreturn
  csrw       mepc, t1
  mret
.Lreturn:
  ret

  .balign 4
  .globl eg2_isa_run
  .type eg2_isa_run, @function
eg2_isa_run:
  // a2 is the context, at EG2_ISA_CTX (the handler reaches it by that address).
  sw         ra, 0xa0(a2)
  sw         sp, 0xa4(a2)
  sw         gp, 0xa8(a2)
  sw         tp, 0xac(a2)
  sw         s0, 0xb0(a2)
  sw         s1, 0xb4(a2)
  sw         s2, 0xb8(a2)
  sw         s3, 0xbc(a2)
  sw         s4, 0xc0(a2)
  sw         s5, 0xc4(a2)
  sw         s6, 0xc8(a2)
  sw         s7, 0xcc(a2)
  sw         s8, 0xd0(a2)
  sw         s9, 0xd4(a2)
  sw         s10, 0xd8(a2)
  sw         s11, 0xdc(a2)
  sw         a0, 0x8c(a2)
  sw         x0, 0x90(a2)
  la         t1, .Lvec
  csrrw      t2, mtvec, t1
  sw         t2, 0xe0(a2)
  csrr       t2, mscratch            // the handler uses mscratch; put back at the end
  sw         t2, 0xe4(a2)
  csrw       mepc, a0
  li         t1, 0x1800              // MPP = M
  csrs       mstatus, t1
  li         t1, 0x80                // MPIE = 0
  csrc       mstatus, t1
  // Every register D.
  mv         x1, a1
  mv         x2, a1
  mv         x3, a1
  mv         x4, a1
  mv         x5, a1
  mv         x6, a1
  mv         x7, a1
  mv         x8, a1
  mv         x9, a1
  mv         x10, a1
  mv         x12, a1
  mv         x13, a1
  mv         x14, a1
  mv         x15, a1
  mv         x16, a1
  mv         x17, a1
  mv         x18, a1
  mv         x19, a1
  mv         x20, a1
  mv         x21, a1
  mv         x22, a1
  mv         x23, a1
  mv         x24, a1
  mv         x25, a1
  mv         x26, a1
  mv         x27, a1
  mv         x28, a1
  mv         x29, a1
  mv         x30, a1
  mv         x31, a1
  mret
  .size eg2_isa_run, . - eg2_isa_run

#endif  // __CHERIOT__
