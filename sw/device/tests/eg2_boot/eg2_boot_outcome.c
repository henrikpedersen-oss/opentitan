// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * The owner image of the boot-outcome tests (EG2 verification spec REQ_BOOT_06, REQ_BOOT_08,
 * REQ_BOOT_14): chip_sw_eg2_boot_key_role, chip_sw_eg2_boot_usage_constraints and
 * chip_sw_eg2_boot_manifest_bad. Each test is a set of images that differ in what ROM or ROM_EXT
 * must verify (the ROM_EXT's signing key, this image's manifest); the bench
 * (chip_sw_eg2_boot_outcome_vseq) checks the outcome each must have: the boot failure code on
 * UART0, or ROM_EXT reached, or this image's pass.
 *
 * When this image runs, it checks that ROM_EXT booted it from slot A (the boot log) and prints
 * the life-cycle state and its own manifest's usage constraints.
 *
 * Built with EG2_BOOT_MUST_NOT_RUN (the images that must be refused), it fails at once with
 * `EG2CHK FAIL a refused image ran`.
 * Built with EG2_BOOT_OUTCOME_INJECT_FAIL the image is expected in slot B: the run must fail with
 * `EG2CHK FAIL boot_log bl0_slot`.
 */

#include "sw/device/tests/eg2_boot/eg2_boot.h"

EG2_TEST_CONFIG();

bool eg2_test_main(void) {
#ifdef EG2_BOOT_MUST_NOT_RUN
  eg2_boot_expect("a refused image ran", false, 0);
  return false;
#else
  eg2_boot_note("life-cycle state", eg2_lc_state_decoded());
#ifdef EG2_BOOT_OUTCOME_INJECT_FAIL
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotB);
#else
  bool ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA);
#endif
  return eg2_boot_summary() && ok;
#endif
}
