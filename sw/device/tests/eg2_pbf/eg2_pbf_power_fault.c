// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_power_fault (EG2 verification spec REQ_PBF_18, "Post-boot faults"): a glitch of
 * the main power while the chip enters, sleeps in, or leaves low power. Check: the reset
 * information, and no half state: the next sleep works as if nothing had happened.
 *
 * Expected behaviour from pwrmgr's theory of operation ("Main Power Unstable Reset Requests": the
 * power okay going low while main power should be on requests an internal reset, recorded in
 * rstmgr RESET_INFO.HW_REQ bit 5 "pwrmgr: main power glitch reset request"; the checks are valid
 * only in states where power should be on, so a glitch while main power is off in deep sleep
 * does nothing) and its programmer's guide (low-power exit through a reset, LOW_POWER_EXIT).
 *
 * Boot 0: the aon_timer wake-up after kSleepCycles, normal or deep sleep (`deep`), WFI. The bench
 * (chip_sw_eg2_pbf_power_fault_vseq) glitches vcmain_pok at `fault_point`: 0 the sleep entry
 * (core_sleeping rises), 1 during the sleep, 2 the exit (pwrmgr leaving low power).
 * Boot 1: the reset reason, by the documents:
 *   - entry, or the sleep of a normal sleep: the main power glitch (PowerUnstable) is set, and
 *     nothing but it and the low-power exit;
 *   - the sleep of a deep sleep (main power off, the glitch is not seen): the low-power exit alone;
 *   - the exit: the low-power exit or the glitch or both, nothing else.
 * pwrmgr's CONTROL.LOW_POWER_HINT reads 0, FAULT_STATUS 0 (the glitch is cleared by the reset it
 * caused). Then the clean cycle: deep sleep again, aon_timer wake-up.
 * Boot 2: the low-power exit alone, WAKE_INFO the aon_timer alone: no half state.
 *
 * Knobs: deep 0-1, fault_point 0-2, model_gaps (0 on the chip; eg2sim has no bench, so no glitch:
 * it checks the clean behaviour and names the glitch checks it skips).
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (boot 1's LOW_POWER_HINT) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/pwrmgr_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define POWER_FAULT_PARAMS(X) \
  X(deep, 1, 0, 1)            \
  X(fault_point, 0, 0, 2)     \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(POWER_FAULT_PARAMS)

enum {
  kSleepCycles = 100,  // 500 us asleep
  kRetPhase = 0,
};

static bool sleep_now(bool deep) {
  if (!eg2_pbf_step("low-power entry armed (CFG_CDC_SYNC)",
                    sys_low_power_arm(1u << kSysWakeAonTimer, deep))) {
    return false;
  }
  sys_wkup_after(kSleepCycles);
  sys_wfi();
  sys_low_power_disarm();
  return eg2_pbf_step("sleep entered (the WFI did not return)", false);
}

static bool boot_1(void) {
  uint32_t reason = eg2_pbf_reset_reason();
  bool ok = eg2_pbf_check("CONTROL.LOW_POWER_HINT after the reset", 0,
                          eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CONTROL_REG_OFFSET) & 1u);
  ok = eg2_pbf_check("FAULT_STATUS after the reset", 0,
                     eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  eg2_pbf_note("reset reason", reason);
  eg2_pbf_note("WAKE_INFO", sys_wake_info(true));
  uint32_t allowed = kSysRstPowerUnstable | kSysRstLowPowerExit;
  ok = eg2_pbf_step("reset reason within the glitch and the low-power exit",
                    reason != 0 && (reason & ~allowed) == 0) &&
       ok;
  bool deep = EG2_PARAM(deep) != 0;
  uint32_t point = EG2_PARAM(fault_point);
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("the main power glitch (no bench: the reset is the low-power exit alone)");
    ok = eg2_pbf_check("reset reason without a glitch", kSysRstLowPowerExit, reason) && ok;
  } else if (point == 0 || (point == 1 && !deep)) {
    ok = eg2_pbf_step("reset reason has the main power glitch", reason & kSysRstPowerUnstable) &&
         ok;
  } else if (point == 1 && deep) {
    ok = eg2_pbf_check("reset reason: a glitch while main power is off is not seen",
                       kSysRstLowPowerExit, reason) &&
         ok;
  }
  sys_wkup_clear();
  *eg2_pbf_ret(kRetPhase) = 2;
  if (!ok) {
    return false;
  }
  eg2_pbf_step("the clean sleep cycle follows", true);
  return sleep_now(true);
}

static bool boot_2(void) {
  uint32_t info = sys_wake_info(true);
  bool ok = eg2_pbf_check("clean cycle: reset reason the low-power exit alone",
                          kSysRstLowPowerExit, eg2_pbf_reset_reason());
  ok = eg2_pbf_check("clean cycle: WAKE_INFO the aon_timer alone", 1u << kSysWakeAonTimer,
                     info) &&
       ok;
  sys_wkup_clear();
  eg2_pbf_activity("sleep cycles through a glitch", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("power_fault", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  uint32_t boot = eg2_pbf_boot_index();
  if (boot == 0) {
    *eg2_pbf_ret(kRetPhase) = 1;
    eg2_pbf_note("fault point (0 entry, 1 asleep, 2 exit)", EG2_PARAM(fault_point));
    return sleep_now(EG2_PARAM(deep) != 0);
  }
  if (boot == 1 && *eg2_pbf_ret(kRetPhase) == 1) {
    return boot_1();
  }
  if (boot == 2 && *eg2_pbf_ret(kRetPhase) == 2) {
    return boot_2();
  }
  return eg2_pbf_step("no further boot expected", false);
}
