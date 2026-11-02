// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

  .section .text.init, "ax", @progbits
  .org 0x80
  .globl _start
_start:
  la sp, _stack_top
  .option push
  .option norelax
  la gp, __global_pointer$
  .option pop
  la t0, eg2_rand_trap_entry
  csrw mtvec, t0
  call main
  li t0, 0x411f0080
  bnez a0, 1f
  li t1, 0x900d
  sw t1, 0(t0)
  j 2f
1:
  li t1, 0xbaad
  sw t1, 0(t0)
2:
  j 2b
