// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_wipe_before_switch and _cheriot (EG2 verification spec REQ_SEC_11, "Security in
 * RISC-V mode"): the boot stages clear their secrets before the next stage runs, and in Earl Grey
 * 2 before the CHERIoT switch. Behind the full secure-boot chain (sim_dv_rom_ext).
 *
 * The check is the bench's (chip_sw_eg2_sec_wipe_before_switch_vseq): when this image enters its
 * test (the DV status window's "in test", written after the CHERIoT switch and lock in the
 * `_cheriot` build, and before this image has used any memory in either build), the bench reads
 * the root secrets the hardware holds (the creator root key shares from OTP's SECRET2, the creator
 * and owner seeds as keymgr_dpe receives them from the RRAM controller) and searches main, sec and
 * retention SRAM, descrambled, for any 64-bit run of them. Oracle: identities and root keys
 * "Isolation between boot stages" (ROM_EXT clears the Creator identity seed and private key before
 * the owner's first stage), attestation "Once the ROM_EXT is done using its secrets".
 * A secret that is all zero in this run's image (the seeds, unless the RRAM info pages are
 * provisioned) cannot be searched for: the bench says so.
 *
 * This image checks only that it was booted by ROM_EXT from slot A.
 *
 * Built with EG2_SEC_WIPE_BEFORE_SWITCH_INJECT_FAIL the image is expected in slot B: the run must
 * fail with `EG2CHK FAIL boot_log bl0_slot`; its dvsim entry also sets +eg2_inject_fail=1, which
 * makes the bench plant a copy of the root key in main SRAM before its search
 * (`EG2CHK FAIL secret found in SRAM`).
 */

#include "sw/device/tests/eg2_boot/eg2_boot.h"

EG2_TEST_CONFIG();

bool eg2_test_main(void) {
#ifdef EG2_SEC_WIPE_BEFORE_SWITCH_INJECT_FAIL
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotB);
#else
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
#endif
  return eg2_boot_summary() && ok;
}
