// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Late-switch start-up code (eg2_late.h): `_ottf_start` hands over to the test's `eg2_late_pre`
 * in RISC-V mode; the test switches and continues at `eg2_late_cheriot`, which does what the
 * framework's start-up code does after its switch (eg2/cheriot/eg2_cheriot_start.S steps 2-4:
 * stack, globals, capability relocations) and calls `eg2_late_main`.
 *
 * Helpers for the test's RISC-V-mode assembly (each called with `jal ra`, clobbering a0-a5 and
 * t0-t2 only, and using only encodings both modes share):
 *   eg2_late_meta_init  zero the meta SRAM (sram_ctrl_meta INIT after a new key); a0 = 0 if done
 *   eg2_late_rv_end     end the test in RISC-V mode: a0 = 1 pass, 0 fail, a1 = a code printed
 *                       with a failure (`EG2 late FAIL <code>`); prints the verdict, writes the
 *                       DV status window and parks (never returns)
 *   eg2_late_rv_puthex  print a1 as eight hex digits (UART0 set up by eg2_late_uart_init)
 *   eg2_late_uart_init  UART0's transmitter on (the test ROM leaves it off on sim_dv)
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/sram_ctrl_regs.h"     // Generated.
#include "hw/top/top_memory.h"         // Generated.
#include "hw/top/uart_regs.h"          // Generated.
#include "sw/device/lib/base/multibits_asm.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"

#define DV_STATUS_IN_TEST 0x4354
#define DV_STATUS_PASSED 0x900d
#define DV_STATUS_FAILED 0xbaad
#define MISA_E (1 << 4)
#define PERM_SD (1 << 2)
#define CAP_RELOC_BYTES 20

.macro la_abs rd, sym
  lui  \rd, %hi(\sym)
  addi \rd, \rd, %lo(\sym)
.endm

// A byte to UART0 at integer address in `base` (RISC-V mode) or capability (CHERIoT mode).
.macro uart_put base, reg
1:
  lw   t2, UART_STATUS_REG_OFFSET(\base)
  andi t2, t2, (1 << UART_STATUS_TXFULL_BIT)
  bnez t2, 1b
  sw   \reg, UART_WDATA_REG_OFFSET(\base)
.endm

.macro uart_putc base, char
  addi a2, x0, \char
  uart_put \base, a2
.endm

.macro uart_puthex base, reg
  addi a3, x0, 8
7:
  srli a2, \reg, 28
  slli \reg, \reg, 4
  addi t2, x0, 10
  blt  a2, t2, 8f
  addi a2, a2, ('a' - 10 - '0')
8:
  addi a2, a2, '0'
  uart_put \base, a2
  addi a3, a3, -1
  bnez a3, 7b
.endm

// -----------------------------------------------------------------------------

  .section .vectors, "ax", @progbits
  .option push
  .option norvc
  .option norelax

/**
 * A trap the test did not arrange ends it, as in the framework's start-up code: CHERIoT mode
 * `CTRAP <mcause> <mtval> <mepc>` on UART0; RISC-V mode `RTRAP <mcause> <mtval> <mepc>`; then the
 * failure word in the DV status window.
 */
  .balign 256
  .globl _eg2_late_trap
  .type _eg2_late_trap, @function
_eg2_late_trap:
  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, .L_trap_rv32

  cspecialr       ct0, mtdc
  li              t1, TOP_UART0_BASE_ADDR
  csetaddr        ct0, ct0, t1
  li              t1, TOP_UART0_SIZE_BYTES
  csetboundsexact ct0, ct0, t1
  uart_putc ct0, 'C'
  j .L_trap_body_c
.L_trap_rv32:
  li   t0, TOP_UART0_BASE_ADDR
  uart_putc t0, 'R'
.L_trap_body_c:
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
  csrr      a0, mepc
  uart_puthex ct0, a0
  uart_putc ct0, '\r'
  uart_putc ct0, '\n'
1:
  lw   t2, UART_STATUS_REG_OFFSET(ct0)
  andi t2, t2, (1 << UART_STATUS_TXIDLE_BIT)
  beqz t2, 1b

  csrr t0, misa
  andi t0, t0, MISA_E
  beqz t0, .L_trap_rv32_status
  cspecialr       ct0, mtdc
  li              t1, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  csetaddr        ct0, ct0, t1
  li              t1, DV_STATUS_FAILED
  sw              t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(ct0)
  j               .L_trap_park
.L_trap_rv32_status:
  li   t0, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t1, DV_STATUS_FAILED
  sw   t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(t0)
.L_trap_park:
  wfi
  j    .L_trap_park
  .size _eg2_late_trap, . - _eg2_late_trap

  // The probes (eg2_probe.S) hand a trap they did not expect to the framework's handler by this
  // name.
  .globl _eg2_cheriot_trap
  .set _eg2_cheriot_trap, _eg2_late_trap

  .option pop

