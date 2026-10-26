// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_cheriot_fault (EG2 verification spec REQ_PBF_22, REQ_PBF_07, "Post-boot
 * faults"): a fault while the CHERIoT services are in use, CHERIoT mode only.
 *
 * Boot 1: objects allocated and half of them freed (eg2_pbf_heap.h), then the fault armed and
 * fired at `fault_point`:
 *   0 during a revocation sweep: a long sweep is started over the holders and the heap, and the
 *     fault fires after `sweep_progress` percent of its expected length (two bus words per
 *     capability; the test checks the engine is still busy and reports it);
 *   1 during a compartment switch: the fault fires as the test calls into a "compartment", a
 *     function reached through a sentry with one capability argument (the bare-metal stand-in for
 *     the RTOS switcher: there is no RTOS port for the chip in this tree), which runs until the
 *     reset.
 * `fault` 0 an escalation: rv_core_ibex's fatal_sw_err raised, class A escalating straight to the
 * escalation reset (no NMI: the CHERIoT build ends the test on a trap); 1 a watchdog bite:
 * aon_timer's watchdog enabled with a bite threshold of one tick, its reset request enabled in
 * pwrmgr.
 *
 * Boot 2: the reset reason is the fault's (RESET_INFO.HW_REQ: escalation or aon_timer); the engine
 * is at its documented reset state (TBRE_REGWEN 1, TBRE_BASE_ADDR, TBRE_NUM_CAPS, TBRE_STATUS and
 * TBRE_EPOCH 0: the sweep did not survive the reset); no stale tag: every holder loads untagged
 * and the revocation bitmap is clear, with the meta SRAM zeroed again before CHERIoT mode was
 * locked (REQ_CMS_15); for an escalation, the alert crash dump holds fatal_sw_err. That the chip
 * reset to RISC-V mode (cheriot_ena not MuBi4True when the core leaves reset) is the bench's
 * check (chip_sw_eg2_pbf_cheriot_fault_vseq).
 *
 * Knobs: fault_point, fault, sweep_progress (0-100), model_gaps.
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top/aon_timer_regs.h"  // Generated.
#include "hw/top/pwrmgr_regs.h"     // Generated.
#include "hw/top/rstmgr_regs.h"     // Generated.
#include "sw/device/lib/base/macros.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_heap.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define CHERIOT_FAULT_PARAMS(X) \
  X(fault_point, 0, 0, 1)       \
  X(fault, 0, 0, 1)             \
  X(sweep_progress, 50, 0, 100) \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(CHERIOT_FAULT_PARAMS)

enum {
  kAllocs = 8,
  kSweepCaps = 8192,
  // A sweep reads two bus words per capability; one spin iteration is a few cycles.
  kSpinPerPercent = kSweepCaps / 100,
  kEscCycles = 20,
  kResetWait = 2000000,
  kRetPhase = 0,
  kRetBusy = 1,
  kPhaseFired = 0x46495245,  // "FIRE"
  kResetEscalation = 1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 3),
  kResetWatchdog = 1u << (RSTMGR_RESET_INFO_HW_REQ_OFFSET + 1),
};

static void pwrmgr_sync(void) {
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  for (uint32_t i = 0; i < 100000; ++i) {
    if ((eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1) == 0) {
      break;
    }
  }
}

static uint32_t fatal_sw_err;

static void arm(void) {
  if (EG2_PARAM(fault) == 0) {
    fatal_sw_err = eg2_pbf_alert("rv_core_ibex_fatal_sw_err");
    eg2_pbf_escalate_reset(&fatal_sw_err, 1, kEscCycles);
    eg2_status_expect_alert(fatal_sw_err);
    eg2_pbf_crashdump_arm();
  } else {
    eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_RESET_EN_REG_OFFSET, 1u << PWRMGR_RESET_EN_EN_1_BIT);
    pwrmgr_sync();
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 0xffffffff);
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, 1);
  }
}

/** Fires the armed fault (a function of its own: a fault can be placed at it). */
static OT_NOINLINE void fire(void) {
  *eg2_pbf_ret(kRetPhase) = kPhaseFired;
  if (EG2_PARAM(fault) == 0) {
    eg2_status_raise_alert(fatal_sw_err);
  } else {
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET,
                    1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
  }
}

