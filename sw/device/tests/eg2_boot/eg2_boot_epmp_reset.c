// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_boot_epmp_reset (EG2 verification spec REQ_BOOT_05, "Secure boot in RISC-V mode"):
 * at reset only the ROM is executable. The ePMP reset values (ibex_pmp_reset_pkg, MMWP set) allow
 * fetch from the ROM's first 2 KiB and the DV ROM only, read and write of the device range
 * 0x4000_0000 - 0x4201_0000 only, and deny every other access, RRAM and SRAM included.
 *
 * This test is the boot ROM (kind "rom"). Its start-up code (eg2_boot_epmp_reset_start.S) probes,
 * from reset, every target x access below before anything changes the ePMP, then opens it and
 * calls this report. The expected causes come from the reset values above (RISC-V privileged
 * spec: an access the PMP denies faults with instruction / load / store access fault, 1 / 5 / 7).
 * Every target and access is probed in each run: the spec's knobs (target, access) are exhausted,
 * not drawn. The scoreboard (Spike from the same reset values) checks each instruction too.
 *
 * Built with EG2_BOOT_EPMP_RESET_INJECT_FAIL the RRAM fetch is expected to fault as a load: the
 * run must fail with `EG2CHK FAIL RRAM fetch`.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"

void eg2_epmp_reset_report(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);

enum {
  kFetch = 1,
  kLoad = 5,
  kStore = 7,
  kNone = 0,
  kProbes = 14,
};

#ifdef EG2_BOOT_EPMP_RESET_INJECT_FAIL
#define RRAM_FETCH kLoad
#else
#define RRAM_FETCH kFetch
#endif

// In probe order (eg2_boot_epmp_reset_start.S): name and expected mcause.
static const char *const kNames[kProbes] = {
    "RRAM fetch",        "RRAM load",         "RRAM store",          "SRAM fetch",
    "SRAM load",         "SRAM store",        "UART0 fetch",         "UART0 load",
    "UART0 store",       "rv_plic fetch",     "rv_plic load",        "rv_plic store",
    "ROM past 2 KiB fetch", "ROM past 2 KiB load",
};
static const uint8_t kExpected[kProbes] = {
    RRAM_FETCH, kLoad, kStore, kFetch, kLoad, kStore, kFetch,
    kNone,      kNone, kFetch, kLoad,  kStore, kFetch, kLoad,
};

static void dv_status(uint32_t word) {
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET, word);
}

void eg2_epmp_reset_report(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
  const uint32_t regs[4] = {r0, r1, r2, r3};
  eg2_console_init();
  eg2_result_in_test();
  eg2_puts("EG2 ePMP at reset (boot ROM probes)\r\n");
  uint32_t failed = 0;
  for (uint32_t i = 0; i < kProbes; ++i) {
    uint32_t got = (regs[i / 4] >> (8 * (i % 4))) & 0xff;
    eg2_puts(got == kExpected[i] ? "EG2CHK ok " : "EG2CHK FAIL ");
    eg2_puts(kNames[i]);
    eg2_puts(" exp mcause ");
    eg2_putdec(kExpected[i]);
    eg2_puts(" got ");
    eg2_putdec(got);
    eg2_puts("\r\n");
    failed += got == kExpected[i] ? 0 : 1;
  }
  eg2_puts("EG2CHK checks ");
  eg2_putdec(kProbes);
  eg2_puts(", failed ");
  eg2_putdec(failed);
  eg2_puts(failed == 0 ? "\r\nPASS!\r\n" : "\r\nFAIL!\r\n");
  dv_status(failed == 0 ? 0x900d : 0xbaad);
}
