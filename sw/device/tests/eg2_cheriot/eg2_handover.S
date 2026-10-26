// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_handover, RISC-V-mode part (eg2_late.h; EG2 spec "Mode switch and handover",
 * REQ_SOC_07).
 *
 * In RISC-V mode: zero the meta SRAM; draw the knobs left to the seed; give x1-x31, mtvec and mepc
 * values of the drawn class (`gpr_fill`), recording them in the capture area's fill table;
 * cpuctrlsts.icache_enable and dummy_instr_en as drawn; then load every register from the table
 * (x16-x31 with raw encodings, which the CHERIoT assembler does not name) and lock CHERIoT mode,
 * with the drawn instruction around the CHERIOT_LOCK store (`switch_point`: a store or a load just
 * before it, WFI, a taken branch or a CSR write just after it), then `fence.i`. A load or store
 * after the lock store would run in either mode, and in CHERIoT mode its integer base register is
 * an untagged capability: memory accesses go before the lock.
 *
 * In CHERIoT mode, before anything else: each of x1-x15 and every SCR stored whole (CSC, tag
 * kept) into the capture area. Two registers hold the lock sequence's own values (s1 the
 * rv_core_ibex.cfg base, t0 the capture area's address, both integers set in RISC-V mode, so
 * their expected tag is 0 too); s1 is parked in MTDC while MTDC's root, moved to the capture area,
 * is the store authority. MTCC then points at the trap vector again, and the start-up code
 * continues (eg2_late_cheriot); eg2_handover.c checks what was captured.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/rv_timer_regs.h"      // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "sw/device/lib/base/multibits_asm.h"
#include "sw/device/tests/eg2_cheriot/eg2_handover_params.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"

#define CPUCTRLSTS 0x7c0
#define MSTATUS_MIE 0x8
#define MIE_MTIE 0x80

.macro la_abs rd, sym
  lui  \rd, %hi(\sym)
  addi \rd, \rd, %lo(\sym)
.endm

// xorshift32 on \r.
.macro xorshift r, tmp
  slli \tmp, \r, 13
  xor  \r, \r, \tmp
  srli \tmp, \r, 17
  xor  \r, \r, \tmp
  slli \tmp, \r, 5
  xor  \r, \r, \tmp
.endm

// A value of class a3 (0 zero, 1 all ones, 2 random, 3 pointer-like) into \out; a5 the generator.
.macro fill_value out
  li   \out, 0
  beqz a3, 91f
  li   \out, -1
  addi t2, a3, -1
  beqz t2, 91f
  xorshift a5, t2
  mv   \out, a5
  addi t2, a3, -2
  beqz t2, 91f
  li   t2, 0x2fff8
  and  \out, a5, t2
  li   t2, 0x10000000
  add  \out, \out, t2
91:
.endm

// lw x\rd, 4 * rd(a1), for the registers the CHERIoT assembler has no name for.
.macro lw_x rd
  .word ((4 * \rd) << 20) | (11 << 15) | (2 << 12) | (\rd << 7) | 0x03
.endm

// Every register from the fill table; a1 last, as it is the base.
.macro load_all
  addi a1, s0, EG2_LATE_FILL
  .irp n, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31
  lw_x \n
  .endr
  lw   ra, 4(a1)
  lw   sp, 8(a1)
  lw   gp, 12(a1)
  lw   tp, 16(a1)
  lw   t0, 20(a1)
  lw   t1, 24(a1)
  lw   t2, 28(a1)
  lw   s0, 32(a1)
  lw   s1, 36(a1)
  lw   a0, 40(a1)
  lw   a2, 48(a1)
  lw   a3, 52(a1)
  lw   a4, 56(a1)
  lw   a5, 60(a1)
  lw   a1, 44(a1)
.endm

.macro lock_pre
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
.endm

.macro lock_post
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK)
  fence.i
  j    .Lcapture
.endm

  .section .rodata
  EG2_HANDOVER_PARAMS(EG2_LATE_PARAM_ASM_)

  .text
  .balign 4
  .option push
  .option norvc
  .option norelax

  .globl eg2_late_pre
  .type eg2_late_pre, @function
eg2_late_pre:
  cjal eg2_late_meta_init
  beqz a0, 1f
  li   a0, 0
  li   a1, 0x10   // the meta SRAM's INIT failed
  j    eg2_late_rv_end
1:
  la_abs s0, _stack_start
  li   t0, EG2_LATE_AREA_BELOW_STACK
  sub  s0, s0, t0

  la_abs t0, kEg2Seed
  lw   a5, 0(t0)
  bnez a5, 2f
  li   a5, 1
2:
  la_abs t0, kEg2Param_switch_point
  lw   a4, 0(t0)
  li   t1, 5
  bltu a4, t1, 3f
  xorshift a5, t2
  remu a4, a5, t1
3:
  sw   a4, (EG2_LATE_INFO + EG2_HANDOVER_INFO_SWITCH_POINT)(s0)
  la_abs t0, kEg2Param_gpr_fill
  lw   a3, 0(t0)
  li   t1, 4
  bltu a3, t1, 4f
  xorshift a5, t2
  remu a3, a5, t1
4:
  sw   a3, (EG2_LATE_INFO + EG2_HANDOVER_INFO_FILL)(s0)

  // The fill table: x0 = 0, x1-x31 of the class.
  addi a1, s0, EG2_LATE_FILL
  sw   x0, 0(a1)
  li   a0, 1
