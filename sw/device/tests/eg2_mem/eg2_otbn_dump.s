/* Copyright lowRISC contributors (OpenTitan project). */
/* Licensed under the Apache License, Version 2.0, see LICENSE for details. */
/* SPDX-License-Identifier: Apache-2.0 */

/*
 * chip_sw_otbn_isa's second program (eg2_otbn_isa.c): it only dumps the register files, as the
 * OTBN smoke test's reg_dump does (hw/ip/otbn/dv/smoke/smoke_test.s), into gpr_state and
 * wdr_state, so the test can check OTBN's secure wipe after the smoke run left nothing of its
 * register values (chip_otbn_testplan chip_sw_otbn_isa).
 */

.section .text.start
  la x1, gpr_state
  sw x2, 0(x1)
  la x2, gpr_state
  sw  x3,   4(x2)
  sw  x4,   8(x2)
  sw  x5,  12(x2)
  sw  x6,  16(x2)
  sw  x7,  20(x2)
  sw  x8,  24(x2)
  sw  x9,  28(x2)
  sw x10,  32(x2)
  sw x11,  36(x2)
  sw x12,  40(x2)
  sw x13,  44(x2)
  sw x14,  48(x2)
  sw x15,  52(x2)
  sw x16,  56(x2)
  sw x17,  60(x2)
  sw x18,  64(x2)
  sw x19,  68(x2)
  sw x20,  72(x2)
  sw x21,  76(x2)
  sw x22,  80(x2)
  sw x23,  84(x2)
  sw x24,  88(x2)
  sw x25,  92(x2)
  sw x26,  96(x2)
  sw x27, 100(x2)
  sw x28, 104(x2)
  sw x29, 108(x2)
  sw x30, 112(x2)
  sw x31, 116(x2)
  li x2, 0
  la x3, wdr_state
  li x1, 32
  loop x1, 2
  bn.sid x2++, 0(x3)
  addi x3, x3, 32
  ecall

.section .data
.global gpr_state
.balign 32
gpr_state:
  .zero (32 / 4) * 30

.global wdr_state
.balign 32
wdr_state:
  .zero (256 / 4) * 32
