// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Calls that may trap (eg2_chk.h). Each installs a trap handler of its own (MTCC in CHERIoT mode,
 * `mtvec` in RISC-V mode; the handler 256-byte aligned, as Ibex's vectored `mtvec` takes its
 * base), jumps, and puts the framework's handler back. The handler records `mcause` and `mtval`
 * and resumes at the routine's recovery point, which it reaches PC-relatively, so it depends on
 * no register the callee may have changed. Interrupts must be off. Only x0-x15 are used, as
 * CHERIoT mode has no others.
 *
 * `eg2_chk_sram_call_start` .. `_end` is position-independent and 512 bytes long: it is copied to
 * sec SRAM and runs from there (the RRAM EXEC tests switch off fetching from RRAM, where the test
 * itself runs). Its trap handler is its first instruction, its entry 64 bytes in.
 */

  .option norvc
  .option norelax

// ------------------------------------------------------------------------------------------------
// The callee every test can reach: addi a0, a0, 1 ; c.jr ra.
  .section .text.eg2_chk_stub_inc, "ax", @progbits
  .balign 4
  .globl eg2_chk_stub_inc
  .type eg2_chk_stub_inc, @function
eg2_chk_stub_inc:
  .word 0x00150513  // addi a0, a0, 1
  .word 0x00018082  // c.jr ra ; c.nop
  .size eg2_chk_stub_inc, . - eg2_chk_stub_inc

#ifdef __CHERIOT__

// ------------------------------------------------------------------------------------------------
// uint32_t eg2_chk_call(uint32_t target /*a0*/, uint32_t arg /*a1*/, eg2_chk_trap_t *out /*ca2*/)
  .section .text.eg2_chk_call, "ax", @progbits
  .balign 256
  // A vector table: every synchronous trap (offset 0) and every interrupt, the core's internal
  // interrupts included (a load integrity error is an NMI at offset 0x7c), reach the handler.
.Lcall_trap:
  .rept 32
  j          .Lcall_handler
  .endr
.Lcall_handler:
  csrr       a0, mcause
  csrr       a1, mtval
  cllc       ct0, .Lcall_recover
  cspecialw  mepcc, ct0
  mret

  .balign 4
  .globl eg2_chk_call
  .type eg2_chk_call, @function
eg2_chk_call:
  cincoffsetimm csp, csp, -32
  csc        cra, 0(csp)
  csc        cs0, 8(csp)
  csc        cs1, 16(csp)
  cmove      cs1, ca2               // out
  cspecialr  cs0, mtcc              // the framework's handler
  cllc       ct0, .Lcall_trap
  cspecialw  mtcc, ct0
  auipcc     ct1, 0                 // the executable root (PCC's bounds)
  csetaddr   ct1, ct1, a0
  mv         a0, a1
  cjalr      cra, ct1
  // Returned: a0 is the callee's result.
  sw         a0, 4(cs1)
  sw         x0, 8(cs1)
  addi       a0, x0, 0
  j          .Lcall_done
.Lcall_recover:
  // Trapped: a0 = mcause, a1 = mtval.
  sw         x0, 4(cs1)
  sw         a1, 8(cs1)
.Lcall_done:
  cspecialw  mtcc, cs0
  sw         a0, 0(cs1)
  clc        cra, 0(csp)
  clc        cs0, 8(csp)
  clc        cs1, 16(csp)
  cincoffsetimm csp, csp, 32
  cret
  .size eg2_chk_call, . - eg2_chk_call

// ------------------------------------------------------------------------------------------------
// uint32_t eg2_chk_sram_call_asm(volatile uint32_t *reg /*ca0*/, uint32_t value /*a1*/,
//                                uint32_t target /*a2*/, uint32_t restore /*a3*/,
//                                eg2_chk_trap_t *out /*ca4*/, uint32_t entry /*a5*/)
// Runs the SRAM copy at `entry` (an executable capability from PCC).
  .section .text.eg2_chk_sram_call_asm, "ax", @progbits
  .balign 4
  .globl eg2_chk_sram_call_asm
  .type eg2_chk_sram_call_asm, @function
