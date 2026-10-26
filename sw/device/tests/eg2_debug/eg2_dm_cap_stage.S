// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The stages of chip_eg2_rv_dm_cap_bypass, chip_eg2_rv_dm_halt_revocation and chip_eg2_rv_dm_priv
 * (eg2_dm_cap.h states the protocol and the registers). Each is called by eg2_probe_call with
 * ca0 = the context and ca1 = op1, and touches only ca1, ca2, ca4, ct0-ct2 (never ca3 or ca5,
 * eg2_probe_call's). Only x0-x15 are used.
 */

#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"

  .option norvc
  .option norelax

  .section .text.eg2_dmx, "ax", @progbits

#ifdef __CHERIOT__

// Publish the address of `label` in the flag word. Clobbers ct0, ct1.
.macro FLAG label
  clc        ct0, EG2_DMX_FLAG(ca0)
  cllc       ct1, \label
  cgetaddr   t1, ct1
  sw         t1, 0(ct0)
.endm

// Spin at `label` until the stop word is set. Clobbers ct0.
.macro WAIT_STOP label
\label:
  clc        ct0, EG2_DMX_STOP(ca0)
  lw         t0, 0(ct0)
  beqz       t0, \label
.endm

// void eg2_dmx_park(ctx): ca4 = the landing capability (from PCC: tagged, unsealed, EX), its
// address in ctx->landing; wait for stop; return.
  .balign 4
  .globl eg2_dmx_park
  .type eg2_dmx_park, @function
eg2_dmx_park:
  cllc       ca4, eg2_dmx_landing
  cgetaddr   t0, ca4
  sw         t0, EG2_DMX_LANDING(ca0)
  FLAG       .Lpark_wait
  WAIT_STOP  .Lpark_wait
  cret
  .size eg2_dmx_park, . - eg2_dmx_park

// void eg2_dmx_park_ecall(ctx): as eg2_dmx_park, then ecall (recorded by the probe; never
// returns).
  .balign 4
  .globl eg2_dmx_park_ecall
  .type eg2_dmx_park_ecall, @function
eg2_dmx_park_ecall:
  cllc       ca4, eg2_dmx_landing
  cgetaddr   t0, ca4
  sw         t0, EG2_DMX_LANDING(ca0)
  FLAG       .Lpecall_wait
  WAIT_STOP  .Lpecall_wait
  ecall
  cret
  .size eg2_dmx_park_ecall, . - eg2_dmx_park_ecall

// The target of a bad DEPCC: its fetch must fault. If it runs, the ecall is recorded instead.
  .balign 8
  .globl eg2_dmx_landing
  .type eg2_dmx_landing, @function
eg2_dmx_landing:
  ecall
  cret
  .size eg2_dmx_landing, . - eg2_dmx_landing

// void eg2_dmx_step_clc(ctx, t1_init): the bench single-steps from the wait (dpc = .Lstep_wait).
// Step 1: the beqz falls through once the bench wrote t1; steps 2-4: the three CLCs.
  .balign 4
  .globl eg2_dmx_step_clc
  .type eg2_dmx_step_clc, @function
eg2_dmx_step_clc:
  addi       t2, a1, 0
  clc        ca1, EG2_DMX_AUTH_MARK(ca0)
  clc        ca4, EG2_DMX_AUTH_CLEAN(ca0)
  FLAG       .Lstep_wait
  addi       t1, t2, 0
.Lstep_wait:
  beqz       t1, .Lstep_wait
  clc        ca2, 0(ca1)
  clc        ct0, 0(ca4)
  clc        ct2, 0(ca1)
  csc        ca2, (EG2_DMX_OUT+0)(ca0)
  csc        ct0, (EG2_DMX_OUT+8)(ca0)
  csc        ct2, (EG2_DMX_OUT+16)(ca0)
  cret
  .size eg2_dmx_step_clc, . - eg2_dmx_step_clc

// void eg2_dmx_clc_loop(ctx): CLC of the marked and the clean capability until stop (at least
// once). ctx->n = iterations, ctx->errs = CLCs whose tag disagreed with the bitmap.
  .balign 4
  .globl eg2_dmx_clc_loop
  .type eg2_dmx_clc_loop, @function
eg2_dmx_clc_loop:
  clc        ca1, EG2_DMX_AUTH_MARK(ca0)
  clc        ca4, EG2_DMX_AUTH_CLEAN(ca0)
  addi       t2, x0, 0
  FLAG       .Lclc_loop
  addi       t1, x0, 0
.Lclc_loop:
  clc        ca2, 0(ca1)
  cgettag    t0, ca2
  add        t1, t1, t0               // marked: the tag must be 0
  clc        ca2, 0(ca4)
  cgettag    t0, ca2
  xori       t0, t0, 1
  add        t1, t1, t0               // clean: the tag must be 1
  addi       t2, t2, 1
  clc        ca2, EG2_DMX_STOP(ca0)
  lw         t0, 0(ca2)
  beqz       t0, .Lclc_loop
  sw         t2, EG2_DMX_N(ca0)
  sw         t1, EG2_DMX_ERRS(ca0)
  cret
  .size eg2_dmx_clc_loop, . - eg2_dmx_clc_loop

// CSpecialRW of the debug SCRs (CHERIoT-Sail encdec: 0b0000001 @ scr @ cs1 @ 000 @ cd @ 1011011),
// as raw words: the assembler knows only SCRs 28-31 by name.
.macro SCR_STUB name, word
  .balign 8
  .globl eg2_dmx_\name
  .type eg2_dmx_\name, @function
eg2_dmx_\name:
  .4byte     \word
  cret
  .size eg2_dmx_\name, . - eg2_dmx_\name
.endm
  SCR_STUB   scr24_r, 0x0380065b      // cspecialr ca2, 24 (DEPCC)
  SCR_STUB   scr25_r, 0x0390065b      // cspecialr ca2, 25 (DScratchC0)
  SCR_STUB   scr26_r, 0x03a0065b      // cspecialr ca2, 26 (DScratchC1)
  SCR_STUB   scr24_w, 0x0385805b      // cspecialw 24, ca1
  SCR_STUB   scr25_w, 0x0395805b      // cspecialw 25, ca1
  SCR_STUB   scr26_w, 0x03a5805b      // cspecialw 26, ca1

#else  // RISC-V mode

.macro FLAG label
  lw         t0, EG2_DMX_FLAG(a0)
  la         t1, \label
  sw         t1, 0(t0)
.endm

.macro WAIT_STOP label
\label:
  lw         t0, EG2_DMX_STOP(a0)
  lw         t0, 0(t0)
  beqz       t0, \label
.endm

  .balign 4
  .globl eg2_dmx_park
  .type eg2_dmx_park, @function
eg2_dmx_park:
  la         a4, eg2_dmx_landing
  sw         a4, EG2_DMX_LANDING(a0)
  FLAG       .Lpark_wait
  WAIT_STOP  .Lpark_wait
  ret
  .size eg2_dmx_park, . - eg2_dmx_park

// Waits at the privilege dret gave it (U or M), then ecall.
  .balign 4
  .globl eg2_dmx_park_ecall
  .type eg2_dmx_park_ecall, @function
eg2_dmx_park_ecall:
  la         a4, eg2_dmx_landing
  sw         a4, EG2_DMX_LANDING(a0)
  FLAG       .Lpecall_wait
  WAIT_STOP  .Lpecall_wait
  ecall
  ret
  .size eg2_dmx_park_ecall, . - eg2_dmx_park_ecall

  .balign 8
  .globl eg2_dmx_landing
  .type eg2_dmx_landing, @function
eg2_dmx_landing:
  ecall
  ret
  .size eg2_dmx_landing, . - eg2_dmx_landing

#endif  // __CHERIOT__