// -----------------------------------------------------------------------------

  .section .crt, "ax", @progbits
  .balign 4
  .option push
  .option norvc
  .option norelax

  .globl _ottf_start
  .type _ottf_start, @function
_ottf_start:
  la_abs t0, _eg2_late_trap
  csrw   mtvec, t0
  j      eg2_late_pre
  .size _ottf_start, . - _ottf_start

/** Zeroes the meta SRAM: a new key, then INIT (sram_ctrl_meta is built with SecZeroInit). */
  .globl eg2_late_meta_init
  .type eg2_late_meta_init, @function
eg2_late_meta_init:
  li     t0, TOP_SRAM_CTRL_META_REGS_BASE_ADDR
  addi   t1, x0, (1 << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT) | (1 << SRAM_CTRL_CTRL_INIT_BIT)
  sw     t1, SRAM_CTRL_CTRL_REG_OFFSET(t0)
1:
  lw     t1, SRAM_CTRL_STATUS_REG_OFFSET(t0)
  andi   t2, t1, (1 << SRAM_CTRL_STATUS_INIT_ERROR_BIT)
  bnez   t2, 2f
  andi   t2, t1, (1 << SRAM_CTRL_STATUS_INIT_DONE_BIT)
  beqz   t2, 1b
  addi   a0, x0, 0
  cret
2:
  addi   a0, x0, 1
  cret
  .size eg2_late_meta_init, . - eg2_late_meta_init

/** UART0's transmitter on, at the platform's NCO value (eg2_console_init). */
  .globl eg2_late_uart_init
  .type eg2_late_uart_init, @function
eg2_late_uart_init:
  la_abs t0, kUartNCOValue
  lw     t1, 0(t0)
  li     t2, UART_CTRL_NCO_MASK
  and    t1, t1, t2
  slli   t1, t1, UART_CTRL_NCO_OFFSET
  ori    t1, t1, (1 << UART_CTRL_TX_BIT)
  li     t0, TOP_UART0_BASE_ADDR
  sw     t1, UART_CTRL_REG_OFFSET(t0)
  cret
  .size eg2_late_uart_init, . - eg2_late_uart_init

/** a1 as eight hex digits on UART0 (RISC-V mode). */
  .globl eg2_late_rv_puthex
  .type eg2_late_rv_puthex, @function
eg2_late_rv_puthex:
  li   t0, TOP_UART0_BASE_ADDR
  uart_puthex t0, a1
  cret
  .size eg2_late_rv_puthex, . - eg2_late_rv_puthex

/** The NUL-terminated string at integer address a1 on UART0 (RISC-V mode). Clobbers a1, a2, t0-t2. */
  .globl eg2_late_rv_puts
  .type eg2_late_rv_puts, @function
eg2_late_rv_puts:
  li   t0, TOP_UART0_BASE_ADDR
5:
  lbu  a2, 0(a1)
  beqz a2, 6f
  uart_put t0, a2
  addi a1, a1, 1
  j    5b
6:
  cret
  .size eg2_late_rv_puts, . - eg2_late_rv_puts

/**
 * A bench step from RISC-V mode (eg2_memsys.h's protocol): prints
 * `EG2 BENCH x<n> <op> <a1> <a2>` (n in hex, after an `x`), then waits until the bench has written
 * n into kEg2BenchAck[0]; returns kEg2BenchAck[1] in a0, or 0xffffffff if the bench did not
 * answer. a0 = the op's string (integer address), a1, a2 = its arguments; a3 = 1: return once the
 * line is out, without waiting (the bench's step may switch the mode under the caller). Clobbers
 * a0-a5 and t0-t2 (ra is kept in a5 meanwhile), and the capture area's last four words.
 */
  .globl eg2_late_rv_bench
  .type eg2_late_rv_bench, @function
eg2_late_rv_bench:
  mv   a5, ra
  mv   a4, a0
  // The arguments wait in the capture area's last words (no stack yet).
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK + EG2_LATE_AREA_SIZE - 8)
  sw   a1, 0(t0)
  sw   a2, 4(t0)
  sw   a3, -8(t0)
  cjal eg2_late_uart_init
  la_abs a1, .L_bench_prefix
  cjal eg2_late_rv_puts
  la_abs t0, kEg2BenchAck
  lw   a1, 0(t0)
  addi a1, a1, 1
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK + EG2_LATE_AREA_SIZE - 12)
  sw   a1, 0(t0)
  cjal eg2_late_rv_puthex
  la_abs a1, .L_space
  cjal eg2_late_rv_puts
  mv   a1, a4
  cjal eg2_late_rv_puts
  la_abs a1, .L_space
  cjal eg2_late_rv_puts
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK + EG2_LATE_AREA_SIZE - 8)
  lw   a1, 0(t0)
  cjal eg2_late_rv_puthex
  la_abs a1, .L_space
  cjal eg2_late_rv_puts
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK + EG2_LATE_AREA_SIZE - 4)
  lw   a1, 0(t0)
  cjal eg2_late_rv_puthex
  la_abs a1, .L_crlf
  cjal eg2_late_rv_puts
  la_abs t0, (_stack_start - EG2_LATE_AREA_BELOW_STACK + EG2_LATE_AREA_SIZE - 12)
  lw   a4, 0(t0)
  lw   a3, -4(t0)
  beqz a3, 2f
  li   a0, 0
  mv   ra, a5
  cret
