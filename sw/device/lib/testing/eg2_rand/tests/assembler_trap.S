// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

  .section .text, "ax", @progbits
  .align 2
  .globl eg2_rand_illegal_instruction
eg2_rand_illegal_instruction:
  .word 0xffffffff
  ret

  .align 8
  .globl eg2_rand_trap_entry
eg2_rand_trap_entry:
  csrr t0, mcause
  csrr t1, mtval
  csrr t2, mepc
  csrr t3, mstatus
  la t4, eg2_rand_trap_observation
  li t5, 1
  sw t5, 0(t4)
  sw zero, 4(t4)
  sw t0, 8(t4)
  sw t1, 12(t4)
  sw t2, 16(t4)
  sw t3, 20(t4)
  addi t2, t2, 4
  csrw mepc, t2
  mret

  .section .bss, "aw", @nobits
  .align 2
  .globl eg2_rand_trap_observation
eg2_rand_trap_observation:
  .zero 24
