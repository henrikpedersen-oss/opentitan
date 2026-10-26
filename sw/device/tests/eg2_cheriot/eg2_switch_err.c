// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_core_ibex_cheriot_switch_err (EG2 spec "Mode switch and handover"; REQ_SOC_04,
 * REQ_CIB_01). RISC-V mode (the switch never completes).
 *
 * A lock with a value other than MuBi4True, a non-strict CHERIOT_ENA, or a fault in the switch's
 * sparse state register sends the switch to its error state: cheriot_ena is then neither
 * MuBi4True nor MuBi4False, the core and cheriot_mem_sys fail closed (no fetch, no meta SRAM
 * access), and fatal_hw_err is raised (REQ_SOC_04, derived from the RTL; rv_core_ibex.hjson
 * CHERIOT_LOCK: "Any other value moves the switch into a terminal error state ... and the fatal
 * alert is raised"). Each of the invalid encodings of cheriot_enable_i raises the core's major
 * internal alert (REQ_CIB_01).
 *
 * Boot 1: rv_core_ibex's fatal_hw_err goes to class A, which escalates to a reset on its first
 * occurrence, with the crash dump captured. CHERIOT_ENA gets `ena_class` (MuBi4True, MuBi4False,
 * another encoding); then either the bench faults one bit of the switch's state register
 * (`state_fault`, a bench step `switch_fault`) and the core locks with MuBi4True, or the core
 * writes `lock_value` (one of the 15 encodings other than MuBi4True) to CHERIOT_LOCK. 64
 * instructions later the test records that it still runs (fetch did not stop) and waits for the
 * reset.
 *
 * Boot 2: the reset reason is the escalation, the crash dump holds fatal_hw_err, and boot 1 never
 * recorded that it ran on after the lock. Not checked here (bench): no meta SRAM access after the
 * error (the vseq), ERR_STATUS.FATAL_CORE_ERR (reset by the escalation).
 *
 * Without a bench (`model_gaps`) a drawn state fault becomes a lock value.
 *
 * Built with EG2_SWITCH_ERR_INJECT_FAIL, boot 2 expects the reset reason software: the run must
 * fail with `MISMATCH reset reason`.
 */

#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define SWITCH_ERR_PARAMS(X) \
  X(lock_value, 16, 0, 16)   \
  X(ena_class, 0, 0, 3)      \
  X(state_fault, 2, 0, 2)    \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(SWITCH_ERR_PARAMS);

enum {
  kRecArmed = 0,
  kRecRanOn = 1,
  kRecLock = 2,
  kRecEna = 3,
};

static eg2_tally_t tally;

/** The lock, then 64 instructions, then the record that fetch went on. */
static OT_NOINLINE void lock_and_watch(uint32_t value, volatile uint32_t *ran_on) {
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET, value);
  __asm__ volatile(".rept 64\n nop\n .endr");
  *ran_on = 1;
}

bool eg2_test_main(void) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  int alert = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (alert < 0) {
    return false;
  }
  if (rec->boot == 1) {
    uint32_t cls = EG2_PARAM(ena_class) != 0 ? EG2_PARAM(ena_class) : eg2_rand_range(1, 3);
    uint32_t ena = cls == 1 ? 0x6 : cls == 2 ? 0x9 : 0;
    if (cls == 3) {
      do {
        ena = eg2_rand_range(0, 15);
      } while (ena == 0x6 || ena == 0x9);
    }
    uint32_t lock = EG2_PARAM(lock_value);
    if (lock == 16 || lock == 0x6) {
      do {
        lock = eg2_rand_range(0, 15);
      } while (lock == 0x6);
    }
    uint32_t fault = EG2_PARAM(state_fault) != 2 ? EG2_PARAM(state_fault) : eg2_pick(2);
    if (!bench) {
      fault = 0;
    }
    eg2_put_kv("EG2 switch_err: CHERIOT_ENA", ena);
    eg2_put_kv(fault ? " state fault, lock" : " lock", fault ? 0x6 : lock);
    eg2_puts("\r\n");
    eg2_ms_escalation_arm((size_t)alert);
    rec->data[kRecArmed] = 1;
    rec->data[kRecRanOn] = 0;
    rec->data[kRecLock] = fault ? 0x6 : lock;
    rec->data[kRecEna] = ena;
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET, ena);
    if (fault && !eg2_ms_bench("switch_fault", 0, 0, bench)) {
      return false;
    }
    lock_and_watch(fault ? 0x6 : lock, &rec->data[kRecRanOn]);
    eg2_ms_spin(1000000);
    eg2_puts("EG2 no escalation reset\r\n");
    return false;
  }
  eg2_check(&tally, "boot 1 armed", 0, rec->data[kRecArmed], 1);
  uint32_t exp_reason = kEg2MsResetEscalation;
#ifdef EG2_SWITCH_ERR_INJECT_FAIL
  exp_reason = kEg2MsResetSw;
#endif
  eg2_check(&tally, "reset reason", 0, eg2_ms_reset_reason() & exp_reason, exp_reason);
  eg2_check(&tally, "crash dump: fatal_hw_err", 0,
            eg2_ms_crashdump_cause((size_t)alert) ? 1 : 0, 1);
  eg2_check(&tally, "fetch stopped after the lock", 0, rec->data[kRecRanOn], 0);
  eg2_put_kv("EG2 boot 1 locked with", rec->data[kRecLock]);
  eg2_put_kv(" CHERIOT_ENA", rec->data[kRecEna]);
  eg2_puts("\r\n");
  // This boot is in RISC-V mode again (REQ_SOC_02).
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  eg2_check(&tally, "misa RISC-V", 0, misa & ((1u << 4) | (1u << 8)), 1u << 8);
  eg2_tally_print("switch_err", &tally);
  return eg2_tally_ok(&tally);
}
