// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Start-up code of the EG2 framework's CHERIoT build: from reset to `eg2_cheriot_main` with
 * CHERIoT mode locked and purecap C ready to run (EG2 spec REQ_STE_02).
 *
 *  1. Switch to CHERIoT mode and lock it, as `cheriot_smoketest.S` does: the trap vector armed
 *     first, CHERIOT_ENA read back before the once-only lock. Any trap from here on ends the
 *     test with `CTRAP <mcause> <mtval> <mepc>` and the failure word in the DV status window.
 *  2. The stack: CSP derived from the memory root (MTDC), bounded to [_stack_start, _stack_end).
 *  3. The globals: CGP derived from the memory root, bounded to the one output section that
 *     holds data and BSS, with its address in the middle of it. That is where the linker
 *     resolves CGP-relative offsets from (the CHERIoT ABI's biased CGP; see ottf_common.ld).
 *     The section's initial image, BSS zeroes included, is copied from NVM.
 *  4. The capability relocations the linker leaves in `__cap_relocs` (pointers held in globals),
 *     one 20-byte entry each: location, base, offset, length, flags. A function pointer is
 *     derived from PCC (bit 31 of the flags), a pointer to constant data from the memory root
 *     without store permission (bit 30), any other from the memory root; all but function
 *     pointers bounded to the object. Function pointers keep PCC's bounds, so the code they
 *     reach can still address its own constants PCC-relatively.
 *  5. Call `eg2_cheriot_main`, which never returns.
 *
 * MTDC keeps the memory root for the whole test: the device library derives its capabilities
 * from it. Only x0-x15 are used, as CHERIoT mode has no others.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "hw/top/uart_regs.h"          // Generated.
#include "sw/device/lib/base/multibits_asm.h"

#define DV_STATUS_FAILED 0xbaad
#define MISA_E (1 << 4)
// Architectural permission bits (CHERIoT ISA, getCapPerms): SD is bit 2.
#define PERM_SD (1 << 2)
#define CAP_RELOC_BYTES 20

// An absolute address into an integer register. Not `la`, which in CHERIoT mode is
// PCC-relative and yields a capability.
.macro la_abs rd, sym
  lui  \rd, %hi(\sym)
  addi \rd, \rd, %lo(\sym)
.endm

// Writes the byte in `reg` to the UART whose capability is `cap`, once its FIFO has room.
// Clobbers t2.
.macro uart_put cap, reg
1:
  lw   t2, UART_STATUS_REG_OFFSET(\cap)
  andi t2, t2, (1 << UART_STATUS_TXFULL_BIT)
  bnez t2, 1b
  sw   \reg, UART_WDATA_REG_OFFSET(\cap)
.endm

.macro uart_putc cap, char
  addi a2, x0, \char
  uart_put \cap, a2
.endm

// Writes `reg` as eight hex digits; `reg` is consumed. Clobbers a1, a2, t2.
.macro uart_puthex cap, reg
  addi a1, x0, 8
1:
  srli a2, \reg, 28
  slli \reg, \reg, 4
  addi t2, x0, 10
  blt  a2, t2, 2f
  addi a2, a2, ('a' - 10 - '0')
2:
  addi a2, a2, '0'
  uart_put \cap, a2
  addi a1, a1, -1
  bnez a1, 1b
.endm

// -----------------------------------------------------------------------------

  .section .vectors, "ax", @progbits
  .option push
  .option norvc
  .option norelax

/**
 * Any trap ends the test. In CHERIoT mode: `CTRAP <mcause> <mtval> <mepc>` on UART0, then the
 * failure word in the DV status window. Still in RISC-V mode (the switch failed): the failure
 * word only.
 */
  .balign 256
  .globl _eg2_cheriot_trap
  .type _eg2_cheriot_trap, @function
_eg2_cheriot_trap:
  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, .L_trap_rv32

  cspecialr       ct0, mtdc
  li              t1, TOP_UART0_BASE_ADDR
  csetaddr        ct0, ct0, t1
  li              t1, TOP_UART0_SIZE_BYTES
  csetboundsexact ct0, ct0, t1
  uart_putc ct0, 'C'
  uart_putc ct0, 'T'
  uart_putc ct0, 'R'
  uart_putc ct0, 'A'
  uart_putc ct0, 'P'
  uart_putc ct0, ' '
  csrr      a0, mcause
  uart_puthex ct0, a0
  uart_putc ct0, ' '
  csrr      a0, mtval
  uart_puthex ct0, a0
  uart_putc ct0, ' '
  cspecialr ca0, mepcc
  uart_puthex ct0, a0
  uart_putc ct0, '\r'
  uart_putc ct0, '\n'

  cspecialr       ct0, mtdc
  li              t1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  csetaddr        ct0, ct0, t1
  li              t1, TOP_RV_CORE_IBEX_CFG_SIZE_BYTES
  csetboundsexact ct0, ct0, t1
  li              t1, DV_STATUS_FAILED
  sw              t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(ct0)
  j               .L_trap_park

.L_trap_rv32:
  li   t0, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t1, DV_STATUS_FAILED
  sw   t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(t0)
.L_trap_park:
  wfi
  j    .L_trap_park
  .size _eg2_cheriot_trap, . - _eg2_cheriot_trap

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

  // 1. The mode switch, with integer addresses while still in RISC-V mode.
  la_abs t0, _eg2_cheriot_trap
  csrw   mtvec, t0
  li     s1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  addi   t0, x0, MULTIBIT_ASM_BOOL4_TRUE
  sw     t0, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  lw     t1, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET(s1)
  bne    t0, t1, .L_no_cheriot
  sw     t0, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET(s1)
  fence.i
  csrr   t0, misa
  andi   t0, t0, MISA_E
  beqz   t0, .L_no_cheriot

  // 2. The stack, from the memory root.
  cspecialr  cs0, mtdc
  la_abs     t0, _stack_start
  la_abs     t1, _stack_end
  csetaddr   csp, cs0, t0
  sub        t1, t1, t0
  csetbounds csp, csp, t1
  cincoffset csp, csp, t1

  // 3. The globals: CGP over [_data_start, _bss_end), the image copied from NVM, then CGP's
  // address moved to the middle. t1 holds the size throughout.
  la_abs     t0, _data_start
  la_abs     t1, _bss_end
  csetaddr   cgp, cs0, t0
  sub        t1, t1, t0
  csetbounds cgp, cgp, t1
  la_abs     a0, _data_init_start
  la_abs     a1, _data_init_end
  csetaddr   ca0, cs0, a0
  cmove      ca2, cgp
.L_copy:
  bgeu       a0, a1, .L_copy_done
  lw         t2, 0(ca0)
  sw         t2, 0(ca2)
  cincoffsetimm ca0, ca0, 4
  cincoffsetimm ca2, ca2, 4
  j          .L_copy
.L_copy_done:
  srli       t1, t1, 1
  cincoffset cgp, cgp, t1

  // 4. The capability relocations. cs1 is PCC, the root of function pointers.
  la_abs     a0, __cap_relocs_start
  la_abs     a1, __cap_relocs_end
  csetaddr   ca0, cs0, a0
  auipcc     cs1, 0
.L_reloc:
  bgeu       a0, a1, .L_reloc_done
  lw         t0, 0(ca0)              // location
  lw         a2, 4(ca0)              // base
  lw         a3, 8(ca0)              // offset
  lw         a4, 12(ca0)             // length
  lw         a5, 16(ca0)             // flags
  bltz       a5, .L_reloc_function
  csetaddr   ct1, cs0, a2
  csetbounds ct1, ct1, a4
  cincoffset ct1, ct1, a3
  slli       a5, a5, 1
  bgez       a5, .L_reloc_store
  li         t2, ~PERM_SD
  candperm   ct1, ct1, t2
  j          .L_reloc_store
.L_reloc_function:
  csetaddr   ct1, cs1, a2
  cincoffset ct1, ct1, a3
.L_reloc_store:
  csetaddr   ct2, cs0, t0
  csc        ct1, 0(ct2)
  cincoffsetimm ca0, ca0, CAP_RELOC_BYTES
  j          .L_reloc
.L_reloc_done:

  // 5. C.
  ccall      eg2_cheriot_main

  // Still in RISC-V mode: the switch did not take. Fail in the DV status window and park.
.L_no_cheriot:
  li   t1, DV_STATUS_FAILED
  sw   t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(s1)
.L_park:
  wfi
  j    .L_park

  .option pop
  .size _ottf_start, . - _ottf_start
