// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

bool eg2_rm_param(const char *name, uint32_t value, uint32_t min, uint32_t max) {
  // The framework's format (eg2_params.c), so the logs of every test read alike.
  eg2_puts("EG2 param ");
  eg2_puts(name);
  eg2_puts(" = ");
  eg2_putdec(value);
  eg2_puts(" (0x");
  eg2_puthex(value);
  eg2_puts(")\r\n");
  if (value < min || value > max) {
    eg2_puts("EG2 param ");
    eg2_puts(name);
    eg2_puts(" out of range [");
    eg2_putdec(min);
    eg2_puts(", ");
    eg2_putdec(max);
    eg2_puts("]\r\n");
    return false;
  }
  return true;
}
