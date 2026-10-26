// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_core_ibex_cheriot_switch (EG2 spec "Mode switch and handover"; REQ_SOC_03).
 *
 * The first write of MuBi4True to CHERIOT_LOCK locks the mode CHERIOT_ENA holds at that moment.
 * Until the next reset no write changes the mode; CHERIoT mode never returns to ePMP mode. In
 * CHERIoT mode misa has E=1/I=0 (RFC CHERIoT in OpenTitan p2, p3 "self-locking, write-once
 * register"; rv_core_ibex.hjson CHERIOT_ENA, CHERIOT_LOCK).
 *
 * eg2_switch.S writes CHERIOT_ENA `ena_writes` times with drawn classes, then the final mode's
 * value, and locks it from `lock_source` (the core, or SBA through the bench); an ePMP lock is
 * judged there, in RISC-V mode. This is the CHERIoT lock's part: misa E=1/I=0; every earlier
 * CHERIOT_ENA write read back as written; then requests of ePMP mode (CHERIOT_ENA MuBi4False and
 * other encodings, CHERIOT_LOCK with all 16 encodings) after each of which misa is unchanged and
 * the core still runs this purecap code. No alert (the status sweep). The life-cycle state is the
 * OTP image the bench loads (TEST_UNLOCKED*, DEV, RMA).
 *
 * Built with EG2_SWITCH_INJECT_FAIL, misa is expected RISC-V after the later requests (in an ePMP
 * lock, CHERIoT): the run must fail with `MISMATCH misa after a request` (or `EG2 late FAIL 21`).
 */

#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_late.h"
#include "sw/device/tests/eg2_cheriot/eg2_switch_params.h"

EG2_LATE_TEST_PARAMS(EG2_SWITCH_PARAMS);

static eg2_tally_t tally;

static uint32_t misa_mode(void) {
  uint32_t m;
  __asm__ volatile("csrr %0, misa" : "=r"(m));
  return m & ((1u << 4) | (1u << 8));
}

bool eg2_test_main(void) {
  eg2_put_kv("EG2 switch: CHERIOT_ENA writes", eg2_late_word(EG2_LATE_INFO + EG2_SWITCH_INFO_WRITES));
  eg2_put_kv(" lock from", eg2_late_word(EG2_LATE_INFO + EG2_SWITCH_INFO_SOURCE));
  eg2_puts(" (1 core, 2 SBA)\r\n");
  eg2_check(&tally, "misa after the lock", 0, misa_mode(), 1u << 4);
  eg2_check(&tally, "CHERIOT_ENA writes read back", 0,
            eg2_late_word(EG2_LATE_INFO + EG2_SWITCH_INFO_READBACK), 0);
  // Requests of the other mode, and every lock encoding.
  for (uint32_t v = 0; v < 16; ++tally.done, ++v) {
    ++tally.planned;
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET,
                    v == 0x6 ? 0x9 : v);
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, v);
    __asm__ volatile("fence.i");
    uint32_t exp = 1u << 4;
#ifdef EG2_SWITCH_INJECT_FAIL
    exp = 1u << 8;
#endif
    eg2_check(&tally, "misa after a request", v, misa_mode(), exp);
  }
  eg2_tally_print("switch", &tally);
  return eg2_tally_ok(&tally);
}
