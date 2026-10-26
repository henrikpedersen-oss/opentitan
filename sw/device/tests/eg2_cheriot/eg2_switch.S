// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_core_ibex_cheriot_switch, RISC-V-mode part (eg2_late.h; EG2 spec "Mode switch and
 * handover", REQ_SOC_03).
 *
 * `ena_writes` writes of CHERIOT_ENA, each of a drawn class (MuBi4True, MuBi4False, another
 * encoding), each read back (the register is rw); then the final mode's strict value; then the
 * lock, from the core or over SBA (a bench step); `fence.i`.
 *  - CHERIoT mode locked: eg2_switch.c continues (after the start-up code's steps).
 *  - ePMP mode locked: here, in RISC-V mode: misa I set and E clear; then the other mode requested
 *    (CHERIOT_ENA MuBi4True, CHERIOT_LOCK MuBi4True) and misa read again: still RISC-V; the verdict
 *    from here (`EG2 late FAIL <code>` on a failure).
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "sw/device/lib/base/multibits_asm.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"
#include "sw/device/tests/eg2_cheriot/eg2_switch_params.h"

#define MISA_E (1 << 4)
#define MISA_I (1 << 8)

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

  .section .rodata
  EG2_SWITCH_PARAMS(EG2_LATE_PARAM_ASM_)
.L_op_sba_wr32:
  .asciz "sba_wr32"

  .text
  .balign 4
  .option push
  .option norvc
  .option norelax

  .globl eg2_late_pre
  .type eg2_late_pre, @function
eg2_late_pre:
  cjal eg2_late_meta_init
  bnez a0, .Lfail_meta
  la_abs s0, (_stack_start - EG2_LATE_AREA_BELOW_STACK)
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  // The generator: kEg2Seed, or 1.
  la_abs t0, kEg2Seed
  lw   a5, 0(t0)
  bnez a5, 1f
  li   a5, 1
1:
  // ena_writes (9 draws 0-8) in a4.
  la_abs t0, kEg2Param_ena_writes
  lw   a4, 0(t0)
  li   t1, 9
  bltu a4, t1, 2f
  xorshift a5, t2
  remu a4, a5, t1
2:
  sw   a4, (EG2_LATE_INFO + EG2_SWITCH_INFO_WRITES)(s0)
  sw   x0, (EG2_LATE_INFO + EG2_SWITCH_INFO_READBACK)(s0)
3:
  beqz a4, .Lfinal
  // The class: the knob, or 50 : 25 : 25.
  la_abs t0, kEg2Param_ena_class
  lw   a3, 0(t0)
  bnez a3, 4f
  xorshift a5, t2
  andi a3, a5, 3          // 0, 1: True; 2: False; 3: another
  li   t1, 2
  bltu a3, t1, 41f
  j    4f              // 2 False, 3 another
41:
  li   a3, 1
4:
  li   a2, MULTIBIT_ASM_BOOL4_TRUE
  li   t1, 1
  beq  a3, t1, 5f
  li   a2, MULTIBIT_ASM_BOOL4_FALSE
  li   t1, 2
  beq  a3, t1, 5f
  // Another encoding: any of the other fourteen.
  xorshift a5, t2
  andi a2, a5, 0xf
  li   t1, MULTIBIT_ASM_BOOL4_TRUE
  bne  a2, t1, 42f
  addi a2, a2, 1
42:
  li   t1, MULTIBIT_ASM_BOOL4_FALSE
  bne  a2, t1, 5f
  addi a2, a2, 1
5:
  sw   a2, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  lw   t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  andi t1, t1, 0xf
  beq  t1, a2, 6f
  lw   t1, (EG2_LATE_INFO + EG2_SWITCH_INFO_READBACK)(s0)
  addi t1, t1, 1
  sw   t1, (EG2_LATE_INFO + EG2_SWITCH_INFO_READBACK)(s0)
6:
  addi a4, a4, -1
  j    3b

.Lfinal:
  // final_mode in a4: 1 CHERIoT, 2 ePMP.
  la_abs t0, kEg2Param_final_mode
  lw   a4, 0(t0)
  bnez a4, 7f
  xorshift a5, t2
  andi a4, a5, 1
  addi a4, a4, 1
7:
  li   a2, MULTIBIT_ASM_BOOL4_TRUE
  li   t1, 1
  beq  a4, t1, 8f
  li   a2, MULTIBIT_ASM_BOOL4_FALSE
8:
  sw   a2, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  // lock_source: 1 the core, 2 SBA (70 : 30); without a bench (model_gaps) the core.
  la_abs t0, kEg2Param_lock_source
  lw   a3, 0(t0)
  bnez a3, 9f
  xorshift a5, t2
  li   t1, 10
  remu a3, a5, t1
  li   t1, 7
  sltu a3, a3, t1          // 1 if < 7 (the core)
  xori a3, a3, 1
  addi a3, a3, 1           // 1 core, 2 SBA
9:
  la_abs t0, kEg2Param_model_gaps
  lw   t1, 0(t0)
  beqz t1, 10f
  li   a3, 1
10:
  sw   a3, (EG2_LATE_INFO + EG2_SWITCH_INFO_SOURCE)(s0)
  li   t1, 2
  beq  a3, t1, .Lsba_lock
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  j    .Llocked
.Lsba_lock:
  // The bench writes MuBi4True to CHERIOT_LOCK over SBA. a4 (the final mode) waits in s1 (the
  // helper keeps s0 and s1). Locking CHERIoT mode switches the core under whatever it runs, so for
  // that mode the core does not wait for the bench's answer (a load from RRAM through an
  // integer base would fault once switched): it polls misa.E with CSR reads only, which run alike
  // in both modes.
  mv   s1, a4
  la_abs a0, .L_op_sba_wr32
  li   a1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR + RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET
  li   a2, MULTIBIT_ASM_BOOL4_TRUE
  li   t1, 1
  sub  a3, t1, a4
  seqz a3, a3          // 1 for CHERIoT: do not wait
  cjal eg2_late_rv_bench
  mv   a4, s1
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t1, 1
  beq  a4, t1, 12f
  // ePMP: the bench has answered; its sberror recorded.
  sw   a0, (EG2_LATE_INFO + EG2_SWITCH_INFO_SBA_ERR)(s0)
  j    .Llocked
12:
  li   a3, 100000000
11:
  csrr t0, misa
  andi t0, t0, MISA_E
  bnez t0, .Llocked
  addi a3, a3, -1
  bnez a3, 11b
.Llocked:
  fence.i
  li   t1, 1
  beq  a4, t1, .Lcheriot

  // ePMP mode locked: RISC-V mode stays, whatever is requested now.
  csrr t0, misa
  andi t0, t0, (MISA_E | MISA_I)
  li   t1, MISA_I
  bne  t0, t1, .Lfail_misa1
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  fence.i
  csrr t0, misa
  andi t0, t0, (MISA_E | MISA_I)
#ifdef EG2_SWITCH_INJECT_FAIL
  li   t1, MISA_E
#else
  li   t1, MISA_I
#endif
  bne  t0, t1, .Lfail_misa2
  lw   t0, (EG2_LATE_INFO + EG2_SWITCH_INFO_READBACK)(s0)
  bnez t0, .Lfail_readback
  li   a0, 1
  li   a1, 0
  j    eg2_late_rv_end

.Lcheriot:
  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, .Lfail_misa1
  j    eg2_late_cheriot

.Lfail_meta:
  li   a0, 0
  li   a1, 0x10
  j    eg2_late_rv_end
.Lfail_misa1:
  li   a0, 0
  li   a1, 0x20   // misa after the lock is not the locked mode's
  j    eg2_late_rv_end
.Lfail_misa2:
  li   a0, 0
  li   a1, 0x21   // misa changed by a request after an ePMP lock
  j    eg2_late_rv_end
.Lfail_readback:
  li   a0, 0
  li   a1, 0x22   // a CHERIOT_ENA write did not read back
  j    eg2_late_rv_end
  .size eg2_late_pre, . - eg2_late_pre

  .option pop
