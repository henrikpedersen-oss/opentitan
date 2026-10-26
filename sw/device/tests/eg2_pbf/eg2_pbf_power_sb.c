// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_power_sb (EG2 verification spec REQ_PBF_18, "Post-boot features in CHERIoT
 * mode"): a wake-up from normal or deep sleep after secure boot, in RISC-V and CHERIoT mode.
 * Expected behaviour from pwrmgr's theory of operation and programmer's guide
 * (hw/top_earlgrey/ip_autogen/pwrmgr/doc: the low-power entry sequence, WAKE_INFO, and "Exiting
 * from Normal Sleep" / "Exiting from Deep Sleep": both exits go through a reset recorded as
 * RESET_INFO.LOW_POWER_EXIT, only a fall-through or an abort continues after the WFI), and
 * REQ_SOC_02: every reset returns to RISC-V mode, which the CHERIoT build's start-up code switches
 * from again.
 *
 * Knobs:
 *   wake_src     0 aon_timer's wake-up timer; 1 sensor_ctrl (an AST event raised through
 *                ALERT_TRIG as sleep is entered, as sensor_ctrl_wakeup_test does); 2 a pin: pinmux
 *                wake-up detector 5 on IOB7, rising edge, driven by the bench (the vseq's
 *                pinmux_wkup_if) once pwrmgr is in low power;
 *   deep         0 normal sleep, 1 deep sleep;
 *   sleep_class  the time asleep: aon_timer threshold 20, 100 or 400 AON cycles (100 us, 500 us,
 *                2 ms); for the pin, the bench's delay in AON cycles (the vseq);
 *   model_gaps   0 on the chip; 1 in eg2sim, which has no bench and no sensor_ctrl events:
 *                wake_src 1 and 2 run as 0 and say so.
 *
 * Checks, on the boot after the wake-up: WAKE_INFO.REASONS has exactly the source's bit, no
 * fall-through, no abort; the reset reason is the low-power exit alone; the mode (misa) is the
 * build's. A WFI that returns fails. Activity (the bench): pwrmgr's low_power_o rises.
 * Gap: REQ_PER_05's "a wake-up from normal sleep keeps the core's state, continues in CHERIoT
 * mode with the tags intact" disagrees with pwrmgr's guide (normal sleep exits through a reset);
 * the test follows the guide and reports the gap.
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (the wake-up reason) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 *
 * EG2_NEEDS_BENCH: the low-power exit needs the DV bench's AST clock and power model; on the
 * Verilator top (AST_BYPASS_CLK) no sleep entered by WFI came back within the hour (regr-vlt
 * 2026-10-26: this test, pbf_sensor_sb, rram_idle_low_power, revocation_sleep and OpenTitan's
 * pwrmgr_smoketest, whose Verilator target upstream marks manual).
 */

#include "hw/top/pinmux_regs.h"       // Generated.
#include "hw/top/sensor_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define POWER_SB_PARAMS(X) \
  X(wake_src, 0, 0, 2)     \
  X(deep, 0, 0, 1)         \
  X(sleep_class, 1, 0, 2)  \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(POWER_SB_PARAMS)

enum {
  kPinDetector = 5,
  kPolls = 100000,
  kMisaE = 1u << 4,
  kMisaI = 1u << 8,
  // Retention words.
  kRetSrc = 0,
  kRetSlept = 1,
  kRetDeep = 2,
};

static const uint32_t kSleepCycles[3] = {20, 100, 400};

/** The wake-up source this run uses: the pin needs the bench. */
static uint32_t wake_src(void) {
  uint32_t s = EG2_PARAM(wake_src);
  if (s == 2 && EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("pin wake-up (no bench in the model: the aon_timer instead)");
    return 0;
  }
  if (s == 1 && EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("sensor_ctrl wake-up (ALERT_TRIG and RECOV_ALERT: the aon_timer instead)");
    return 0;
  }
  return s;
}

static uint32_t pwrmgr_index(uint32_t src) {
  return src == 0 ? kSysWakeAonTimer : src == 1 ? kSysWakeSensorCtrl : kSysWakePin;
}

static void source_arm(uint32_t src) {
  switch (src) {
    case 0:
      sys_wkup_after(kSleepCycles[EG2_PARAM(sleep_class)]);
      break;
    case 1:
      eg2_status_expect_alert((size_t)eg2_status_alert_find("sensor_ctrl_recov_alert"));
      eg2_dev_write32(kEg2DevSensorCtrl, SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 1u);
      break;
    default: {
      uint32_t det = 4 * kPinDetector;
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_DETECTOR_0_REG_OFFSET + det,
                      PINMUX_WKUP_DETECTOR_0_MODE_0_VALUE_POSEDGE);
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_DETECTOR_CNT_TH_0_REG_OFFSET + det, 0);
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_DETECTOR_PADSEL_0_REG_OFFSET + det,
                      kTopEarlgreyPinmuxInselIob7);
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET + det, 1);
      break;
    }
  }
}

