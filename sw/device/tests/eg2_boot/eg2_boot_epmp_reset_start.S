// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_epmp_reset (EG2 verification spec REQ_BOOT_05): the probes, run as the boot
 * ROM from reset, before anything changes the ePMP.
 *
 * This image is the ROM (kind "rom", the test ROM's linker script). Its first 2 KiB are all the
 * reset ePMP lets the core fetch (ibex_pmp_reset_pkg region 2, the ROM's `_epmp_reset_rx_size`),
 * so the vectors, the probes and their trap handler come first, in `.vectors` and `.crt`, and use
 * registers only: at reset no RAM is accessible. Each probe records its mcause (0: no trap) in one
 * byte of s2-s5. Then the ePMP is opened (entry 15: the whole address space, as the test ROM's
 * start-up does), the C runtime is set up, and `eg2_epmp_reset_report` (eg2_boot_epmp_reset.c)
 * compares the bytes with what the documents allow and reports.
 *
 * A fetch probe jumps to its target; a fetch the ePMP allowed would run whatever is there (RRAM
 * and SRAM hold zeros or random data at this point) and most likely trap with another cause,
 * which the report flags.
 */

#include "hw/top/top_memory.h"

  .option push
  .option norvc
  .option norelax

  .section .vectors, "ax"
  .balign 256
  .global _eg2_epmp_vectors
  .type _eg2_epmp_vectors, @function
_eg2_epmp_vectors:
  // Ibex: vectored mtvec; at reset mtvec is the boot address, exceptions enter at entry 0.
  .rept 32
  j _eg2_epmp_trap
  .endr
  // The reset vector (boot address + 0x80).
  j _reset_start
  .size _eg2_epmp_vectors, .-_eg2_epmp_vectors

  .section .crt, "ax"
  .balign 4
  .global _eg2_epmp_trap
  .type _eg2_epmp_trap, @function
_eg2_epmp_trap:
  csrr a5, mcause
  csrw mepc, t2
  mret
  .size _eg2_epmp_trap, .-_eg2_epmp_trap

// One probe: `kind` 0 fetch, 1 load, 2 store at `addr`; its mcause into byte `byte` of `reg`.
.macro PROBE reg, byte, kind, addr
  li   t0, \addr
  la   t2, 9f
  li   a5, 0
  .if \kind == 0
  jalr x0, 0(t0)
  .elseif \kind == 1
  lw   a4, 0(t0)
  .else
  sw   x0, 0(t0)
  .endif
9:
  andi a5, a5, 0xff
  slli a5, a5, (8 * \byte)
  or   \reg, \reg, a5
.endm

  .balign 4
  .global _reset_start
  .type _reset_start, @function
_reset_start:
  li s2, 0
  li s3, 0
  li s4, 0
  li s5, 0
  // RRAM (no reset entry: MMWP denies everything).
  PROBE s2, 0, 0, TOP_RRAM_CTRL_HOST_BASE_ADDR
  PROBE s2, 1, 1, TOP_RRAM_CTRL_HOST_BASE_ADDR
  PROBE s2, 2, 2, TOP_RRAM_CTRL_HOST_BASE_ADDR
  // Main SRAM (no reset entry).
  PROBE s2, 3, 0, TOP_SRAM_CTRL_MAIN_RAM_BASE_ADDR
  PROBE s3, 0, 1, TOP_SRAM_CTRL_MAIN_RAM_BASE_ADDR
  PROBE s3, 1, 2, TOP_SRAM_CTRL_MAIN_RAM_BASE_ADDR
  // A device window inside the reset MMIO entry (0x4000_0000 - 0x4201_0000, read-write, no
  // execute): UART0's CTRL register.
  PROBE s3, 2, 0, (TOP_UART0_BASE_ADDR + 0x10)
  PROBE s3, 3, 1, (TOP_UART0_BASE_ADDR + 0x10)
  PROBE s4, 0, 2, (TOP_UART0_BASE_ADDR + 0x10)
  // A device window above the reset MMIO entry: rv_plic.
  PROBE s4, 1, 0, TOP_RV_PLIC_BASE_ADDR
  PROBE s4, 2, 1, TOP_RV_PLIC_BASE_ADDR
  PROBE s4, 3, 2, TOP_RV_PLIC_BASE_ADDR
  // The ROM past its first 2 KiB (the reset ROM entry is 2 KiB, read and execute).
  PROBE s5, 0, 0, (TOP_ROM_CTRL_ROM_BASE_ADDR + 0x800)
  PROBE s5, 1, 1, (TOP_ROM_CTRL_ROM_BASE_ADDR + 0x800)

  // Open the ePMP for the report (entry 15: locked NAPOT RWX over the whole address space; RLB is
  // set at reset, so the locked reset entries can be cleared).
  li   t0, (0x9f << 24)
  csrw pmpcfg3, t0
  li   t0, 0x7fffffff
  csrw pmpaddr15, t0
  csrw pmpcfg0, zero
  csrw pmpcfg1, zero
  csrw pmpcfg2, zero

  .option push
  .option norelax
  la   gp, __global_pointer$
  .option pop
  la   sp, _stack_end
  la   a0, _bss_start
  la   a1, _bss_end
  call crt_section_clear
  la   a0, _data_start
  la   a1, _data_end
  la   a2, _data_init_start
  call crt_section_copy
  mv   a0, s2
  mv   a1, s3
  mv   a2, s4
  mv   a3, s5
  call eg2_epmp_reset_report
.L_park:
  wfi
  j    .L_park
  .size _reset_start, .-_reset_start

  .option pop
