// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sram_ctrl_sleep_sram_ret_contents_impl.h"
#include "sw/device/lib/testing/test_framework/ottf_main.h"

// EG2, CHERIoT mode: the boot stage leaves the REQ_SOC_08 locks open for this test
// (sw/device/lib/testing/eg2/cheriot/eg2_cheriot_start.S step 0b): it wipes and re-keys the
// retention SRAM through sram_ctrl_ret's CTRL, which a compliant boot locks before the switch.
const volatile uint8_t eg2_cheriot_boot_locks_open __attribute__((used)) = 1;

OTTF_DEFINE_TEST_CONFIG();

bool test_main(void) {
  return execute_sram_ctrl_sleep_ret_sram_contents_test(true);
}