/** Takes the source down after the wake-up, so nothing is left for the final sweep. */
static void source_clear(uint32_t src) {
  switch (src) {
    case 0:
      sys_wkup_clear();
      break;
    case 1:
      eg2_dev_write32(kEg2DevSensorCtrl, SENSOR_CTRL_ALERT_TRIG_REG_OFFSET, 0);
      // The event goes down a few AON cycles after the trigger; then the latch is cleared.
      for (uint32_t i = 0; i < 100; ++i) {
        eg2_pbf_spin(50);
        eg2_dev_write32(kEg2DevSensorCtrl, SENSOR_CTRL_RECOV_ALERT_REG_OFFSET, 0x7ffu);
        if (eg2_dev_read32(kEg2DevSensorCtrl, SENSOR_CTRL_RECOV_ALERT_REG_OFFSET) == 0) {
          break;
        }
      }
      break;
    default:
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_DETECTOR_EN_0_REG_OFFSET + 4 * kPinDetector, 0);
      eg2_dev_write32(kEg2DevPinmux, PINMUX_WKUP_CAUSE_REG_OFFSET, 0);
      break;
  }
}

static uint32_t misa_mode(void) {
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  return misa & (kMisaE | kMisaI);
}

static bool check_wake_info(uint32_t src) {
  uint32_t info = sys_wake_info(true);
  bool ok = eg2_pbf_check("WAKE_INFO.REASONS: the source alone", 1u << pwrmgr_index(src),
                          info & 0x3fu);
  ok = eg2_pbf_check("WAKE_INFO fall-through and abort", 0, info & 0xc0u) && ok;
  return ok;
}

/**
 * Low-power entry. Normal and deep sleep both end in a reset recorded as a low-power exit
 * (pwrmgr programmer's guide, "Exiting from Normal Sleep": only a fall-through or an abort
 * continues after the WFI without one); normal sleep keeps the power domains on.
 */
static bool sleep_enter(uint32_t src, bool deep) {
  *eg2_pbf_ret(kRetSrc) = src;
  *eg2_pbf_ret(kRetSlept) = 1;
  *eg2_pbf_ret(kRetDeep) = deep ? 1 : 0;
  if (!eg2_pbf_step("low-power entry armed (CFG_CDC_SYNC)",
                    sys_low_power_arm(1u << pwrmgr_index(src), deep))) {
    return false;
  }
  source_arm(src);
  sys_wfi();
  // A fall-through or an abort: the WFI returned without the low-power exit's reset.
  sys_low_power_disarm();
  eg2_pbf_note("WAKE_INFO after a WFI that returned", sys_wake_info(false));
  return eg2_pbf_step("sleep entered (the WFI did not return)", false);
}

static bool sleep_exit(void) {
  uint32_t src = *eg2_pbf_ret(kRetSrc);
  bool ok = check_wake_info(src);
  ok = eg2_pbf_check("reset reason: the low-power exit alone", kSysRstLowPowerExit,
                     eg2_pbf_reset_reason()) &&
       ok;
  ok = eg2_pbf_check("mode after the wake-up's reset (misa, after the build's start-up)",
                     eg2_pbf_cheriot() ? kMisaE : kMisaI, misa_mode()) &&
       ok;
  if (*eg2_pbf_ret(kRetDeep) == 0 && eg2_pbf_cheriot()) {
    eg2_pbf_gap("normal sleep continuing in CHERIoT mode with its tags (REQ_PER_05)",
                "pwrmgr's programmer's guide exits normal sleep through a reset, so the core's "
                "state is not kept; the CHERIoT start-up code runs again (and zeroes the meta "
                "SRAM)");
  }
  source_clear(src);
  if (src == 1) {
    // sensor_ctrl is always-on: its event may reach this boot's alert handler again before the
    // trigger is cleared. That cause is the test's own; it is cleared, never left to the sweep.
    int a = eg2_status_alert_find("sensor_ctrl_recov_alert");
    if (a >= 0 && eg2_status_wait_alert((size_t)a, 1)) {
      eg2_status_expect_alert((size_t)a);
      eg2_status_clear_expected();
    }
  }
  eg2_pbf_activity(*eg2_pbf_ret(kRetDeep) ? "deep sleep wake-ups" : "normal sleep wake-ups",
                   ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("power", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  eg2_pbf_boot_stage();
  uint32_t boot = eg2_pbf_boot_index();
  if (boot == 0) {
    uint32_t src = wake_src();
    eg2_pbf_note("wake-up source (0 aon_timer, 1 sensor_ctrl, 2 pin)", src);
    return sleep_enter(src, EG2_PARAM(deep) != 0);
  }
  if (boot == 1 && *eg2_pbf_ret(kRetSlept) == 1) {
    return sleep_exit();
  }
  return eg2_pbf_step("no further boot expected", false);
}
