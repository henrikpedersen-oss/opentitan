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
  beqz a0, 1f
  li t0, 0x20000
  li t1, 'F'
  sw t1, 0(t0)
  li t1, 'A'
  sw t1, 0(t0)
  li t1, 'I'
  sw t1, 0(t0)
  li t1, 'L'
  sw t1, 0(t0)
  j 2f
1:
  li t0, 0x20000
  li t1, 'P'
  sw t1, 0(t0)
  li t1, 'A'
  sw t1, 0(t0)
  li t1, 'S'
  sw t1, 0(t0)
  sw t1, 0(t0)
2:
  li t1, 10
  sw t1, 0(t0)
  li t1, 1
  sw t1, 8(t0)
3:
  j 3b
