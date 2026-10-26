// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * OTTF's C entry in CHERIoT mode (eg2_top_verification, EG2 spec REQ_STE_02).
 *
 * The start-up code is the EG2 framework's (`sw/device/lib/testing/eg2/cheriot/
 * eg2_cheriot_start.S`): it zeroes the meta SRAM, switches to CHERIoT mode and
 * locks it, sets up CSP and the biased CGP, copies the globals and applies the
 * capability relocations, keeps the memory root in MTDC (from which every MMIO
 * capability is derived, `eg2_mmio.c`), then calls `eg2_cheriot_main`. Until
 * here any trap ends the test (`CTRAP <mcause> <mtval> <mepc>`). This file
 * provides that entry for OTTF: it points MTCC at OTTF's CHERIoT trap entry
 * (`ottf_isrs_cheriot.S`), so interrupts and exceptions reach OTTF's handlers
 * and the test's overrides as in RISC-V mode, then runs OTTF's `_ottf_main`,
 * which runs the test's `test_main` and reports its status.
 */

#include <stdbool.h>
#include <stdint.h>

/**
 * Keeps the globals' output section in the image.
 *
 * The start-up code copies the whole output section that holds the globals, BSS included, from
 * its image in NVM (cheriot/ottf_common.ld): the BSS zeroes are stored in the image. An image has
 * them only if the section holds at least one initialised global; otherwise the linker makes the
 * section NOBITS and the copy would read whatever NVM holds after the image. An OTTF image need
 * not have one (the EG2 framework's always does), so this is it.
 */
static volatile uint32_t ottf_cheriot_data_present = 1;

/** OTTF's C entry (ottf_main.c). */
void _ottf_main(void);

/** Points MTCC at OTTF's CHERIoT trap entry (ottf_isrs_cheriot.S). */
void ottf_cheriot_trap_install(void);

void eg2_cheriot_main(void) __attribute__((noreturn));

void eg2_cheriot_main(void) {
  // Read once, so neither the compiler nor the linker drops it.
  (void)ottf_cheriot_data_present;
  ottf_cheriot_trap_install();
  _ottf_main();
  // `_ottf_main` reports the status and does not return.
  for (;;) {
    __asm__ volatile("wfi");
  }
}
