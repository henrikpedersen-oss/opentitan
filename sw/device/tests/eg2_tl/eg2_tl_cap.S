// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * A capability load or store that may fault (eg2_tl_errors, CHERIoT mode only), under a trap
 * handler of its own (MTCC, 256-byte aligned like eg2_chk_call.S's handlers). Interrupts must be
 * off. Only x0-x15 are used.
 *
 * uint32_t eg2_tl_cap_access(void *auth (ca0), uint32_t op (a1: 0 CLC, 1 CSC),
 *                            void *wcap (ca2), eg2_tl_cap_out_t *out (ca3))
 * out: +0 the loaded capability (8 bytes), +8 mcause, +12 mtval. Returns mcause (0: no trap).
 */

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_tl_cap_access, "ax", @progbits
  .balign 256
.Ltc_trap:
  csrr       a5, mcause
  csrr       t1, mtval
  cllc       ct2, .Ltc_recover
  cspecialw  mepcc, ct2
  mret

  .balign 4
  .globl eg2_tl_cap_access
  .type eg2_tl_cap_access, @function
eg2_tl_cap_access:
  cspecialr  ct0, mtcc
  cllc       ct1, .Ltc_trap
  cspecialw  mtcc, ct1
  addi       a5, x0, 0
  addi       t1, x0, 0
  cmove      ca4, cnull
  bnez       a1, .Ltc_csc
  clc        ca4, 0(ca0)
  j          .Ltc_recover
.Ltc_csc:
  csc        ca2, 0(ca0)
.Ltc_recover:
  cspecialw  mtcc, ct0
  csc        ca4, 0(ca3)
  sw         a5, 8(ca3)
  sw         t1, 12(ca3)
  mv         a0, a5
  cret
  .size eg2_tl_cap_access, . - eg2_tl_cap_access

#endif  // __CHERIOT__
