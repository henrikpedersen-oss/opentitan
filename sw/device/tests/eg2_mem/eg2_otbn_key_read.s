/* Copyright lowRISC contributors (OpenTitan project). */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * chip_sw_otbn_keymgr's OTBN program (eg2_otbn_keymgr.c): reads the key keymgr_dpe sideloaded,
 * the WSRs KEY_S0_L (0x4), KEY_S0_H (0x5), KEY_S1_L (0x6) and KEY_S1_H (0x7) (OTBN
 * isa.md "Wide Special Purpose Registers"), into key_out, one 256-bit word each.
 */

.section .text.start
  bn.wsrr w0, 0x4
  bn.wsrr w1, 0x5
  bn.wsrr w2, 0x6
  bn.wsrr w3, 0x7
  li x2, 0
  la x3, key_out
  bn.sid x2++, 0(x3)
  bn.sid x2++, 32(x3)
  bn.sid x2++, 64(x3)
  bn.sid x2++, 96(x3)
  ecall

.section .data
.global key_out
.balign 32
key_out:
  .zero 128