/** The compartment: runs on its one capability until the reset. */
static OT_NOINLINE void compartment(void *volatile *holder) {
  for (uint32_t i = 0; i < kResetWait; ++i) {
    (void)__builtin_cheri_tag_get(*holder);
  }
}

static bool boot_1(void) {
  void *objs[kAllocs];
  uint32_t next = kHeapObjects;
  for (uint32_t i = 0; i < kAllocs; ++i) {
    objs[i] = heap_alloc(&next, eg2_rand_range(8, 512));
    heap_holder_store(i, objs[i]);
  }
  for (uint32_t i = 0; i < kAllocs; i += 2) {
    heap_mark(objs[i], true);
  }
  arm();
  if (EG2_PARAM(fault_point) == 0) {
    bool started = tbre_start(kHeapHolders, kSweepCaps);
    eg2_pbf_spin(EG2_PARAM(sweep_progress) * kSpinPerPercent);
    uint32_t busy = ms_rd(kMsTbreStatus) & kMsStatusBusy;
    *eg2_pbf_ret(kRetBusy) = busy | (started ? 2u : 0u);
    fire();
  } else {
    void (*volatile entry)(void *volatile *) = compartment;  // a sentry in CHERIoT mode
    fire();
    entry(heap_slot(kHeapHolders));
  }
  eg2_pbf_spin(kResetWait);
  return eg2_pbf_step("reset after the fault", false);
}

static bool boot_2(void) {
  bool ok = eg2_pbf_check("phase before the reset", kPhaseFired, *eg2_pbf_ret(kRetPhase));
  uint32_t want = EG2_PARAM(fault) == 0 ? kResetEscalation : kResetWatchdog;
  ok = eg2_pbf_check("reset reason", want, eg2_pbf_reset_reason()) && ok;
  if (EG2_PARAM(fault_point) == 0) {
    uint32_t b = *eg2_pbf_ret(kRetBusy);
    eg2_pbf_note("sweep started (2) and busy at the fault (1)", b);
    ok = eg2_pbf_check("sweep started before the fault", 2, b & 2) && ok;
    eg2_pbf_activity("faults during a busy sweep", b & 1);
  }
  ok = eg2_pbf_check("TBRE_REGWEN after the reset", 1, ms_rd(kMsTbreRegwen)) && ok;
  ok = eg2_pbf_check("TBRE_STATUS after the reset", 0, ms_rd(kMsTbreStatus)) && ok;
  ok = eg2_pbf_check("TBRE_EPOCH after the reset", 0, ms_rd(kMsTbreEpoch)) && ok;
  ok = eg2_pbf_check("TBRE_BASE_ADDR after the reset", 0, ms_rd(kMsTbreBase)) && ok;
  ok = eg2_pbf_check("TBRE_NUM_CAPS after the reset", 0, ms_rd(kMsTbreNum)) && ok;
  uint32_t stale = 0;
  for (uint32_t i = 0; i < kAllocs; ++i) {
    stale += heap_holder_tag(i);
  }
  ok = eg2_pbf_check("holders tagged after the reset", 0, stale) && ok;
  if (EG2_PARAM(model_gaps)) {
    // eg2sim keeps the revbm window apart from sram_ctrl_meta's RAM, so the meta SRAM's INIT on
    // this boot does not clear it; in the chip they are one memory (cheriot_mem_sys theory of
    // operation, "Meta SRAM Address Map").
    eg2_pbf_model_gap("revocation bitmap cleared by the meta SRAM INIT");
  } else {
    ok = eg2_pbf_check("revocation bitmap words set after the reset", 0, heap_revbm_nonzero()) &&
         ok;
  }
  if (EG2_PARAM(fault) == 0) {
    uint32_t a = eg2_pbf_alert("rv_core_ibex_fatal_sw_err");
    ok = eg2_pbf_check("alert crash dump: fatal_sw_err", 1, eg2_pbf_crashdump_alert(a)) && ok;
  }
  eg2_pbf_gap("the sweep's own error", "a reset clears TBRE_STATUS: the aborted sweep leaves no "
                                       "error to read; its effect (no stale tag) is checked");
  eg2_pbf_activity("cheriot faults recovered", 1);
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("cheriot_fault", EG2_PBF_INJECT);
  if (eg2_pbf_boot_index() == 0) {
    return boot_1() && ok;
  }
  return boot_2() && ok;
}
