// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_execution_main_cheriot's call (eg2_sram_execution_main.c), CHERIoT mode only.
 *
 * uint32_t eg2_xm_call(uint32_t base (a0), uint32_t len (a1), uint32_t entry (a2),
 *                      uint32_t untag (a3), uint32_t arg (a4), eg2_xm_trap_t *out (ca5))
 *
 * Derives an executable capability from PCC (the executable root), sets its bounds exactly to
 * [base, base + len), its address to `entry`, clears its tag if `untag` is 1, records the tag,
 * base and length it ended up with, and jumps to it (`cjalr cra, ct1`) with a0 = `arg` under a
 * trap handler of its own, as eg2_chk_call does. Records mcause, the returned a0, mtval, mepcc's
 * address and the address of the `cjalr` (the call site). Interrupts must be off.
 *
 * Record (eg2_xm_trap_t): mcause 0, value 4, mtval 8, mepc 12, site 16, tag 20, base 24, len 28.
 */

#ifndef __CHERIOT__
#error "eg2_sram_exec_call.S is built in CHERIoT mode only"
#endif

  .option norvc
  .option norelax

  .section .text.eg2_xm_call, "ax", @progbits
  .balign 256
  // Every synchronous trap (offset 0) and every interrupt reach the handler.
.Lxm_trap:
  .rept 32
  j          .Lxm_handler
  .endr
.Lxm_handler:
  csrr       a0, mcause
  csrr       a1, mtval
  cspecialr  ct2, mepcc
  addi       a2, t2, 0              // mepcc's address
  cllc       ct0, .Lxm_recover
  cspecialw  mepcc, ct0
  mret

  .balign 4
  .globl eg2_xm_call
  .type eg2_xm_call, @function
eg2_xm_call:
  cincoffsetimm csp, csp, -32
  csc        cra, 0(csp)
  csc        cs0, 8(csp)
  csc        cs1, 16(csp)
  cmove      cs1, ca5               // out
  auipcc     ct1, 0                 // the executable root (PCC's bounds)
  csetaddr   ct1, ct1, a0
  csetboundsexact ct1, ct1, a1
  csetaddr   ct1, ct1, a2
  andi       t0, a3, 1
  beqz       t0, 1f
  ccleartag  ct1, ct1
1:
  cgettag    t0, ct1
  sw         t0, 20(cs1)
  cgetbase   t0, ct1
  sw         t0, 24(cs1)
  cgetlen    t0, ct1
  sw         t0, 28(cs1)
  cllc       ct0, .Lxm_jump
  sw         t0, 16(cs1)            // the call site's address
  cspecialr  cs0, mtcc              // the framework's handler
  cllc       ct0, .Lxm_trap
  cspecialw  mtcc, ct0
  addi       a0, a4, 0
.Lxm_jump:
  cjalr      cra, ct1
  // Returned: a0 is the callee's result.
  sw         a0, 4(cs1)
  sw         x0, 8(cs1)
  sw         x0, 12(cs1)
  addi       a0, x0, 0
  j          .Lxm_done
.Lxm_recover:
  // Trapped: a0 = mcause, a1 = mtval, a2 = mepcc's address.
  sw         x0, 4(cs1)
  sw         a1, 8(cs1)
  sw         a2, 12(cs1)
.Lxm_done:
  cspecialw  mtcc, cs0
  sw         a0, 0(cs1)
  clc        cra, 0(csp)
  clc        cs0, 8(csp)
  clc        cs1, 16(csp)
  cincoffsetimm csp, csp, 32
  cret
  .size eg2_xm_call, . - eg2_xm_call
