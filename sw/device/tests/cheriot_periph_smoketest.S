// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Reaches every register window of the chip through the mode's own pointers, and in CHERIoT mode
 * checks that a device capability confines its holder to that window.
 *
 * EG2 verification spec REQ_PER_01 and REQ_PER_02. The windows come from
 * `cheriot_periph_table.h` (generated from the top description and each IP's registers): one
 * entry per register interface, with a register the test may write back.
 *
 * Per window, in both modes:
 *   - read the chosen register;
 *   - if it has a write-back mask (INTR_ENABLE): write the value with every masked bit flipped,
 *     read it back, restore the original and read that back.
 * In CHERIoT mode, before that, the pointer is a capability derived from the memory root
 * (`mtdc`) with exact bounds: its tag, base and length are checked. After it:
 *   - a load one byte past the window must trap with a CHERI bounds violation;
 *   - a store through a copy without store permission (SD) must trap with a store-permission
 *     violation, and the register must still hold its original value.
 *
 * `CHERIOT_MODE` picks the mode as in `cheriot_smoketest.S`, whose mode switch this follows.
 * Build with `-DCHERIOT_PERIPH_INJECT_FAIL` to expect the wrong cause for the bounds check: the
 * run must then fail with `FAIL b` (the check is live).
 *
 * Only x0-x15 are used, as CHERIoT mode has no others.
 *   s0 UART0      s1 rv_core_ibex cfg   a0 table cursor   a1 entry index
 *   a2 window     a3 window size        a4 register offset a5 write-back mask
 *   sp original register value          t0-t2 scratch (the trap handler uses t0 too)
 *   tp 1 while a trap is expected; the handler clears it and leaves mcause in ra, mtval in gp
 */

#include "sw/device/lib/base/multibits_asm.h"
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "hw/top/uart_regs.h"          // Generated.
#include "sw/device/tests/cheriot_periph_table.h"  // Generated.

#if !defined(CHERIOT_MODE)
#error "CHERIOT_MODE must be 1 (boot into CHERIoT) or 0 (stay in ePMP)."
#endif

#define DV_STATUS_IN_TEST 0x4354
#define DV_STATUS_PASSED 0x900d
#define DV_STATUS_FAILED 0xbaad

#define MISA_E (1 << 4)
#define MISA_I (1 << 8)
#define MISA_MODE (MISA_E | MISA_I)

// CHERIoT ISA: the CHERI exception's `mcause`, and the cause codes in `mtval[4:0]`.
#define MCAUSE_CHERI 28
#define CAPEX_BOUNDS 0x01
#define CAPEX_PERMIT_STORE 0x13
#define CAPEX_CAUSE_MASK 0x1f
// Architectural permission bits (CHERIoT ISA, getCapPerms): SD is bit 2.
#define PERM_SD (1 << 2)

#if defined(CHERIOT_PERIPH_INJECT_FAIL)
#define CAPEX_BOUNDS_EXPECTED CAPEX_PERMIT_STORE
#else
#define CAPEX_BOUNDS_EXPECTED CAPEX_BOUNDS
#endif

#define ENTRY_BYTES 16

#if CHERIOT_MODE
#define ENA_SELECTED MULTIBIT_ASM_BOOL4_TRUE
#define MISA_MODE_EXPECTED MISA_E
#define PASS_MSG "CHERIoT periph PASS!\r\n"
#else
#define ENA_SELECTED MULTIBIT_ASM_BOOL4_FALSE
#define MISA_MODE_EXPECTED MISA_I
#define PASS_MSG "ePMP periph PASS!\r\n"
#endif

// -----------------------------------------------------------------------------

/**
 * Trap handler. An expected trap (tp != 0) records `mcause` and `mtval`, clears tp and resumes
 * after the faulting instruction, which is four bytes (`norvc`). Anything else is reported as
 * `[RC]TRAP <mcause>` and parks the core, as in `cheriot_smoketest.S`.
 */
  .section .vectors, "ax", @progbits
  .option push
  .option norvc
  .option norelax

  .balign 256
  .globl _trap_handler
  .type _trap_handler, @function
_trap_handler:
  beqz tp, .L_trap_unexpected
  csrr ra, mcause
  csrr gp, mtval
  addi tp, x0, 0
  csrr t0, misa
  andi t0, t0, MISA_E
  bnez t0, .L_trap_resume_cheriot
  csrr t0, mepc
  addi t0, t0, 4
  csrw mepc, t0
  mret
.L_trap_resume_cheriot:
  cspecialr     ct0, mepcc
  cincoffsetimm ct0, ct0, 4
  cspecialw     mepcc, ct0
  mret

.L_trap_unexpected:
  csrr t0, misa
  andi t0, t0, MISA_E
  bnez t0, .L_trap_cheriot
  lui  t1, %hi(TOP_UART0_BASE_ADDR)
  addi t2, x0, 'R'
  j    .L_trap_mark
.L_trap_cheriot:
  cspecialr     t1, mtdc
  lui           t2, %hi(TOP_UART0_BASE_ADDR)
  csetaddr      t1, t1, t2
  csetboundsimm t1, t1, 0x40
  addi          t2, x0, 'C'
.L_trap_mark:
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, 'T'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, 'R'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, 'A'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, 'P'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, ' '
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  csrr s0, mcause
  addi s1, x0, 8
.L_trap_nibble:
  srli a0, s0, 28
  slli s0, s0, 4
  addi a1, x0, 10
  blt  a0, a1, .L_trap_digit
  addi a0, a0, ('a' - 10 - '0')
.L_trap_digit:
  addi a0, a0, '0'
  sw   a0, UART_WDATA_REG_OFFSET(t1)
  addi s1, s1, -1
  bnez s1, .L_trap_nibble
  addi t2, x0, '\r'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
  addi t2, x0, '\n'
  sw   t2, UART_WDATA_REG_OFFSET(t1)
.L_trap_park:
  wfi
  j    .L_trap_park
  .size _trap_handler, . - _trap_handler

  .option pop

// -----------------------------------------------------------------------------

  .section .crt, "ax", @progbits
  .balign 4
  .globl _ottf_start
  .type _ottf_start, @function
_ottf_start:
  .option push
  .option norvc
  .option norelax

  // The mode switch, as in cheriot_smoketest.S: integer bases while still in RV32 mode, the
  // trap handler armed first, CHERIOT_ENA read back before the once-only lock.
  li   s0, TOP_UART0_BASE_ADDR
  li   s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  addi tp, x0, 0
  lui  t0, %hi(_trap_handler)
  addi t0, t0, %lo(_trap_handler)
  csrw mtvec, t0

  addi t0, x0, ENA_SELECTED
  sw   t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  lw   t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  addi t2, x0, 'E'
  bne  t0, t1, _fail
  addi t0, x0, MULTIBIT_ASM_BOOL4_TRUE
  sw   t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  fence.i

#if CHERIOT_MODE
  // MMIO and the table now need capabilities, derived from the memory root.
  cspecialr ct1, mtdc
  li        t2, TOP_UART0_BASE_ADDR
  csetaddr  cs0, ct1, t2
  li        t2, TOP_UART0_SIZE_BYTES
  csetboundsexact cs0, cs0, t2
  li        t2, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  csetaddr  cs1, ct1, t2
  li        t2, TOP_RV_CORE_IBEX_CFG_SIZE_BYTES
  csetboundsexact cs1, cs1, t2
  cllc      ca0, kPeriphTable
#else
  lui  a0, %hi(kPeriphTable)
  addi a0, a0, %lo(kPeriphTable)
#endif

  csrr t0, misa
  andi t0, t0, MISA_MODE
  addi t1, x0, MISA_MODE_EXPECTED
  addi t2, x0, 'M'
  bne  t0, t1, _fail

#ifdef DV_TEST_STATUS
  li   t0, DV_STATUS_IN_TEST
  sw   t0, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(s1)
#endif

  addi a1, x0, 0

.L_next_window:
  addi t0, x0, CHERIOT_PERIPH_COUNT
  beq  a1, t0, .L_all_done
  lw   t0, 0(a0)                   // base
  lw   a3, 4(a0)                   // size
  lw   a4, 8(a0)                   // register offset
  lw   a5, 12(a0)                  // write-back mask

#if CHERIOT_MODE
  // A capability for exactly this window: tag kept (exact bounds), base and length as asked.
  cspecialr ca2, mtdc
  csetaddr  ca2, ca2, t0
  csetboundsexact ca2, ca2, a3
  cgettag   t1, ca2
  addi      t2, x0, 'X'
  beqz      t1, _fail
  cgetbase  t1, ca2
  addi      t2, x0, 'B'
  bne       t1, t0, _fail
  cgetlen   t1, ca2
  bne       t1, a3, _fail
  cincoffset ca2, ca2, a4
#else
  add  a2, t0, a4
#endif

  // Read; if the window has a write-back register, flip, read back, restore, read back.
  lw   sp, 0(a2)
  beqz a5, .L_rw_done
  xor  t1, sp, a5
  sw   t1, 0(a2)
  lw   t0, 0(a2)
  and  t0, t0, a5
  and  t1, t1, a5
  addi t2, x0, 'W'
  bne  t0, t1, _fail
  sw   sp, 0(a2)
  lw   t0, 0(a2)
  and  t0, t0, a5
  and  t1, sp, a5
  addi t2, x0, 'R'
  bne  t0, t1, _fail
.L_rw_done:

#if CHERIOT_MODE
  // One byte past the window: a bounds violation before the bus.
  sub        t1, a3, a4
  cincoffset ct2, ca2, t1
  addi       tp, x0, 1
  lw         t1, 0(ct2)
  addi       t2, x0, 'F'
  bnez       tp, _fail
  addi       t1, x0, MCAUSE_CHERI
  addi       t2, x0, 'b'
  bne        ra, t1, _fail
  andi       t0, gp, CAPEX_CAUSE_MASK
  addi       t1, x0, CAPEX_BOUNDS_EXPECTED
  bne        t0, t1, _fail

  // A store through a copy without store permission: a store-permission violation, and the
  // register keeps its value. A write-back register gets the flipped value, so a store that
  // got through would show.
  li         t1, ~PERM_SD
  candperm   ct2, ca2, t1
  xor        t1, sp, a5
  addi       tp, x0, 1
  sw         t1, 0(ct2)
  addi       t2, x0, 'P'
  bnez       tp, _fail
  addi       t1, x0, MCAUSE_CHERI
  addi       t2, x0, 'p'
  bne        ra, t1, _fail
  andi       t0, gp, CAPEX_CAUSE_MASK
  addi       t1, x0, CAPEX_PERMIT_STORE
  bne        t0, t1, _fail
  beqz       a5, .L_perm_done
  lw         t0, 0(a2)
  and        t0, t0, a5
  and        t1, sp, a5
  addi       t2, x0, 'C'
  bne        t0, t1, _fail
.L_perm_done:
  cincoffsetimm ca0, ca0, ENTRY_BYTES
#else
  addi a0, a0, ENTRY_BYTES
#endif
  addi a1, a1, 1
  j    .L_next_window

.L_all_done:
#if CHERIOT_MODE
  cllc     ca2, kPassMsg
#else
  lui      a2, %hi(kPassMsg)
  addi     a2, a2, %lo(kPassMsg)
#endif
.L_tx_next:
  lbu      a4, 0(a2)
  beqz     a4, .L_tx_done
#if CHERIOT_MODE
  cincoffsetimm ca2, ca2, 1
#else
  addi     a2, a2, 1
#endif
.L_tx_wait:
  lw       a5, UART_STATUS_REG_OFFSET(s0)
  andi     a5, a5, (1 << UART_STATUS_TXFULL_BIT)
  bnez     a5, .L_tx_wait
  sw       a4, UART_WDATA_REG_OFFSET(s0)
  j        .L_tx_next
.L_tx_done:
#ifdef DV_TEST_STATUS
  li   t0, DV_STATUS_PASSED
  sw   t0, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(s1)
#endif
.L_park:
  wfi
  j        .L_park

  .option pop
  .size _ottf_start, . - _ottf_start

// -----------------------------------------------------------------------------

/**
 * Failure reporter: `FAIL <letter> <index>`, the index of the window (two hex digits, the
 * table's order) and the letter of the check:
 *
 *   E  CHERIOT_ENA did not read back as written      M  wrong mode after the switch
 *   X  the window's capability lost its tag          B  its base or length is not the window
 *   W  the flipped value did not read back           R  the restored value did not read back
 *   F  a load past the window did not trap           b  it trapped, not with a bounds violation
 *   P  a store without SD did not trap               p  it trapped, not with a store violation
 *   C  the register changed although the store trapped
 */
  .balign 4
  .type _fail, @function
_fail:
  .option push
  .option norvc
  .option norelax
  addi t0, x0, 'F'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, 'A'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, 'I'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, 'L'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, ' '
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  sw   t2, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, ' '
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  srli t0, a1, 4
  andi t0, t0, 0xf
  addi t1, x0, 10
  blt  t0, t1, 1f
  addi t0, t0, ('a' - 10 - '0')
1:
  addi t0, t0, '0'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  andi t0, a1, 0xf
  blt  t0, t1, 2f
  addi t0, t0, ('a' - 10 - '0')
2:
  addi t0, t0, '0'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, '\r'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
  addi t0, x0, '\n'
  sw   t0, UART_WDATA_REG_OFFSET(s0)
#ifdef DV_TEST_STATUS
  li   t0, DV_STATUS_FAILED
  sw   t0, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(s1)
#endif
.L_fail_park:
  wfi
  j    .L_fail_park
  .option pop
  .size _fail, . - _fail

// -----------------------------------------------------------------------------

  .section .rodata, "a", @progbits
  .balign 4
#define CHERIOT_PERIPH_ENTRY(base, size, offset, mask) .word base, size, offset, mask;
kPeriphTable:
  CHERIOT_PERIPH_TABLE(CHERIOT_PERIPH_ENTRY)
  .size kPeriphTable, . - kPeriphTable

kPassMsg:
  .asciz PASS_MSG
