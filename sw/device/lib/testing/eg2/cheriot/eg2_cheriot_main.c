// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_test.h"

/**
 * The CHERIoT build's C entry, called by `eg2_cheriot_start.S` once CHERIoT mode is locked and
 * the stack, globals and capability relocations are in place.
 */
void eg2_cheriot_main(void) __attribute__((noreturn));

void eg2_cheriot_main(void) {
  eg2_console_init();
  eg2_result_in_test();
  eg2_puts("EG2 test, CHERIoT mode\r\n");
  eg2_result_report(test_main());
}