2:
  la_abs t0, kEg2BenchAck
  li   a3, 20000000
3:
  lw   t1, 0(t0)
  beq  t1, a4, 4f
  addi a3, a3, -1
  bnez a3, 3b
  li   a0, -1
  mv   ra, a5
  cret
4:
  lw   a0, 4(t0)
  mv   ra, a5
  cret
  .size eg2_late_rv_bench, . - eg2_late_rv_bench

  .section .rodata
.L_bench_prefix:
  .asciz "EG2 BENCH x"
.L_space:
  .asciz " "
.L_crlf:
  .asciz "\r\n"
  .section .crt, "ax", @progbits

/** The verdict in RISC-V mode: a0 = 1 pass, 0 fail; a1 = the failure's code. Never returns. */
  .globl eg2_late_rv_end
  .type eg2_late_rv_end, @function
eg2_late_rv_end:
  mv   a4, a0
  mv   a5, a1
  cjal eg2_late_uart_init
  li   t0, TOP_UART0_BASE_ADDR
  bnez a4, 3f
  uart_putc t0, 'E'
  uart_putc t0, 'G'
  uart_putc t0, '2'
  uart_putc t0, ' '
  uart_putc t0, 'l'
  uart_putc t0, 'a'
  uart_putc t0, 't'
  uart_putc t0, 'e'
  uart_putc t0, ' '
  uart_putc t0, 'F'
  uart_putc t0, 'A'
  uart_putc t0, 'I'
  uart_putc t0, 'L'
  uart_putc t0, ' '
  mv   a1, a5
  uart_puthex t0, a1
  uart_putc t0, '\r'
  uart_putc t0, '\n'
  uart_putc t0, 'F'
  uart_putc t0, 'A'
  uart_putc t0, 'I'
  uart_putc t0, 'L'
  j    4f
3:
  uart_putc t0, 'P'
  uart_putc t0, 'A'
  uart_putc t0, 'S'
  uart_putc t0, 'S'
4:
  uart_putc t0, '!'
  uart_putc t0, '\r'
  uart_putc t0, '\n'
5:
  lw   t2, UART_STATUS_REG_OFFSET(t0)
  andi t2, t2, (1 << UART_STATUS_TXIDLE_BIT)
  beqz t2, 5b
  li   t0, TOP_RV_CORE_IBEX_CFG_BASE_ADDR
  li   t1, DV_STATUS_IN_TEST
  sw   t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(t0)
  li   t1, DV_STATUS_FAILED
  beqz a4, 6f
  li   t1, DV_STATUS_PASSED
6:
  sw   t1, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET(t0)
7:
  wfi
  j    7b
  .size eg2_late_rv_end, . - eg2_late_rv_end

/**
 * CHERIoT mode is locked; MTDC holds the memory root. The framework's steps 2-4, then C.
 */
  .globl eg2_late_cheriot
  .type eg2_late_cheriot, @function
eg2_late_cheriot:
  // 2. The stack, from the memory root.
  cspecialr  cs0, mtdc
  la_abs     t0, _stack_start
  la_abs     t1, _stack_end
  csetaddr   csp, cs0, t0
  sub        t1, t1, t0
  csetbounds csp, csp, t1
  cincoffset csp, csp, t1

  // 3. The globals: CGP over [_data_start, _bss_end), the image copied from NVM, CGP's address
  // in the middle.
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

  // 4. The capability relocations.
  la_abs     a0, __cap_relocs_start
  la_abs     a1, __cap_relocs_end
  csetaddr   ca0, cs0, a0
  auipcc     cs1, 0
.L_reloc:
  bgeu       a0, a1, .L_reloc_done
  lw         t0, 0(ca0)
  lw         a2, 4(ca0)
  lw         a3, 8(ca0)
  lw         a4, 12(ca0)
  lw         a5, 16(ca0)
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
  ccall      eg2_late_main
1:
  wfi
  j          1b
  .size eg2_late_cheriot, . - eg2_late_cheriot

  .option pop