eg2_chk_sram_call_asm:
  cincoffsetimm csp, csp, -16
  csc        cra, 0(csp)
  auipcc     ct0, 0
  csetaddr   ct0, ct0, a5
  cjalr      cra, ct0
  clc        cra, 0(csp)
  cincoffsetimm csp, csp, 16
  cret
  .size eg2_chk_sram_call_asm, . - eg2_chk_sram_call_asm

// ------------------------------------------------------------------------------------------------
// The SRAM copy. Entry (start + 64): ca0 the register, a1 its value, a2 the target, a3 the value
// to restore, ca4 the record. Returns mcause in a0.
  .section .rodata.eg2_chk_sram_call, "a", @progbits
  .balign 512
  .globl eg2_chk_sram_call_start
  .type eg2_chk_sram_call_start, @object
eg2_chk_sram_call_start:
.Lsc_start:
  // +0: the trap handler.
  csrr       t0, mcause
  csrr       t1, mtval
  cllc       ct2, .Lsc_recover        // PC-relative: right in the copy too
  cspecialw  mepcc, ct2
  mret
  .balign 64
  // +64: the entry. Frame: cra 0, cs0 8, cs1 16, the register 24, the restore value 32.
.Lsc_entry:
  cincoffsetimm csp, csp, -48
  csc        cra, 0(csp)
  csc        cs0, 8(csp)
  csc        cs1, 16(csp)
  csc        ca0, 24(csp)
  sw         a3, 32(csp)
  cmove      cs1, ca4
  sw         x0, 4(cs1)
  cspecialr  cs0, mtcc
  cllc       ct0, .Lsc_start
  cspecialw  mtcc, ct0
  sw         a1, 0(ca0)             // the register's new value
  lw         t0, 0(ca0)             // read back: the write has landed
  fence.i
  auipcc     ct2, 0
  csetaddr   ct2, ct2, a2
  addi       a0, x0, 0
  cjalr      cra, ct2
  // Returned: a0 the callee's result.
  sw         a0, 4(cs1)
  addi       t0, x0, 0
  addi       t1, x0, 0
.Lsc_recover:
  sw         t0, 0(cs1)
  sw         t1, 8(cs1)
  clc        ca0, 24(csp)
  lw         a3, 32(csp)
  sw         a3, 0(ca0)             // restore
  lw         t2, 0(ca0)
  fence.i
  cspecialw  mtcc, cs0
  mv         a0, t0
  clc        cra, 0(csp)
  clc        cs0, 8(csp)
  clc        cs1, 16(csp)
  cincoffsetimm csp, csp, 48
  cret
  .balign 512
eg2_chk_sram_call_end_:
  .size eg2_chk_sram_call_start, eg2_chk_sram_call_end_ - eg2_chk_sram_call_start


// ------------------------------------------------------------------------------------------------
// uint32_t eg2_chk_access(volatile void *addr /*ca0*/, uint32_t op /*a1*/, uint32_t wdata /*a2*/,
//                         eg2_chk_trap_t *out /*ca3*/)
  .section .text.eg2_chk_access, "ax", @progbits
  .balign 256
  // A vector table: every synchronous trap (offset 0) and every interrupt, the core's internal
  // interrupts included (a load integrity error is an NMI at offset 0x7c), reach the handler.
.Lacc_trap:
  .rept 32
  j          .Lacc_handler
  .endr
.Lacc_handler:
  csrr       a5, mcause
  csrr       t1, mtval
  cllc       ct2, .Lacc_recover
  cspecialw  mepcc, ct2
  mret

  .balign 4
  .globl eg2_chk_access
  .type eg2_chk_access, @function
