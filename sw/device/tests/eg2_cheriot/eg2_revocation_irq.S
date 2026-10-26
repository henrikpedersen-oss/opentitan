// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revocation_irq's handler and preempted thread (eg2_revocation_irq.c). CHERIoT
 * mode only; only x0-x15 are used.
 *
 * The handler is a context switch: MScratchC holds the context (C_* offsets, mirrored in the C
 * file's enum), whose first 128 bytes are the register save area in sec SRAM, outside every marked
 * granule. On entry it spills every capability register (c1-c15) there, handles the cause, and
 * reloads every register with CLC, so a register whose base granule is marked at the reload comes
 * back untagged (the load filter), as after an RTOS context switch. Causes:
 *  - timer (7): counted; the next compare is set `period` later (periodic) or rv_timer's interrupt
 *    is disabled (one-shot); the interrupt is cleared. On the timer interrupt numbered `mark_at`,
 *    the preemption frees an object: the bitmap word (C_MARK) is written with `mark_val`, and with
 *    0 again before the reload when `unmark` is set; `marked` records it.
 *  - external (11): counted; PLIC claim, cheriot_mem_sys INTR_STATE.tbre_done cleared, complete;
 *    the claimed source is recorded.
 *  - NMI (31): counted with its mcause and TBRE_STATUS at that moment; the watchdog stopped, its
 *    bark and rv_core_ibex NMI_STATE.wdog cleared.
 *  - anything else: counted, the registers restored, and the framework's handler reports it
 *    (`CTRAP ...`, the test fails).
 * The first four interrupt causes are recorded in order. The vector table has 32 entries that all
 * enter the handler, so it works whether MTCC is direct or vectored (Ibex: the NMI at +0x7c).
 */

#include "hw/top/aon_timer_regs.h"
#include "hw/top/rv_timer_regs.h"

  .option norvc
  .option norelax

// The context (eg2_revocation_irq.c kC*).
#define C_T_CMP 128
#define C_T_IE 136
#define C_T_IS 144
#define C_P_CC 152
#define C_M_IS 160
#define C_M_ST 168
#define C_A_CTRL 176
#define C_A_IS 184
#define C_I_NMI 192
#define C_MARK 200
#define C_PERIOD 208
#define C_CMP 212
#define C_N_TIMER 216
#define C_N_EXT 220
#define C_N_NMI 224
#define C_N_OTHER 228
#define C_MARK_AT 232
#define C_MARK_VAL 236
#define C_UNMARK 240
#define C_MARKED 244
#define C_PERIODIC 248
#define C_CLAIMED 252
#define C_ST_AT_NMI 256
#define C_NMI_CAUSE 260
#define C_ORDER_N 264
#define C_ORDER 268

#ifdef __CHERIOT__

  .section .text.eg2_rvirq, "ax", @progbits

  .balign 256
  .globl eg2_rvirq_vector
  .type eg2_rvirq_vector, @function
eg2_rvirq_vector:
  .rept 32
  j          .Lh
  .endr
.Lh:
  cspecialrw ct0, mscratchc, ct0
  csc        cra, 8(ct0)
  csc        csp, 16(ct0)
  csc        cgp, 24(ct0)
  csc        ctp, 32(ct0)
  csc        ct1, 48(ct0)
  csc        ct2, 56(ct0)
  csc        cs0, 64(ct0)
  csc        cs1, 72(ct0)
  csc        ca0, 80(ct0)
  csc        ca1, 88(ct0)
  csc        ca2, 96(ct0)
  csc        ca3, 104(ct0)
  csc        ca4, 112(ct0)
  csc        ca5, 120(ct0)
  csrr       t1, mcause
  // The order of the first four.
  lw         t2, C_ORDER_N(ct0)
  li         a0, 4
  bgeu       t2, a0, 1f
  slli       a0, t2, 2
  cincoffset ca1, ct0, a0
  sw         t1, C_ORDER(ca1)
1:
  addi       t2, t2, 1
  sw         t2, C_ORDER_N(ct0)
  bgez       t1, .Lh_other
  andi       t1, t1, 31
  li         a0, 7
  beq        t1, a0, .Lh_timer
  li         a0, 11
  beq        t1, a0, .Lh_ext
  li         a0, 31
  beq        t1, a0, .Lh_nmi
  j          .Lh_other

.Lh_timer:
  lw         t2, C_N_TIMER(ct0)
  addi       t2, t2, 1
  sw         t2, C_N_TIMER(ct0)
  lw         a1, C_PERIODIC(ct0)
  beqz       a1, 2f
  lw         a1, C_CMP(ct0)
  lw         a2, C_PERIOD(ct0)
  add        a1, a1, a2
  sw         a1, C_CMP(ct0)
  clc        ca0, C_T_CMP(ct0)
  sw         a1, 0(ca0)
  j          3f
2:
  clc        ca0, C_T_IE(ct0)
  sw         zero, 0(ca0)
3:
  clc        ca0, C_T_IS(ct0)
  li         a1, 1
  sw         a1, 0(ca0)
  // The preemption frees the victim's object on the timer interrupt numbered mark_at.
  lw         a1, C_MARK_AT(ct0)
  bne        a1, t2, .Lh_restore
  clc        ca0, C_MARK(ct0)
  lw         a1, C_MARK_VAL(ct0)
  sw         a1, 0(ca0)
  li         a2, 1
  sw         a2, C_MARKED(ct0)
  lw         a2, C_UNMARK(ct0)
  beqz       a2, .Lh_restore
  sw         zero, 0(ca0)
  j          .Lh_restore

.Lh_ext:
  lw         t2, C_N_EXT(ct0)
  addi       t2, t2, 1
  sw         t2, C_N_EXT(ct0)
  clc        ca0, C_P_CC(ct0)
  lw         a1, 0(ca0)
  sw         a1, C_CLAIMED(ct0)
  clc        ca2, C_M_IS(ct0)
  li         a3, 1
  sw         a3, 0(ca2)
  sw         a1, 0(ca0)
  j          .Lh_restore

.Lh_nmi:
  lw         t2, C_N_NMI(ct0)
  addi       t2, t2, 1
  sw         t2, C_N_NMI(ct0)
  csrr       a1, mcause
  sw         a1, C_NMI_CAUSE(ct0)
  clc        ca2, C_M_ST(ct0)
  lw         a1, 0(ca2)
  sw         a1, C_ST_AT_NMI(ct0)
  clc        ca0, C_A_CTRL(ct0)
  sw         zero, 0(ca0)
  clc        ca0, C_A_IS(ct0)
  li         a1, 1 << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT
  sw         a1, 0(ca0)
  clc        ca0, C_I_NMI(ct0)
  li         a1, 2  // NMI_STATE.wdog
  sw         a1, 0(ca0)
  j          .Lh_restore

.Lh_other:
  lw         t2, C_N_OTHER(ct0)
  addi       t2, t2, 1
  sw         t2, C_N_OTHER(ct0)
  clc        cra, 8(ct0)
  clc        csp, 16(ct0)
  clc        cgp, 24(ct0)
  clc        ctp, 32(ct0)
  clc        ct1, 48(ct0)
  clc        ct2, 56(ct0)
  clc        cs0, 64(ct0)
  clc        cs1, 72(ct0)
  clc        ca0, 80(ct0)
  clc        ca1, 88(ct0)
  clc        ca2, 96(ct0)
  clc        ca3, 104(ct0)
  clc        ca4, 112(ct0)
  clc        ca5, 120(ct0)
  cspecialrw ct0, mscratchc, ct0
  j          _eg2_cheriot_trap

.Lh_restore:
  clc        cra, 8(ct0)
  clc        csp, 16(ct0)
  clc        cgp, 24(ct0)
  clc        ctp, 32(ct0)
  clc        ct1, 48(ct0)
  clc        ct2, 56(ct0)
  clc        cs0, 64(ct0)
  clc        cs1, 72(ct0)
  clc        ca0, 80(ct0)
  clc        ca1, 88(ct0)
  clc        ca2, 96(ct0)
  clc        ca3, 104(ct0)
  clc        ca4, 112(ct0)
  clc        ca5, 120(ct0)
  cspecialrw ct0, mscratchc, ct0
  mret
  .size eg2_rvirq_vector, . - eg2_rvirq_vector

// void eg2_rvirq_thread(void *slots /*ca0*/, uint32_t *out /*ca1*/, void *ctx /*ca2*/,
//                       uint32_t want /*a3*/, uint32_t limit /*a4*/):
// the preempted thread. Loads the five slots' capabilities into cs0, cs1, ca2, ca3, ca5, enables
// interrupts and spins until the handler has taken `want` timer interrupts (at most `limit`
// iterations), disables them, and writes each register's tag and address to out[0..9] and the
// iterations left to out[10]. Then writes 0 to the bitmap word the handler may have marked
// (C_MARK), so no call runs while it is set, and clears the registers.
  .balign 4
  .globl eg2_rvirq_thread
  .type eg2_rvirq_thread, @function
eg2_rvirq_thread:
  cincoffset csp, csp, -16
  csc        cs0, 0(csp)
  csc        cs1, 8(csp)
  cmove      ct1, ca1
  cmove      ct2, ca2
  mv         a1, a3
  clc        cs0, 0(ca0)
  clc        cs1, 8(ca0)
  clc        ca2, 16(ca0)
  clc        ca3, 24(ca0)
  clc        ca5, 32(ca0)
  li         a0, 0
  csrsi      mstatus, 8
.Lthr_wait:
  lw         a0, C_N_TIMER(ct2)
  bgeu       a0, a1, .Lthr_done
  addi       a4, a4, -1
  bnez       a4, .Lthr_wait
.Lthr_done:
  csrci      mstatus, 8
  cgettag    a0, cs0
  sw         a0, 0(ct1)
  cgetaddr   a0, cs0
  sw         a0, 4(ct1)
  cgettag    a0, cs1
  sw         a0, 8(ct1)
  cgetaddr   a0, cs1
  sw         a0, 12(ct1)
  cgettag    a0, ca2
  sw         a0, 16(ct1)
  cgetaddr   a0, ca2
  sw         a0, 20(ct1)
  cgettag    a0, ca3
  sw         a0, 24(ct1)
  cgetaddr   a0, ca3
  sw         a0, 28(ct1)
  cgettag    a0, ca5
  sw         a0, 32(ct1)
  cgetaddr   a0, ca5
  sw         a0, 36(ct1)
  sw         a4, 40(ct1)
  clc        ca0, C_MARK(ct2)
  sw         zero, 0(ca0)
  li         a0, 0
  li         a2, 0
  li         a3, 0
  li         a5, 0
  li         t1, 0
  li         t2, 0
  clc        cs0, 0(csp)
  clc        cs1, 8(csp)
  cincoffset csp, csp, 16
  cret
  .size eg2_rvirq_thread, . - eg2_rvirq_thread

#endif  // __CHERIOT__
