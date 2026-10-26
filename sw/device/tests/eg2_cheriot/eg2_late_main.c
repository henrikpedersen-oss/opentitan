// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_params.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"

/**
 * Keeps the globals' output section in the image (as ottf_cheriot_main.c does): the start-up code
 * copies the section from NVM, which has its bytes only if it holds an initialised global.
 */
static volatile uint32_t eg2_late_data_present = 1;

void eg2_late_main(void) __attribute__((noreturn));

void eg2_late_main(void) {
  (void)eg2_late_data_present;
  eg2_console_init();
  eg2_result_in_test();
  eg2_puts("EG2 test, CHERIoT mode (switched by the test)\r\n");
  bool ok = eg2_params_begin();
  eg2_status_init();
  if (ok) {
    ok = eg2_test_main();
  }
  eg2_result_report(eg2_test_end(ok));
}

uint32_t eg2_late_area(void) { return eg2_sym_stack_start() - EG2_LATE_AREA_BELOW_STACK; }

uint32_t eg2_late_word(uint32_t offset) {
  volatile uint32_t *p = eg2_mem(eg2_late_area() + offset, 4);
  return *p;
}

void *eg2_late_cap(uint32_t offset) {
  void *a = eg2_mem(eg2_late_area() + offset, 8);
  void *c;
  __asm__ volatile("clc %0, 0(%1)" : "=C"(c) : "C"(a) : "memory");
  return c;
}
