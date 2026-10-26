// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_reset_state and chip_sw_cheriot_meta_init_after_reset, RISC-V-mode part
 * (eg2_late.h). On every boot, before the switch: record what the reset left (CHERIOT_ENA, misa,
 * the reset reason the test ROM saved), then zero the meta SRAM only on the first boot (a power-on
 * reset the bench did not ask for) or with `rely_on_crt_meta_init`; later boots leave it to the
 * boot chain (REQ_CMS_15). Then switch and lock CHERIoT mode as the framework does, and continue
 * in eg2_reset_state.c.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "sw/device/lib/base/multibits_asm.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"
#include "sw/device/tests/eg2_cheriot/eg2_reset_params.h"

#define MISA_E (1 << 4)
// retention_sram_t: version, then creator.reset_reasons (retention_sram.h).
#define RET_RESET_REASONS (TOP_SRAM_CTRL_RET_RAM_BASE_ADDR + 4)

.macro la_abs rd, sym
  lui  \rd, %hi(\sym)
  addi \rd, \rd, %lo(\sym)
.endm

  .section .rodata
  EG2_RESET_PARAMS(EG2_LATE_PARAM_ASM_)

  .text
  .balign 4
  .option push
  .option norvc
  .option norelax

  .globl eg2_late_pre
  .type eg2_late_pre, @function
eg2_late_pre:
  la_abs s0, (_stack_start - EG2_LATE_AREA_BELOW_STACK)
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  lw   t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  sw   t1, (EG2_LATE_INFO + EG2_RESET_INFO_ENA)(s0)
  csrr t1, misa
  sw   t1, (EG2_LATE_INFO + EG2_RESET_INFO_MISA)(s0)
  li   t0, RET_RESET_REASONS
  lw   a0, 0(t0)
  sw   a0, (EG2_LATE_INFO + EG2_RESET_INFO_REASON)(s0)
  li   t1, EG2_RESET_MAGIC
  sw   t1, (EG2_LATE_INFO + EG2_RESET_INFO_MAGIC)(s0)

  // A later boot: not a power-on reset, or one the bench applied (por_done).
  andi a1, a0, 1
  la_abs t0, kEg2Param_por_done
  lw   a2, 0(t0)
  la_abs t0, kEg2Param_rely_on_crt_meta_init
  lw   a3, 0(t0)
  bnez a3, 1f        // the start-up code always initialises
  beqz a1, 2f        // not a power-on reset: a later boot
  bnez a2, 2f        // the bench's power-on reset: a later boot
1:
  cjal eg2_late_meta_init
  bnez a0, 3f
  li   t1, 1
  sw   t1, (EG2_LATE_INFO + EG2_RESET_INFO_META_INIT)(s0)
  j    4f
2:
  sw   x0, (EG2_LATE_INFO + EG2_RESET_INFO_META_INIT)(s0)
4:
  // The switch, as the framework's start-up code makes it.
  li   t0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  lw   t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  bne  t0, t1, 5f
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  fence.i
  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, 6f
  j    eg2_late_cheriot
3:
  li   a0, 0
  li   a1, 0x10   // the meta SRAM's INIT failed
  j    eg2_late_rv_end
5:
  li   a0, 0
  li   a1, 0x11   // CHERIOT_ENA did not read back MuBi4True
  j    eg2_late_rv_end
6:
  li   a0, 0
  li   a1, 0x12   // misa.E clear after the lock
  j    eg2_late_rv_end
  .size eg2_late_pre, . - eg2_late_pre

  .option pop
