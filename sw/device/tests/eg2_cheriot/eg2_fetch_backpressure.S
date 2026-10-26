// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_fetch_backpressure's kernel (eg2_fetch_backpressure.c). CHERIoT mode only; only
 * x0-x15 are used.
 *
 * void eg2_fb_kernel(void *slots (ca0), uint32_t *out (ca1), uint32_t iters (a2),
 *                    void *dst (ca3), uint32_t *mark (ca4), uint32_t mask (a5))
 *
 * Position-independent (PC-relative branches and jumps, AUIPCC): the test runs it in place (RRAM)
 * or copies it to main or sec SRAM. On entry it writes `mask` to the bitmap word `mark`, and 0 on
 * exit, so the marks are set only while it runs and no call is made meanwhile. Each iteration
 * loads three slots (slots advance by 24 bytes per iteration):
 *   1. CLC, and a taken branch as the very next instruction;
 *   2. CLC, and a jump (CJAL to c0) as the very next instruction;
 *   3. CLC, and a CJALR through a capability computed before it as the very next instruction;
 *   4. CSC of the third loaded register to `dst`, then CLC of `dst` straight after;
 * and writes {tag4, tag3, tag2, tag1} as bits 3:0 of out[i]. Compressed and 32-bit instructions are
 * mixed, so 32-bit ones straddle fetch-word boundaries (`.option rvc`).
 */

  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_fb_kernel, "ax", @progbits
  .balign 64
  .globl eg2_fb_kernel
  .type eg2_fb_kernel, @function
eg2_fb_kernel:
  .option norvc
  cincoffset csp, csp, -16
  csc        cs0, 0(csp)
  csc        ca4, 8(csp)
  sw         a5, 0(ca4)
  .option rvc
.Lfb_loop:
  li         t0, 0
  // 1: CLC, then a taken branch.
  clc        ca4, 0(ca0)
  bnez       a2, .Lfb_1
  c.nop
.Lfb_1:
  cgettag    s0, ca4
  or         t0, t0, s0
  c.nop
  // 2: CLC, then a jump.
  clc        ca4, 8(ca0)
  j          .Lfb_2
  c.nop
  c.nop
.Lfb_2:
  c.nop
  cgettag    s0, ca4
  slli       s0, s0, 1
  or         t0, t0, s0
  // 3: the target first, then CLC and CJALR (four 32-bit instructions and two c.nop: +20).
  .option norvc
.Lfb_pc3:
  auipcc     ca5, 0
  cincoffset ca5, ca5, 20
  clc        ca4, 16(ca0)
  cjalr      c0, 0(ca5)
  .option rvc
  c.nop
  c.nop
.Lfb_3:
  .if .Lfb_3 - .Lfb_pc3 - 20
  .error "eg2_fb_kernel: the CJALR target is not at +20"
  .endif
  cgettag    s0, ca4
  slli       s0, s0, 2
  or         t0, t0, s0
  // 4: CSC, then CLC of the same address.
  csc        ca4, 0(ca3)
  clc        ca4, 0(ca3)
  c.nop
  cgettag    s0, ca4
  slli       s0, s0, 3
  or         t0, t0, s0
  li         a4, 0
  sw         t0, 0(ca1)
  cincoffset ca1, ca1, 4
  cincoffset ca0, ca0, 24
  addi       a2, a2, -1
  bnez       a2, .Lfb_loop
  .option norvc
  clc        ca4, 8(csp)
  sw         zero, 0(ca4)
  li         a4, 0
  li         a5, 0
  clc        cs0, 0(csp)
  cincoffset csp, csp, 16
  cret
  .globl eg2_fb_kernel_end
eg2_fb_kernel_end:
  .size eg2_fb_kernel, . - eg2_fb_kernel

#endif  // __CHERIOT__