eg2_chk_access:
  cspecialr  ct0, mtcc
  cllc       ct1, .Lacc_trap
  cspecialw  mtcc, ct1
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 0
  beqz       a1, .Lacc_lb
  addi       a1, a1, -1
  beqz       a1, .Lacc_lh
  addi       a1, a1, -1
  beqz       a1, .Lacc_lw
  addi       a1, a1, -1
  beqz       a1, .Lacc_lbu
  addi       a1, a1, -1
  beqz       a1, .Lacc_lhu
  addi       a1, a1, -1
  beqz       a1, .Lacc_sb
  addi       a1, a1, -1
  beqz       a1, .Lacc_sh
  sw         a2, 0(ca0)
  j          .Lacc_recover
.Lacc_lb:
  lb         a4, 0(ca0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lh:
  lh         a4, 0(ca0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lw:
  lw         a4, 0(ca0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lbu:
  lbu        a4, 0(ca0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lhu:
  lhu        a4, 0(ca0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_sb:
  sb         a2, 0(ca0)
  j          .Lacc_recover
.Lacc_sh:
  sh         a2, 0(ca0)
.Lacc_recover:
  cspecialw  mtcc, ct0
  sw         a5, 0(ca3)
  sw         a4, 4(ca3)
  sw         t1, 8(ca3)
  mv         a0, a5
  cret
  .size eg2_chk_access, . - eg2_chk_access

#else  // RISC-V mode

// ------------------------------------------------------------------------------------------------
// uint32_t eg2_chk_call(uint32_t target /*a0*/, uint32_t arg /*a1*/, eg2_chk_trap_t *out /*a2*/)
  .section .text.eg2_chk_call, "ax", @progbits
  .balign 256
  // A vector table: every synchronous trap (offset 0) and every interrupt, the core's internal
  // interrupts included (a load integrity error is an NMI at offset 0x7c), reach the handler.
.Lcall_trap:
  .rept 32
  j          .Lcall_handler
  .endr
.Lcall_handler:
  csrr       a0, mcause
  csrr       a1, mtval
  la         t0, .Lcall_recover
  csrw       mepc, t0
  mret

  .balign 4
  .globl eg2_chk_call
  .type eg2_chk_call, @function
eg2_chk_call:
  addi       sp, sp, -16
  sw         ra, 0(sp)
  sw         s0, 4(sp)
  sw         s1, 8(sp)
  mv         s1, a2                 // out
  la         t0, .Lcall_trap
  csrrw      s0, mtvec, t0          // the framework's handler
  mv         t1, a0
  mv         a0, a1
  jalr       ra, 0(t1)
  sw         a0, 4(s1)
  sw         x0, 8(s1)
  addi       a0, x0, 0
  j          .Lcall_done
.Lcall_recover:
  sw         x0, 4(s1)
  sw         a1, 8(s1)
.Lcall_done:
  csrw       mtvec, s0
  sw         a0, 0(s1)
  lw         ra, 0(sp)
  lw         s0, 4(sp)
  lw         s1, 8(sp)
  addi       sp, sp, 16
  ret
  .size eg2_chk_call, . - eg2_chk_call

// ------------------------------------------------------------------------------------------------
  .section .text.eg2_chk_sram_call_asm, "ax", @progbits
  .balign 4
  .globl eg2_chk_sram_call_asm
  .type eg2_chk_sram_call_asm, @function
eg2_chk_sram_call_asm:
  addi       sp, sp, -16
  sw         ra, 0(sp)
  jalr       ra, 0(a5)
  lw         ra, 0(sp)
  addi       sp, sp, 16
  ret
  .size eg2_chk_sram_call_asm, . - eg2_chk_sram_call_asm

  .section .rodata.eg2_chk_sram_call, "a", @progbits
  .balign 512
  .globl eg2_chk_sram_call_start
  .type eg2_chk_sram_call_start, @object
eg2_chk_sram_call_start:
.Lsc_start:
  // +0: the trap handler.
  csrr       t0, mcause
  csrr       t1, mtval
  la         t2, .Lsc_recover         // PC-relative: right in the copy too
  csrw       mepc, t2
  mret
  .balign 64
  // +64: the entry. Frame: ra 0, s0 4, s1 8, the register 12, the restore value 16.
.Lsc_entry:
  addi       sp, sp, -32
  sw         ra, 0(sp)
  sw         s0, 4(sp)
  sw         s1, 8(sp)
  sw         a0, 12(sp)
  sw         a3, 16(sp)
  mv         s1, a4
  sw         x0, 4(s1)
  la         t0, .Lsc_start
  csrrw      s0, mtvec, t0
  sw         a1, 0(a0)
  lw         t0, 0(a0)
  fence.i
  mv         t2, a2
  addi       a0, x0, 0
  jalr       ra, 0(t2)
  sw         a0, 4(s1)
  addi       t0, x0, 0
  addi       t1, x0, 0
.Lsc_recover:
  sw         t0, 0(s1)
  sw         t1, 8(s1)
  lw         a0, 12(sp)
  lw         a3, 16(sp)
  sw         a3, 0(a0)
  lw         t2, 0(a0)
  fence.i
  csrw       mtvec, s0
  mv         a0, t0
  lw         ra, 0(sp)
  lw         s0, 4(sp)
  lw         s1, 8(sp)
  addi       sp, sp, 32
  ret
  .balign 512
eg2_chk_sram_call_end_:
  .size eg2_chk_sram_call_start, eg2_chk_sram_call_end_ - eg2_chk_sram_call_start


// ------------------------------------------------------------------------------------------------
// uint32_t eg2_chk_access(volatile void *addr /*ca0*/, uint32_t op /*a1*/, uint32_t wdata /*a2*/,
//                         eg2_chk_trap_t *out /*ca3*/)
  .section .text.eg2_chk_access, "ax", @progbits
  .balign 256
  // A vector table: every synchronous trap (offset 0) and every interrupt, the core's internal
  // interrupts included (a load integrity error is an NMI at offset 0x7c), reach the handler.
.Lacc_trap:
  .rept 32
  j          .Lacc_handler
  .endr
.Lacc_handler:
  csrr       a5, mcause
  csrr       t1, mtval
  la         t2, .Lacc_recover
  csrw       mepc, t2
  mret

  .balign 4
  .globl eg2_chk_access
  .type eg2_chk_access, @function
eg2_chk_access:
  la         t1, .Lacc_trap
  csrrw      t0, mtvec, t1
  addi       a5, x0, 0
  addi       a4, x0, 0
  addi       t1, x0, 0
  beqz       a1, .Lacc_lb
  addi       a1, a1, -1
  beqz       a1, .Lacc_lh
  addi       a1, a1, -1
  beqz       a1, .Lacc_lw
  addi       a1, a1, -1
  beqz       a1, .Lacc_lbu
  addi       a1, a1, -1
  beqz       a1, .Lacc_lhu
  addi       a1, a1, -1
  beqz       a1, .Lacc_sb
  addi       a1, a1, -1
  beqz       a1, .Lacc_sh
  sw         a2, 0(a0)
  j          .Lacc_recover
.Lacc_lb:
  lb         a4, 0(a0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lh:
  lh         a4, 0(a0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lw:
  lw         a4, 0(a0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lbu:
  lbu        a4, 0(a0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_lhu:
  lhu        a4, 0(a0)
  nop                               // an internal interrupt (load integrity)
  nop                               // comes at most one instruction later
  j          .Lacc_recover
.Lacc_sb:
  sb         a2, 0(a0)
  j          .Lacc_recover
.Lacc_sh:
  sh         a2, 0(a0)
.Lacc_recover:
  csrw       mtvec, t0
  sw         a5, 0(a3)
  sw         a4, 4(a3)
  sw         t1, 8(a3)
  mv         a0, a5
  ret
  .size eg2_chk_access, . - eg2_chk_access

#endif  // __CHERIOT__
