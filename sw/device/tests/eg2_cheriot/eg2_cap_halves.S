// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_cheriot_cap_halves's copy kernel (eg2_cap_halves.c). CHERIoT mode only.
 *
 * void eg2_ch_copy(void *slots (ca0), void *copies (ca1), uint32_t n (a2), uint32_t rounds (a3))
 *
 * Position-independent, so the test can run it from SRAM: `rounds` times, CLC of each of the n
 * slots and CSC of the loaded register to the copy at the same index, back to back.
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_ch_copy, "ax", @progbits
  .balign 64
  .globl eg2_ch_copy
  .type eg2_ch_copy, @function
eg2_ch_copy:
  beqz       a3, .Lch_done
.Lch_round:
  cmove      ca4, ca0
  cmove      ca5, ca1
  mv         t1, a2
.Lch_loop:
  clc        ct0, 0(ca4)
  csc        ct0, 0(ca5)
  cincoffset ca4, ca4, 8
  cincoffset ca5, ca5, 8
  addi       t1, t1, -1
  bnez       t1, .Lch_loop
  addi       a3, a3, -1
  bnez       a3, .Lch_round
.Lch_done:
  li         t0, 0
  li         a4, 0
  li         a5, 0
  cret
  .globl eg2_ch_copy_end
eg2_ch_copy_end:
  .size eg2_ch_copy, . - eg2_ch_copy

#endif  // __CHERIOT__
