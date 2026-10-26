// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_testrig_seq's entry into a stream (eg2_testrig_seq.c). CHERIoT mode only.
 *
 *   void eg2_testrig_run(void *entry /ca0/, void *save /ca1/)
 *
 * Saves the harness's registers through `save` (48 bytes, sec SRAM), arms the probe handler
 * (eg2_probe.S's record in MScratchC) with the recovery point below, puts `entry` in MEPCC, sets
 * MPIE so interrupts are taken in the stream, clears x1-x15 (the state after a DII reset), and
 * enters the stream with MRET. The stream ends with a trap, its end marker (ebreak) or another;
 * the handler records it and resumes here, where the harness's registers come back through the
 * memory root (MTDC, which the stream cannot change: its PCC has no SR permission) and the
 * address of `save`, kept in eg2_testrig_save_addr.
 */

#define REC_RECOVER 0
#define REC_ARMED 48
#define MSTATUS_MIE 0x8
#define MSTATUS_MPIE 0x80

  .text
  .balign 4
  .option push
  .option norvc
  .option norelax

  .globl eg2_testrig_run
  .type eg2_testrig_run, @function
eg2_testrig_run:
  csc        cra, 0(ca1)
  csc        csp, 8(ca1)
  csc        cgp, 16(ca1)
  csc        cs0, 24(ca1)
  csc        cs1, 32(ca1)
  csc        ctp, 40(ca1)
  cspecialr  ct1, mscratchc
  cllc       ct0, .Lrecover
  csc        ct0, REC_RECOVER(ct1)
  li         t0, 1
  sw         t0, REC_ARMED(ct1)
  cspecialw  mepcc, ca0
  li         t0, MSTATUS_MPIE
  csrs       mstatus, t0
  li         ra, 0
  li         sp, 0
  li         gp, 0
  li         tp, 0
  li         t1, 0
  li         t2, 0
  li         s0, 0
  li         s1, 0
  li         a0, 0
  li         a1, 0
  li         a2, 0
  li         a3, 0
  li         a4, 0
  li         a5, 0
  li         t0, 0
  mret
.Lrecover:
  csrci      mstatus, MSTATUS_MIE
  cspecialr  ct0, mtdc
  lui        t1, %hi(eg2_testrig_save_addr)
  addi       t1, t1, %lo(eg2_testrig_save_addr)
  csetaddr   ct0, ct0, t1
  lw         t1, 0(ct0)
  csetaddr   ct0, ct0, t1
  clc        cra, 0(ct0)
  clc        csp, 8(ct0)
  clc        cgp, 16(ct0)
  clc        cs0, 24(ct0)
  clc        cs1, 32(ct0)
  clc        ctp, 40(ct0)
  cret
  .size eg2_testrig_run, . - eg2_testrig_run

  .option pop
