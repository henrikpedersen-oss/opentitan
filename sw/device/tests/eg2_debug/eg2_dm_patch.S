// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_eg2_rv_dm_code_patch's call into its code slot in main SRAM (eg2_rv_dm_code_patch.c):
 *
 *   uint32_t eg2_dm_patch_call(uint32_t slot, uint32_t x);
 *
 * Calls the code at bus address `slot` with a0 = x and returns its a0. The slot's code ends with
 * `ret` (jalr x0, 0(ra); cjalr cnull, cra in CHERIoT mode). In CHERIoT mode the jump goes through
 * PCC (the executable root of the test image) with its address set to `slot`, as eg2_isa_harness.S
 * does. Only ra / cra, t0 / ct0 and a0 are used.
 */

  .option norvc
  .option norelax

  .section .text.eg2_dm_patch_call, "ax", @progbits
  .balign 4
  .globl eg2_dm_patch_call
  .type eg2_dm_patch_call, @function
eg2_dm_patch_call:
#ifdef __CHERIOT__
  cincoffset csp, csp, -16
  csc        cra, 0(csp)
  auipcc     ct0, 0
  csetaddr   ct0, ct0, a0
  mv         a0, a1
  cjalr      cra, ct0
  clc        cra, 0(csp)
  cincoffset csp, csp, 16
  cret
#else
  addi       sp, sp, -16
  sw         ra, 12(sp)
  mv         t0, a0
  mv         a0, a1
  jalr       ra, 0(t0)
  lw         ra, 12(sp)
  addi       sp, sp, 16
  ret
#endif
  .size eg2_dm_patch_call, .-eg2_dm_patch_call
