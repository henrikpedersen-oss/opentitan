// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_test.h"

// Called by OTTF (RISC-V mode) or by eg2_cheriot_main (CHERIoT mode).
bool test_main(void) {
  bool ok = eg2_params_begin();
  eg2_status_init();
  if (ok) {
    ok = eg2_test_main();
  }
  return eg2_test_end(ok);
}
