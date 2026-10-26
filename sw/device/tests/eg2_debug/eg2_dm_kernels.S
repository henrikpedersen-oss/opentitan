// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The debug-module corner tests' kernels (eg2_dm_corners.h): tight loops that keep the core in one
 * pipeline state, so the bench can time a halt request against it. Each kernel runs until the word
 * at ctx->stop is non-zero or ctx->max iterations, under a trap vector of its own (MTCC in CHERIoT
 * mode, `mtvec` in RISC-V mode; Ibex's `mtvec` is always vectored, so the 32-entry table sends
 * every exception, interrupt and the NMI at +0x7c to one handler). Only x0-x15 are used.
 *
 * Registers while a kernel runs (the handler keeps all but t0, which it saves in MScratch(C)):
 *   s0 ctx   s1 interrupts taken   t2 exceptions taken   a2 iterations   a3 checksum
 *   a4 max   a5 the stop word      a1 the kernel's device or slots   a0, t0 temporaries
 *   t1 the handler's own scratch: no kernel uses it.
 * In the PARK kernel a0 and t1 are free: the debugger may use them in the program buffer.
 *
 * The handler: an exception (TRAP's `ecall`) counts in t2 and resumes after it; the software
 * interrupt (IRQ) counts in s1 and clears rv_plic's MSIP0; the NMI (the watchdog bark) counts in s1
 * and writes ctx->ack[i] = ctx->ack_val[i] (watchdog off, bark cleared, NMI_STATE cleared); any
 * other interrupt counts in s1 and clears `mie`, so the C side's count check sees it.
 */

#include "hw/top/aon_timer_regs.h"  // Generated.
#include "hw/top/rv_timer_regs.h"   // Generated.

  .option norvc
  .option norelax

#ifdef __CHERIOT__
#define PSZ 8
#define LDP(r, off, b) clc c##r, off(c##b)
#define LW32(r, off, b) lw r, off(c##b)
#define SW32(r, off, b) sw r, off(c##b)
#define LDCAP(r, off, b) clc c##r, off(c##b)
#define STCAP(r, off, b) csc c##r, off(c##b)
#define SAVE_T0 cspecialw mscratchc, ct0
#define RESTORE_T0 cspecialr ct0, mscratchc
#define MEPC_ADD4 \
  cspecialr ct0, mepcc; \
  cincoffsetimm ct0, ct0, 4; \
  cspecialw mepcc, ct0
#define ADDR_OF(r, label) \
  cllc c##r, label; \
  cgetaddr r, c##r
#else
#define PSZ 4
#define LDP(r, off, b) lw r, off(b)
#define LW32(r, off, b) lw r, off(b)
#define SW32(r, off, b) sw r, off(b)
#define LDCAP(r, off, b) lw r, off(b)
#define STCAP(r, off, b) sw r, off(b)
#define SAVE_T0 csrw mscratch, t0
#define RESTORE_T0 csrr t0, mscratch
#define MEPC_ADD4 \
  csrr t0, mepc; \
  addi t0, t0, 4; \
  csrw mepc, t0
#define ADDR_OF(r, label) la r, label
#endif

// eg2_dm_kctx_t (eg2_dm_corners.h; the C side checks these offsets).
#define K_STOP (0 * PSZ)
#define K_DEV (1 * PSZ)
#define K_SLOTS (2 * PSZ)
#define K_MSIP (3 * PSZ)
#define K_ACK (4 * PSZ)
#define K_ACKV (7 * PSZ)
#define K_MAX (7 * PSZ + 12)
#define K_ARG (7 * PSZ + 16)
#define K_N (7 * PSZ + 20)
#define K_SUM (7 * PSZ + 24)
#define K_TRAPS (7 * PSZ + 28)
#define K_IRQS (7 * PSZ + 32)

// One iteration done: count it, stop on the stop word or at max.
.macro KTAIL loop
  addi       a2, a2, 1
  LW32(t0, 0, a5)
  bnez       t0, .Lk_done
  bltu       a2, a4, \loop
  j          .Lk_done
.endm

  .section .text.eg2_dm_kernel, "ax", @progbits
  .balign 256
.Lk_vec:
  .rept 32
  j          .Lk_handler
  .endr
.Lk_handler:
  SAVE_T0
  csrr       t0, mcause
  bltz       t0, .Lk_intr
  addi       t2, t2, 1
  MEPC_ADD4
  j          .Lk_hret
.Lk_intr:
  addi       s1, s1, 1
  andi       t0, t0, 31
  addi       t0, t0, -31
  beqz       t0, .Lk_nmi
  addi       t0, t0, 28
  beqz       t0, .Lk_msi
  csrw       mie, zero
  j          .Lk_hret
.Lk_msi:
  LDP(t0, K_MSIP, s0)
  SW32(zero, 0, t0)
  LW32(t0, 0, t0)
  j          .Lk_hret
.Lk_nmi:
  LDP(t0, K_ACK + 0 * PSZ, s0)
  LW32(t1, K_ACKV + 0, s0)
  SW32(t1, 0, t0)
  LDP(t0, K_ACK + 1 * PSZ, s0)
  LW32(t1, K_ACKV + 4, s0)
  SW32(t1, 0, t0)
  LDP(t0, K_ACK + 2 * PSZ, s0)
  LW32(t1, K_ACKV + 8, s0)
  SW32(t1, 0, t0)
.Lk_hret:
  RESTORE_T0
  mret

// ------------------------------------------------------------------------------------------------
// uint32_t eg2_dm_kernel(uint32_t kind /*a0*/, eg2_dm_kctx_t *ctx /*a1*/): returns the iterations.
  .balign 4
  .globl eg2_dm_kernel
  .type eg2_dm_kernel, @function
eg2_dm_kernel:
#ifdef __CHERIOT__
  cincoffsetimm csp, csp, -48
  csc        cra, 0(csp)
  csc        cs0, 8(csp)
  csc        cs1, 16(csp)
  cspecialr  ct0, mtcc
  csc        ct0, 24(csp)
  cspecialr  ct0, mscratchc
  csc        ct0, 32(csp)
  cmove      cs0, ca1
  cllc       ct0, .Lk_vec
  cspecialw  mtcc, ct0
#else
  addi       sp, sp, -32
  sw         ra, 0(sp)
  sw         s0, 4(sp)
  sw         s1, 8(sp)
  csrr       t0, mtvec
  sw         t0, 12(sp)
  csrr       t0, mscratch
  sw         t0, 16(sp)
  mv         s0, a1
  la         t0, .Lk_vec
  csrw       mtvec, t0
#endif
  LDP(a5, K_STOP, s0)
  LDP(a1, K_DEV, s0)
  LW32(a4, K_MAX, s0)
  li         a2, 0
  li         a3, 0
  li         t2, 0
  li         s1, 0
  addi       t0, a0, -1
  beqz       t0, .Lk_div
  addi       t0, t0, -1
  beqz       t0, .Lk_load
  addi       t0, t0, -1
  beqz       t0, .Lk_store
  addi       t0, t0, -1
  beqz       t0, .Lk_cap
  addi       t0, t0, -1
  beqz       t0, .Lk_wfi_setup
  addi       t0, t0, -1
  beqz       t0, .Lk_trap
  addi       t0, t0, -1
  beqz       t0, .Lk_irq
  addi       t0, t0, -1
  beqz       t0, .Lk_nmi_k
  addi       t0, t0, -1
  beqz       t0, .Lk_park
  j          .Lk_done                 // unknown kind: no iteration

// 1 DIV: a divide every iteration, dividend from the running sum.
.Lk_div:
  andi       t0, a2, 255
  addi       t0, t0, 3
  addi       a0, a3, 1365
.Lt_div:
  div        a0, a0, t0
  add        a3, a3, a0
  KTAIL      .Lk_div

// 2 LOAD: a load of a constant word in RRAM (ctx->dev).
.Lk_load:
.Lt_load:
  LW32(t0, 0, a1)
  add        a3, a3, t0
  KTAIL      .Lk_load

// 3 STORE: the iteration number stored to an always-on register (ctx->dev), across the CDC.
.Lk_store:
.Lt_store:
  SW32(a2, 0, a1)
  addi       a3, a3, 1
  KTAIL      .Lk_store

// 4 CAP: two slots swapped (CLC/CSC in CHERIoT mode, LW/SW of the first words in RISC-V mode).
.Lk_cap:
  LDP(a1, K_SLOTS, s0)
.Lk_cap_loop:
.Lt_cap:
  LDCAP(t0, 0, a1)
  LDCAP(a0, 8, a1)
  STCAP(a0, 0, a1)
  STCAP(t0, 8, a1)
  addi       a3, a3, 1
  KTAIL      .Lk_cap_loop

// 5 WFI: sleep until rv_timer (ctx->dev) fires, re-arm it ctx->arg ticks later (mstatus.MIE is
// off: the interrupt wakes the core without a trap).
.Lk_wfi_setup:
  LW32(a0, K_ARG, s0)
.Lk_wfi:
.Lt_wfi:
  wfi
  LW32(t0, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, a1)
  add        t0, t0, a0
  SW32(t0, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, a1)
  li         t0, 1
  SW32(t0, RV_TIMER_INTR_STATE0_REG_OFFSET, a1)
  addi       a3, a3, 1
  KTAIL      .Lk_wfi

// 6 TRAP: an ecall every iteration; the handler resumes after it.
.Lk_trap:
.Lt_trap:
  ecall
  addi       a3, a3, 1
  KTAIL      .Lk_trap

// 7 IRQ: rv_plic's MSIP0 (ctx->dev) set every iteration; the handler clears it. Waits (bounded)
// until the interrupt has been taken.
.Lk_irq:
  addi       a0, s1, 1
  li         t0, 1
.Lt_irq:
  SW32(t0, 0, a1)
  li         t0, 1000
.Lk_irq_wait:
  beq        s1, a0, .Lk_irq_next
  addi       t0, t0, -1
  bnez       t0, .Lk_irq_wait
.Lk_irq_next:
  addi       a3, a3, 1
  KTAIL      .Lk_irq

// 8 NMI: the watchdog (aon_timer, ctx->dev) restarted and enabled every iteration; its bark is the
// NMI, which the handler acknowledges. Waits (bounded by ctx->arg) until the NMI has been taken.
.Lk_nmi_k:
  addi       a0, s1, 1
  SW32(zero, AON_TIMER_WDOG_COUNT_REG_OFFSET, a1)
  li         t0, 1
  SW32(t0, AON_TIMER_WDOG_CTRL_REG_OFFSET, a1)
  LW32(t0, K_ARG, s0)
.Lt_nmi:
  beq        s1, a0, .Lk_nmi_next
  addi       t0, t0, -1
  bnez       t0, .Lt_nmi
.Lk_nmi_next:
  addi       a3, a3, 1
  KTAIL      .Lk_nmi_k

// 9 PARK: a counting loop; a0 and t1 are the debugger's.
.Lk_park:
.Lt_park:
  addi       a3, a3, 1
  KTAIL      .Lk_park

.Lk_done:
  SW32(a2, K_N, s0)
  SW32(a3, K_SUM, s0)
  SW32(t2, K_TRAPS, s0)
  SW32(s1, K_IRQS, s0)
  mv         a0, a2
#ifdef __CHERIOT__
  clc        ct0, 24(csp)
  cspecialw  mtcc, ct0
  clc        ct0, 32(csp)
  cspecialw  mscratchc, ct0
  clc        cra, 0(csp)
  clc        cs0, 8(csp)
  clc        cs1, 16(csp)
  cincoffsetimm csp, csp, 48
  cret
#else
  lw         t0, 12(sp)
  csrw       mtvec, t0
  lw         t0, 16(sp)
  csrw       mscratch, t0
  lw         ra, 0(sp)
  lw         s0, 4(sp)
  lw         s1, 8(sp)
  addi       sp, sp, 32
  ret
#endif
  .size eg2_dm_kernel, . - eg2_dm_kernel

// ------------------------------------------------------------------------------------------------
// uint32_t eg2_dm_kernel_target(uint32_t kind /*a0*/): the bus address of the kernel's timed
// instruction (kind 0: the trap vector).
  .balign 4
  .globl eg2_dm_kernel_target
  .type eg2_dm_kernel_target, @function
eg2_dm_kernel_target:
  mv         t0, a0
  ADDR_OF(a0, .Lk_vec)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_div)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_load)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_store)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_cap)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_wfi)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_trap)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_irq)
  beqz       t0, .Lt_ret
  addi       t0, t0, -1
  ADDR_OF(a0, .Lt_nmi)
  beqz       t0, .Lt_ret
  ADDR_OF(a0, .Lt_park)
.Lt_ret:
#ifdef __CHERIOT__
  cret
#else
  ret
#endif
  .size eg2_dm_kernel_target, . - eg2_dm_kernel_target
