// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_secure_boot (EG2 verification spec REQ_BOOT_02, "Secure Boot", "Debug and
 * boot"): secure boot ends in CHERIoT mode. The ROM verifies ROM_EXT, ROM_EXT verifies this owner
 * image (signed with the fake owner key, sim_dv_rom_ext), and the owner image switches to CHERIoT
 * mode and locks it (its start-up code, eg2_cheriot_start.S). Built for the CHERIoT platform only:
 * the ROM and ROM_EXT run in RISC-V mode before it.
 *
 * Checks:
 *  - ROM_EXT ran and booted this image from slot A (the boot log, retention SRAM);
 *  - the core runs in CHERIoT mode (misa: RV32E, CHERIoT ISA) and the mode is locked: a second
 *    lock with CHERIOT_ENA = MuBi4False changes nothing (the status module repeats this at the
 *    end);
 *  - PCC is a tagged capability (the image runs as CHERIoT code).
 * The scoreboard checks the ROM and ROM_EXT in RISC-V mode (the instruction check).
 *
 * This image, with its signature, its digest or its key id corrupted at ROM_EXT or at itself, is
 * also the image of chip_sw_cheriot_secure_boot_bad_sig_*, where it must never run (the bench
 * checks the boot failure code and that the mode stays RISC-V).
 *
 * Built with EG2_CHERIOT_SECURE_BOOT_INJECT_FAIL the image is expected in slot B: the run must
 * fail with `EG2CHK FAIL boot_log bl0_slot`.
 */

#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"

EG2_TEST_CONFIG();

enum {
  kMisaE = 1u << 4,
  kMisaI = 1u << 8,
};

static uint32_t misa(void) {
  uint32_t v;
  __asm__ volatile("csrr %0, misa" : "=r"(v));
  return v;
}

bool eg2_test_main(void) {
#ifdef EG2_CHERIOT_SECURE_BOOT_INJECT_FAIL
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotB);
#else
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
#endif
  ok = eg2_boot_check("misa: CHERIoT mode (E, not I)", kMisaE, misa() & (kMisaE | kMisaI)) && ok;
#ifdef __CHERIOT__
  void *pcc;
  __asm__ volatile("auipcc %0, 0" : "=C"(pcc));
  ok = eg2_boot_check("PCC tagged", 1, __builtin_cheri_tag_get(pcc)) && ok;
#endif
  // A second lock, for RISC-V mode, must change nothing (REQ_SOC_03).
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET, kMultiBitBool4False);
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, kMultiBitBool4True);
  __asm__ volatile("fence.i");
  ok = eg2_boot_check("mode still CHERIoT after a second lock", kMisaE,
                      misa() & (kMisaE | kMisaI)) &&
       ok;
  return eg2_boot_summary() && ok;
}
