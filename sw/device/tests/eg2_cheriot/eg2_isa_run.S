// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_isa's trampoline (eg2_rand_isa.c): runs one generated block.
 *
 *   uint32_t eg2_isa_run(entry, operands)
 *
 * `entry` is the block (an executable capability in CHERIoT mode, an address in RISC-V mode);
 * `operands` points at 11 slots, the values of x5-x15 when the block starts (8-byte slots,
 * capabilities in CHERIoT mode, loaded with CLC so a slot's tag reaches the register). The block
 * may write x5-x15 only and returns with `jalr x0, 0(ra)` (`cjalr c0, cra`). s0, s1 and tp are
 * saved around it; the block's entry is held in tp. Returns the block's final a0.
 */

  .text
  .balign 4
  .globl eg2_isa_run
  .type eg2_isa_run, @function
eg2_isa_run:
#ifdef __CHERIOT__
  cincoffsetimm csp, csp, -32
  csc        cs0, 0(csp)
  csc        cs1, 8(csp)
  csc        ctp, 16(csp)
  csc        cra, 24(csp)
  cmove      ctp, ca0
  clc        ct0, 0(ca1)
  clc        ct1, 8(ca1)
  clc        ct2, 16(ca1)
  clc        cs0, 24(ca1)
  clc        cs1, 32(ca1)
  clc        ca0, 40(ca1)
  clc        ca2, 56(ca1)
  clc        ca3, 64(ca1)
  clc        ca4, 72(ca1)
  clc        ca5, 80(ca1)
  clc        ca1, 48(ca1)
  cjalr      cra, 0(ctp)
  clc        cra, 24(csp)
  clc        ctp, 16(csp)
  clc        cs1, 8(csp)
  clc        cs0, 0(csp)
  cincoffsetimm csp, csp, 32
  cret
#else
  addi       sp, sp, -16
  sw         s0, 0(sp)
  sw         s1, 4(sp)
  sw         tp, 8(sp)
  sw         ra, 12(sp)
  mv         tp, a0
  lw         t0, 0(a1)
  lw         t1, 8(a1)
  lw         t2, 16(a1)
  lw         s0, 24(a1)
  lw         s1, 32(a1)
  lw         a0, 40(a1)
  lw         a2, 56(a1)
  lw         a3, 64(a1)
  lw         a4, 72(a1)
  lw         a5, 80(a1)
  lw         a1, 48(a1)
  jalr       ra, 0(tp)
  lw         ra, 12(sp)
  lw         tp, 8(sp)
  lw         s1, 4(sp)
  lw         s0, 0(sp)
  addi       sp, sp, 16
  ret
#endif
  .size eg2_isa_run, . - eg2_isa_run
