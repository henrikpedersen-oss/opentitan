// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_epmp_handover (EG2 spec "Mode switch and handover"; REQ_SOC_06).
 *
 * In CHERIoT mode the ePMP has no effect: regions and mseccfg (MML, locked entries) set before the
 * switch deny no fetch, load or store after it (RFC CHERIoT in OpenTitan p3 "the ePMP extension
 * will be disabled"; RFC CHERIoT Ibex "Execution Stage").
 *
 * eg2_epmp_handover.S locks a plan of deny-all entries (and MML with `mml`) and switches. Here,
 * every region the plan denied is reached through a capability from the roots, the first access of
 * the `first_access` kind (fetch, load, store): a call of the denied function, loads and stores in
 * sec SRAM, a load from the RRAM test area, loads and stores in retention SRAM, a register read of
 * each denied device window. None may fault. The plan as programmed (pmpcfg0-3, mseccfg) is
 * printed from the capture area.
 *
 * Differences from the spec's stimulus: the boot is the test ROM's (its plan: one locked RWX entry
 * over everything); the test's entries stand in for ROM_EXT's locked plan. A secure-boot run of
 * this test (ROM_EXT's plan itself) is the `_secure_boot` dvsim entry.
 *
 * Built with EG2_EPMP_HANDOVER_INJECT_FAIL, the call of the denied function is expected to fault:
 * the run must fail with `MISMATCH fetch`.
 */

#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_epmp_handover_params.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_LATE_TEST_PARAMS(EG2_EPMP_HANDOVER_PARAMS);

uint32_t eg2_epmp_target(void);

static eg2_tally_t tally;

static void fetch(uint32_t idx) {
  ++tally.planned;
  uint32_t got = eg2_epmp_target();
  uint32_t exp = 0x5a1e;
#ifdef EG2_EPMP_HANDOVER_INJECT_FAIL
  exp = 0;
#endif
  eg2_check(&tally, "fetch", idx, got, exp);
  ++tally.done;
}

static void access(uint32_t addr, bool store, uint32_t idx) {
  ++tally.planned;
  uint32_t v = 0x12345678u;
  bool t = eg2_probe_call(eg2_mem(addr, 4), (void *)(uintptr_t)v, NULL,
                          store ? eg2_stub_sw : eg2_stub_lw);
  eg2_check(&tally, store ? "store trapped" : "load trapped", idx, t ? eg2_trap_rec.mcause : 0,
            0);
  ++tally.done;
}

static void device_loads(void) {
  access(TOP_EARLGREY_RV_TIMER_BASE_ADDR + RV_TIMER_CTRL_REG_OFFSET, false, 5);
  access(TOP_EARLGREY_GPIO_BASE_ADDR, false, 6);
  access(TOP_EARLGREY_AON_TIMER_BASE_ADDR + 0x4, false, 7);
  access(TOP_EARLGREY_UART1_BASE_ADDR + 0x10, false, 8);
  access(TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR + 0x4, false, 9);
}

/** Loads first (device registers, which hold values), or stores first (SRAM), as drawn. Memory is
 * written before it is read: after the boot it holds nothing yet. */
static void loads_and_stores(uint32_t first) {
  if (first != 3) {
    device_loads();
  }
  access(kEg2SecBase + 0x100, true, 0);
  access(kEg2SecBase + 0x100, false, 1);
  access(kEg2MsRetBase + 0x1c00, true, 3);
  access(kEg2MsRetBase + 0x1c00, false, 4);
  access(kEg2MsRramArea, false, 2);
  if (first == 3) {
    device_loads();
  }
}

bool eg2_test_main(void) {
  eg2_put_kv("EG2 epmp_handover: extra entries", eg2_late_word(EG2_LATE_INFO + EG2_EPMP_INFO_EXTRA));
  eg2_put_kv(" MML", eg2_late_word(EG2_LATE_INFO + EG2_EPMP_INFO_MML));
  eg2_puts("\r\nEG2 pmpcfg0-3:");
  for (uint32_t i = 0; i < 4; ++i) {
    eg2_puts(" ");
    eg2_puthex(eg2_late_word(EG2_LATE_INFO + EG2_EPMP_INFO_PMPCFG0 + 4 * i));
  }
  eg2_put_kv(" mseccfg", eg2_late_word(EG2_LATE_INFO + EG2_EPMP_INFO_MSECCFG));
  eg2_puts("\r\n");
  // The RRAM word the test loads is programmed through the controller (no core access).
  uint32_t w[4] = {1, 2, 3, 4}, err;
  if (!eg2_ms_rram_setup() || !eg2_ms_rram_program(kEg2MsRramArea, w, 4, &err)) {
    eg2_puts("EG2 RRAM programming failed\r\n");
    return false;
  }
  uint32_t first = EG2_PARAM(first_access) != 0 ? EG2_PARAM(first_access) : eg2_rand_range(1, 3);
  eg2_probe_begin();
  if (first == 1) {
    fetch(0);
    loads_and_stores(2);
  } else {
    loads_and_stores(first);
    fetch(0);
  }
  eg2_probe_end();
  eg2_tally_print("epmp_handover", &tally);
  return eg2_tally_ok(&tally);
}
