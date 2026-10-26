// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The main SRAM half of chip_sw_sram_ctrl_scrambled_access_cheriot
 * (eg2_sram_scrambled_access_cheriot.c): sram_ctrl_scrambled_access_test.c's `main_sram_scramble`
 * in purecap CHERIoT code. A new main SRAM key turns the stack and the globals into noise, so from
 * the renewal on nothing here touches memory but through the capabilities it was given in
 * registers, and it never returns: it ends in a software reset, as the OpenTitan test does.
 *
 * void eg2_scr_main_rekey(volatile uint32_t *sram_regs, volatile uint32_t *src,
 *                         volatile uint32_t *dst, uint32_t words, volatile uint32_t *rstmgr);
 *   ca0 sram_ctrl_main's registers, ca1 the frame in main SRAM, ca2 its copy in retention SRAM,
 *   a3 the words to copy, ca4 rstmgr's registers.
 *
 *  1. CTRL.RENEW_SCR_KEY, then STATUS.SCR_KEY_VALID polled.
 *  2. `words` words copied from src to dst; each load is followed by two `nop`s, so the internal
 *     interrupt of a load integrity error (an NMI, vector offset 0x7c) comes before the next
 *     load. The trap vector here counts each such interrupt in s0 and resumes; a synchronous
 *     trap (a load access fault) is counted in s1 and its instruction skipped.
 *  3. The two counts after the copy (dst[words], dst[words + 1]); then RESET_REQ = MuBi4True.
 * Interrupts must be off. Only x0-x15 are used, as CHERIoT mode has no others.
 */

#include "hw/top/rstmgr_regs.h"     // Generated.
#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "sw/device/lib/base/multibits_asm.h"

  .option norvc
  .option norelax

#ifdef __CHERIOT__

  .section .text.eg2_scr_main_rekey, "ax", @progbits
  .balign 256
  // Every synchronous trap (offset 0) and every interrupt, the core's internal ones included,
  // reach the handler.
.Lscr_trap:
  .rept 32
  j          .Lscr_handler
  .endr
.Lscr_handler:
  csrr       a5, mcause
  bltz       a5, .Lscr_irq
  // A synchronous trap: counted, its instruction (4 bytes, no compressed code here) skipped.
  addi       s1, s1, 1
  cspecialr  ct2, mepcc
  cincoffsetimm ct2, ct2, 4
  cspecialw  mepcc, ct2
  mret
.Lscr_irq:
  // The internal interrupt of a load integrity error: counted, resumed.
  addi       s0, s0, 1
  mret

  .balign 4
  .globl eg2_scr_main_rekey
  .type eg2_scr_main_rekey, @function
eg2_scr_main_rekey:
  cllc       ct0, .Lscr_trap
  cspecialw  mtcc, ct0
  addi       s0, x0, 0
  addi       s1, x0, 0
  // 1. The new key.
  addi       t0, x0, (1 << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT)
  sw         t0, SRAM_CTRL_CTRL_REG_OFFSET(ca0)
.Lscr_wait:
  lw         t0, SRAM_CTRL_STATUS_REG_OFFSET(ca0)
  andi       t0, t0, (1 << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)
  beqz       t0, .Lscr_wait
  // 2. The copy.
  mv         t1, a3
.Lscr_copy:
  lw         t0, 0(ca1)
  nop
  nop
  sw         t0, 0(ca2)
  cincoffsetimm ca1, ca1, 4
  cincoffsetimm ca2, ca2, 4
  addi       t1, t1, -1
  bnez       t1, .Lscr_copy
  // 3. The counts, then the software reset.
  sw         s0, 0(ca2)
  sw         s1, 4(ca2)
  lw         t0, 4(ca2)               // read back: the stores have landed
  addi       t0, x0, MULTIBIT_ASM_BOOL4_TRUE
  sw         t0, RSTMGR_RESET_REQ_REG_OFFSET(ca4)
.Lscr_park:
  wfi
  j          .Lscr_park
  .size eg2_scr_main_rekey, . - eg2_scr_main_rekey

#endif  // __CHERIOT__
