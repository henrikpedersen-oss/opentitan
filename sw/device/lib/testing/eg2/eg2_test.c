// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_test.h"

#ifndef __CHERIOT__
#include "sw/device/lib/arch/device.h"
#endif

// Called by OTTF (RISC-V mode) or by eg2_cheriot_main (CHERIoT mode).
bool test_main(void) {
#ifndef __CHERIOT__
  // OTTF and the test ROM set UART0 up on every platform but sim_dv (ottf_main.c,
  // test_rom.c: `kDeviceType != kDeviceSimDV`), and the UART keeps written bytes in its TX
  // FIFO while CTRL.TX is clear: the console would stop at a full FIFO. CHERIoT mode's start-up
  // code does the same (eg2_cheriot_main.c).
  if (kDeviceType == kDeviceSimDV) {
    eg2_console_init();
  }
#endif
  bool ok = eg2_params_begin();
  eg2_status_init();
  if (ok) {
    ok = eg2_test_main();
  }
  return eg2_test_end(ok);
}