5:
  fill_value a2
  slli t0, a0, 2
  add  t0, t0, a1
  sw   a2, 0(t0)
  addi a0, a0, 1
  li   t1, 32
  bltu a0, t1, 5b

  // mtvec and mepc of the class, as the core holds them after the write.
  fill_value a2
  andi a2, a2, -4
  csrw mtvec, a2
  csrr a2, mtvec
  sw   a2, (EG2_LATE_INFO + EG2_HANDOVER_INFO_MTVEC)(s0)
  fill_value a2
  andi a2, a2, -4
  csrw mepc, a2
  csrr a2, mepc
  sw   a2, (EG2_LATE_INFO + EG2_HANDOVER_INFO_MEPC)(s0)

  // The instruction cache and dummy instructions.
  la_abs t0, kEg2Param_icache
  lw   t1, 0(t0)
  li   t2, 2
  beq  t1, t2, 6f
  csrci CPUCTRLSTS, 1
  beqz t1, 6f
  csrsi CPUCTRLSTS, 1
6:
  la_abs t0, kEg2Param_dummy_instr
  lw   t1, 0(t0)
  li   t2, 2
  bltu t1, t2, 7f
  xorshift a5, t2
  andi t1, a5, 1
7:
  csrci CPUCTRLSTS, 4
  beqz t1, 8f
  csrsi CPUCTRLSTS, 4
8:
  csrr t1, CPUCTRLSTS
  sw   t1, (EG2_LATE_INFO + EG2_HANDOVER_INFO_CPUCTRL)(s0)

  // The lock sequence's registers in the table: t0 the capture area, s1 rv_core_ibex.cfg, t1 the
  // load's value where the switch point is a load of CHERIOT_ENA.
  sw   s0, (EG2_LATE_FILL + 4 * 5)(s0)
  li   t0, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  sw   t0, (EG2_LATE_FILL + 4 * 9)(s0)
  li   t0, 4
  bne  a4, t0, 9f
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, (EG2_LATE_FILL + 4 * 6)(s0)
9:
  // The WFI switch point: rv_timer raises the timer interrupt some time after the lock (mie.MTIE
  // set, mstatus.MIE clear, so it wakes the core without a trap).
  li   t0, 1
  bne  a4, t0, 10f
  li   t0, TOP_RV_TIMER_BASE_ADDR
  sw   x0, RV_TIMER_CTRL_REG_OFFSET(t0)
  li   t1, (1 << RV_TIMER_CFG0_STEP_OFFSET)
  sw   t1, RV_TIMER_CFG0_REG_OFFSET(t0)
  sw   x0, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET(t0)
  sw   x0, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET(t0)
  sw   x0, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET(t0)
  li   t1, 2000
  sw   t1, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET(t0)
  li   t1, 1
  sw   t1, RV_TIMER_INTR_ENABLE0_REG_OFFSET(t0)
  sw   t1, RV_TIMER_CTRL_REG_OFFSET(t0)
  csrci mstatus, MSTATUS_MIE
  li   t1, MIE_MTIE
  csrs mie, t1
10:
  beqz a4, .Lsp_store
  li   t0, 1
  beq  a4, t0, .Lsp_wfi
  li   t0, 2
  beq  a4, t0, .Lsp_branch
  li   t0, 3
  beq  a4, t0, .Lsp_csr
  j    .Lsp_load

.Lsp_store:
  load_all
  lock_pre
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  lock_post

.Lsp_wfi:
  load_all
  lock_pre
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  wfi
  lock_post

.Lsp_branch:
  load_all
  lock_pre
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  beq  x0, x0, 11f
  addi x0, x0, 0
11:
  lock_post

.Lsp_csr:
  load_all
  lock_pre
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  csrw mcause, t1
  lock_post

.Lsp_load:
  load_all
  lock_pre
  lw   t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  lock_post

  // CHERIoT mode from here.
.Lcapture:
  cspecialrw cs1, mtdc, cs1
  csetaddr   cs1, cs1, t0
  csc        ct0, EG2_LATE_X(5)(cs1)
  csc        cra, EG2_LATE_X(1)(cs1)
  csc        csp, EG2_LATE_X(2)(cs1)
  csc        cgp, EG2_LATE_X(3)(cs1)
  csc        ctp, EG2_LATE_X(4)(cs1)
  csc        ct1, EG2_LATE_X(6)(cs1)
  csc        ct2, EG2_LATE_X(7)(cs1)
  csc        cs0, EG2_LATE_X(8)(cs1)
  csc        ca0, EG2_LATE_X(10)(cs1)
  csc        ca1, EG2_LATE_X(11)(cs1)
  csc        ca2, EG2_LATE_X(12)(cs1)
  csc        ca3, EG2_LATE_X(13)(cs1)
  csc        ca4, EG2_LATE_X(14)(cs1)
  csc        ca5, EG2_LATE_X(15)(cs1)
  cspecialr  ct0, mtdc
  csc        ct0, EG2_LATE_X(9)(cs1)
  cspecialw  mtdc, cs1
  csc        cs1, EG2_LATE_MTDC(cs1)
  cspecialr  ct0, mtcc
  csc        ct0, EG2_LATE_MTCC(cs1)
  cspecialr  ct0, mepcc
  csc        ct0, EG2_LATE_MEPCC(cs1)
  cspecialr  ct0, mscratchc
  csc        ct0, EG2_LATE_MSCRATCHC(cs1)
  .globl eg2_handover_pcc_at
eg2_handover_pcc_at:
  auipcc     ct0, 0
  csc        ct0, EG2_LATE_PCC(cs1)
  // The trap vector again.
  la_abs     t1, _eg2_late_trap
  csetaddr   ct0, ct0, t1
  cspecialw  mtcc, ct0
  j          eg2_late_cheriot
  .size eg2_late_pre, . - eg2_late_pre

  .option pop
