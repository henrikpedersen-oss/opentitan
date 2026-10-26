// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_epmp_handover, RISC-V-mode part (eg2_late.h; EG2 spec "Mode switch and
 * handover", REQ_SOC_06).
 *
 * The meta SRAM is zeroed first; then a locked ePMP plan is programmed in entries 0-13 (entry 15 is
 * the test ROM's locked RWX entry over everything; with priority to the lowest index, the plan's
 * entries win):
 *   0     NAPOT 256 bytes over eg2_epmp_target (a function), locked, no access: denies its fetch
 *   1-8   the first `epmp_extra` of: sec SRAM, the RRAM test area, retention SRAM, rv_timer, gpio,
 *         aon_timer, uart1, sram_ctrl_sec.regs; locked, no access
 *   10-13 with `mml`: what the rest of this code needs under MML's rules: UART0 and main SRAM and
 *         rv_core_ibex.cfg locked RW, the RRAM locked RX
 * then mseccfg.MML with `mml`, and the CHERIoT switch as the framework makes it. eg2_epmp_handover.c
 * then reaches every denied region.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "sw/device/lib/base/multibits_asm.h"
#include "sw/device/tests/eg2_cheriot/eg2_epmp_handover_params.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"

#define MISA_E (1 << 4)
#define MSECCFG 0x747
#define NAPOT(base, size) (((base) | (((size) >> 1) - 1)) >> 2)
#define CFG_DENY 0x98   // L, NAPOT, no access
#define CFG_RW 0x9b     // L, NAPOT, R W
#define CFG_RX 0x9d     // L, NAPOT, R X

.macro la_abs rd, sym
  lui  \rd, %hi(\sym)
  addi \rd, \rd, %lo(\sym)
.endm

.macro xorshift r, tmp
  slli \tmp, \r, 13
  xor  \r, \r, \tmp
  srli \tmp, \r, 17
  xor  \r, \r, \tmp
  slli \tmp, \r, 5
  xor  \r, \r, \tmp
.endm

// cfg byte \val for entry \n (0-3 of a pmpcfg word) into \reg.
.macro cfg_byte reg, n, val, tmp
  li   \tmp, (\val) << (8 * (\n))
  or   \reg, \reg, \tmp
.endm

  .section .rodata
  EG2_EPMP_HANDOVER_PARAMS(EG2_LATE_PARAM_ASM_)

  .text
  .option push
  .option norvc
  .option norelax

  // The function the plan denies the fetch of, alone in its 256 bytes.
  .balign 256
  .globl eg2_epmp_target
  .type eg2_epmp_target, @function
eg2_epmp_target:
  li   a0, 0x5a1e
  cret
  .size eg2_epmp_target, . - eg2_epmp_target
  .balign 256

  .globl eg2_late_pre
  .type eg2_late_pre, @function
eg2_late_pre:
  cjal eg2_late_meta_init
  bnez a0, .Lfail
  la_abs s0, (_stack_start - EG2_LATE_AREA_BELOW_STACK)
  la_abs t0, kEg2Seed
  lw   a5, 0(t0)
  bnez a5, 1f
  li   a5, 1
1:
  // a4: epmp_extra (0-8), a3: mml (0, 1).
  la_abs t0, kEg2Param_epmp_extra
  lw   a4, 0(t0)
  li   t1, 9
  bltu a4, t1, 2f
  xorshift a5, t2
  remu a4, a5, t1
2:
  la_abs t0, kEg2Param_mml
  lw   a3, 0(t0)
  li   t1, 2
  bltu a3, t1, 3f
  xorshift a5, t2
  andi a3, a5, 1
