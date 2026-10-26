// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_progress's loop (eg2_tbre_progress.c). CHERIoT mode only.
 *
 * uint32_t eg2_tp_rmw(uint32_t *buf (ca0), uint32_t n (a1), uint32_t rounds (a2))
 *
 * Position-independent, so the test can run it from SRAM: `rounds` times, a word read-modify-write
 * (lw, addi 1, sw) of each of the n words of buf. Returns the mcycle count it took.
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_tp_rmw, "ax", @progbits
  .balign 64
  .globl eg2_tp_rmw
  .type eg2_tp_rmw, @function
eg2_tp_rmw:
  csrr       a5, mcycle
  beqz       a2, .Ltp_done
.Ltp_round:
  cmove      ca3, ca0
  mv         a4, a1
.Ltp_loop:
  lw         t0, 0(ca3)
  addi       t0, t0, 1
  sw         t0, 0(ca3)
  cincoffset ca3, ca3, 4
  addi       a4, a4, -1
  bnez       a4, .Ltp_loop
  addi       a2, a2, -1
  bnez       a2, .Ltp_round
.Ltp_done:
  csrr       a0, mcycle
  sub        a0, a0, a5
  li         a3, 0
  cret
  .globl eg2_tp_rmw_end
eg2_tp_rmw_end:
  .size eg2_tp_rmw, . - eg2_tp_rmw

#endif  // __CHERIOT__