3:
  sw   a4, (EG2_LATE_INFO + EG2_EPMP_INFO_EXTRA)(s0)
  sw   a3, (EG2_LATE_INFO + EG2_EPMP_INFO_MML)(s0)

  // The addresses.
  la_abs t0, eg2_epmp_target
  ori  t0, t0, 127
  srli t0, t0, 2
  csrw pmpaddr0, t0
  li   t0, NAPOT(TOP_SRAM_CTRL_SEC_RAM_BASE_ADDR, TOP_SRAM_CTRL_SEC_RAM_SIZE_BYTES)
  csrw pmpaddr1, t0
  li   t0, NAPOT(0x30100000, 0x8000)
  csrw pmpaddr2, t0
  li   t0, NAPOT(TOP_SRAM_CTRL_RET_RAM_BASE_ADDR, TOP_SRAM_CTRL_RET_RAM_SIZE_BYTES)
  csrw pmpaddr3, t0
  li   t0, NAPOT(TOP_RV_TIMER_BASE_ADDR, TOP_RV_TIMER_SIZE_BYTES)
  csrw pmpaddr4, t0
  li   t0, NAPOT(TOP_GPIO_BASE_ADDR, TOP_GPIO_SIZE_BYTES)
  csrw pmpaddr5, t0
  li   t0, NAPOT(TOP_AON_TIMER_BASE_ADDR, TOP_AON_TIMER_SIZE_BYTES)
  csrw pmpaddr6, t0
  li   t0, NAPOT(TOP_UART1_BASE_ADDR, TOP_UART1_SIZE_BYTES)
  csrw pmpaddr7, t0
  li   t0, NAPOT(TOP_SRAM_CTRL_SEC_REGS_BASE_ADDR, TOP_SRAM_CTRL_SEC_REGS_SIZE_BYTES)
  csrw pmpaddr8, t0
  li   t0, NAPOT(TOP_UART0_BASE_ADDR, TOP_UART0_SIZE_BYTES)
  csrw pmpaddr10, t0
  li   t0, NAPOT(TOP_SRAM_CTRL_MAIN_RAM_BASE_ADDR, TOP_SRAM_CTRL_MAIN_RAM_SIZE_BYTES)
  csrw pmpaddr11, t0
  li   t0, NAPOT(TOP_RV_CORE_IBEX_CFG_BASE_ADDR, TOP_RV_CORE_IBEX_CFG_SIZE_BYTES)
  csrw pmpaddr12, t0
  li   t0, NAPOT(TOP_RRAM_CTRL_HOST_BASE_ADDR, TOP_RRAM_CTRL_HOST_SIZE_BYTES)
  csrw pmpaddr13, t0

  // The configurations: entry 0 always; entries 1..a4; with MML 10-13.
  li   t2, CFG_DENY
  mv   a0, x0            // pmpcfg0: entries 0-3
  mv   a1, x0            // pmpcfg1: 4-7
  mv   a2, x0            // pmpcfg2: 8-11
  cfg_byte a0, 0, CFG_DENY, t1
  li   t0, 1
  bltu a4, t0, 4f
  cfg_byte a0, 1, CFG_DENY, t1
  li   t0, 2
  bltu a4, t0, 4f
  cfg_byte a0, 2, CFG_DENY, t1
  li   t0, 3
  bltu a4, t0, 4f
  cfg_byte a0, 3, CFG_DENY, t1
  li   t0, 4
  bltu a4, t0, 4f
  cfg_byte a1, 0, CFG_DENY, t1
  li   t0, 5
  bltu a4, t0, 4f
  cfg_byte a1, 1, CFG_DENY, t1
  li   t0, 6
  bltu a4, t0, 4f
  cfg_byte a1, 2, CFG_DENY, t1
  li   t0, 7
  bltu a4, t0, 4f
  cfg_byte a1, 3, CFG_DENY, t1
  li   t0, 8
  bltu a4, t0, 4f
  cfg_byte a2, 0, CFG_DENY, t1
4:
  // pmpcfg3 keeps entry 15 (the test ROM's, locked); entries 12-14 below it.
  csrr a4, pmpcfg3
  beqz a3, 5f
  cfg_byte a2, 2, CFG_RW, t1   // 10 UART0
  cfg_byte a2, 3, CFG_RW, t1   // 11 main SRAM
  cfg_byte a4, 0, CFG_RW, t1   // 12 rv_core_ibex.cfg
  cfg_byte a4, 1, CFG_RX, t1   // 13 RRAM
5:
  csrw pmpcfg3, a4
  csrw pmpcfg2, a2
  csrw pmpcfg1, a1
  csrw pmpcfg0, a0
  csrr t0, pmpcfg0
  sw   t0, (EG2_LATE_INFO + EG2_EPMP_INFO_PMPCFG0)(s0)
  csrr t0, pmpcfg1
  sw   t0, (EG2_LATE_INFO + EG2_EPMP_INFO_PMPCFG0 + 4)(s0)
  csrr t0, pmpcfg2
  sw   t0, (EG2_LATE_INFO + EG2_EPMP_INFO_PMPCFG0 + 8)(s0)
  csrr t0, pmpcfg3
  sw   t0, (EG2_LATE_INFO + EG2_EPMP_INFO_PMPCFG0 + 12)(s0)
  beqz a3, 6f
  csrsi MSECCFG, 1             // MML
6:
  csrr t0, MSECCFG
  sw   t0, (EG2_LATE_INFO + EG2_EPMP_INFO_MSECCFG)(s0)

  // The switch.
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  fence.i
  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, .Lfail_misa
  j    eg2_late_cheriot
.Lfail:
  li   a0, 0
  li   a1, 0x10
  j    eg2_late_rv_end
.Lfail_misa:
  li   a0, 0
  li   a1, 0x12
  j    eg2_late_rv_end
  .size eg2_late_pre, . - eg2_late_pre

  .option pop
