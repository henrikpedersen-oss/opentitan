// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_power_sb (EG2 verification spec REQ_PBF_18, "Post-boot features in CHERIoT
 * mode"; REQ_PER_05): a wake-up from normal or deep sleep after secure boot, in RISC-V and
 * CHERIoT mode.
 *
 * Expected behaviour, from pwrmgr's documents (hw/top_earlgrey/ip_autogen/pwrmgr/doc):
 *   - normal sleep (CONTROL.MAIN_PD_N 1) keeps every power domain on ("All power domains are kept
 *     on for fast resume", theory_of_operation.md "Normal Sleep"), and on its exit software
 *     "continues executing the instruction after the wait-for-interrupt invocation"
 *     (programmers_guide.md "Exiting from Normal Sleep"): no reset, the core's state kept;
 *     pwrmgr_fsm.sv FastPwrStateLowPowerPrep asserts the non-always-on domains' resets only as
 *     ~main_pd_ni, and OpenTitan's pwrmgr_normal_sleep_all_wake_ups and
 *     pwrmgr_lowpower_cancel_test continue after the WFI. The core leaves the WFI on an enabled
 *     interrupt: pwrmgr's wake-up interrupt, enabled at the PLIC and in mie (mstatus.MIE stays 0,
 *     so no trap is taken);
 *   - deep sleep (MAIN_PD_N 0) exits through a reset into ROM, recorded as
 *     RESET_INFO.LOW_POWER_EXIT ("Exiting from Deep Sleep"), and REQ_SOC_02: every reset returns
 *     to RISC-V mode, which the CHERIoT build's start-up code switches from again.
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
 * Checks, both sleeps: WAKE_INFO.REASONS has exactly the source's bit, no fall-through, no
 * abort. Activity (the bench): pwrmgr's low_power_o rises.
 *   - Normal sleep, in the same boot after the WFI (REQ_PER_05: the core's state kept, CHERIoT
 *     mode kept with its tags): pwrmgr's wake-up interrupt is what ended the WFI; rstmgr's
 *     RESET_INFO has no low-power exit and the boot's reset reason is unchanged; a register held
 *     across the WFI and a word on the stack are unchanged; the mode (misa) is the build's;
 *     CONTROL.LOW_POWER_HINT was cleared by the transition; in CHERIoT mode a capability stored on
 *     the stack before the sleep is still tagged, with its address, and loads through. A boot
 *     after a normal sleep fails (the wake-up reset the chip).
 *   - Deep sleep, on the boot after the wake-up: the reset reason is the low-power exit alone, the
 *     mode (misa) the build's after its start-up. A WFI that returns fails.
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
#include "hw/top/pwrmgr_regs.h"       // Generated.
#include "hw/top/rstmgr_regs.h"       // Generated.
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
  // What the normal sleep keeps: a word on the stack and a register.
  kStackMark = 0x57ac5eedu,
  kRegMark = 0x5e9a11feu,
  // Retention words.
  kRetSrc = 0,
  kRetSlept = 1,  // 1 deep sleep entered, 2 normal sleep entered
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

/** The source's own leftovers: sensor_ctrl's always-on event may reach this boot's alert handler
 * again before the trigger is cleared. That cause is the test's own; it is cleared, never left to
 * the sweep. */
static void source_alert_clear(uint32_t src) {
  if (src != 1) {
    return;
  }
  int a = eg2_status_alert_find("sensor_ctrl_recov_alert");
  if (a >= 0 && eg2_status_wait_alert((size_t)a, 1)) {
    eg2_status_expect_alert((size_t)a);
    eg2_status_clear_expected();
  }
}

static uint32_t misa_mode(void) {
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  return misa & (kMisaE | kMisaI);
}

static bool check_wake_info(uint32_t src) {
  uint32_t info = sys_wake_info(true);
  // The generated field layout: REASONS is NumWkups bits wide, FALL_THROUGH and ABORT follow it
  // (8 wake-ups since the 2026-11-06 merge: bits 8 and 9; the old 0x3f / 0xc0 masks read the
  // aon_timer's reason, bit 6, as a fall through).
  bool ok = eg2_pbf_check("WAKE_INFO.REASONS: the source alone", 1u << pwrmgr_index(src),
                          info & PWRMGR_WAKE_INFO_REASONS_MASK);
  ok = eg2_pbf_check("WAKE_INFO fall-through and abort", 0,
                     info & ((1u << PWRMGR_WAKE_INFO_FALL_THROUGH_BIT) |
                             (1u << PWRMGR_WAKE_INFO_ABORT_BIT))) &&
       ok;
  return ok;
}

/**
 * Normal sleep: the whole round trip in this boot. pwrmgr's wake-up interrupt ends the WFI
 * (enabled at the PLIC and in mie, mstatus.MIE 0: no trap); execution continues after it with
 * the core's state kept (programmers_guide.md "Exiting from Normal Sleep").
 */
static bool normal_sleep(uint32_t src) {
  *eg2_pbf_ret(kRetSrc) = src;
  *eg2_pbf_ret(kRetSlept) = 2;
  *eg2_pbf_ret(kRetDeep) = 0;
  uint32_t reason = eg2_pbf_reset_reason();
  volatile uint32_t stack_word = kStackMark;
  // In CHERIoT mode a tagged capability (to the stack word) stored on the stack.
  void *volatile cap_slot = (void *)&stack_word;
  if (!eg2_pbf_step("low-power entry armed (CFG_CDC_SYNC)",
                    sys_low_power_arm(1u << pwrmgr_index(src), false))) {
    return false;
  }
  sys_wake_irq_on();
  source_arm(src);
  uint32_t reg = kRegMark;
  __asm__ volatile("wfi" : "+r"(reg) : : "memory");
  // Here after the wake-up, in the same boot.
  uint32_t hint = eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CONTROL_REG_OFFSET) &
                  (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT);
  uint32_t plic_id = sys_wake_irq_ack();
  bool ok = check_wake_info(src);
  ok = eg2_pbf_step("the WFI ended by an interrupt (PLIC claim)", plic_id != 0) && ok;
  ok = eg2_pbf_check("rstmgr RESET_INFO: no low-power exit reset", 0,
                     eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET) &
                         (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) &&
       ok;
  ok = eg2_pbf_check("the boot's reset reason unchanged", reason, eg2_pbf_reset_reason()) && ok;
  ok = eg2_pbf_check("a register across the WFI", kRegMark, reg) && ok;
  ok = eg2_pbf_check("a stack word across the sleep", kStackMark, stack_word) && ok;
  ok = eg2_pbf_check("mode after the wake-up (misa): unchanged",
                     eg2_pbf_cheriot() ? kMisaE : kMisaI, misa_mode()) &&
       ok;
  ok = eg2_pbf_check("CONTROL.LOW_POWER_HINT cleared by the transition", 0, hint) && ok;
#ifdef __CHERIOT__
  void *back = cap_slot;
  ok = eg2_pbf_check("capability on the stack: tag after the sleep (REQ_PER_05)", 1,
                     __builtin_cheri_tag_get(back) ? 1 : 0) &&
       ok;
  ok = eg2_pbf_check("capability on the stack: address after the sleep",
                     (uint32_t)__builtin_cheri_address_get((void *)&stack_word),
                     (uint32_t)__builtin_cheri_address_get(back)) &&
       ok;
  if (__builtin_cheri_tag_get(back)) {
    ok = eg2_pbf_check("a load through it", kStackMark, *(volatile uint32_t *)back) && ok;
  }
#else
  (void)cap_slot;
#endif
  sys_low_power_disarm();
  source_clear(src);
  source_alert_clear(src);
  eg2_pbf_activity("normal sleep wake-ups", ok ? 1 : 0);
  return ok;
}

/**
 * Deep sleep: ends in a reset recorded as a low-power exit ("Exiting from Deep Sleep"); only a
 * fall-through or an abort continues after the WFI.
 */
static bool deep_sleep_enter(uint32_t src) {
  *eg2_pbf_ret(kRetSrc) = src;
  *eg2_pbf_ret(kRetSlept) = 1;
  *eg2_pbf_ret(kRetDeep) = 1;
  if (!eg2_pbf_step("low-power entry armed (CFG_CDC_SYNC)",
                    sys_low_power_arm(1u << pwrmgr_index(src), true))) {
    return false;
  }
  source_arm(src);
  sys_wfi();
  // A fall-through or an abort: the WFI returned without the low-power exit's reset.
  sys_low_power_disarm();
  eg2_pbf_note("WAKE_INFO after a WFI that returned", sys_wake_info(false));
  return eg2_pbf_step("deep sleep entered (the WFI did not return)", false);
}

static bool deep_sleep_exit(void) {
  uint32_t src = *eg2_pbf_ret(kRetSrc);
  bool ok = check_wake_info(src);
  ok = eg2_pbf_check("reset reason: the low-power exit alone", kSysRstLowPowerExit,
                     eg2_pbf_reset_reason()) &&
       ok;
  ok = eg2_pbf_check("mode after the wake-up's reset (misa, after the build's start-up)",
                     eg2_pbf_cheriot() ? kMisaE : kMisaI, misa_mode()) &&
       ok;
  source_clear(src);
  source_alert_clear(src);
  eg2_pbf_activity("deep sleep wake-ups", ok ? 1 : 0);
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
    return EG2_PARAM(deep) ? deep_sleep_enter(src) : normal_sleep(src);
  }
  if (boot == 1 && *eg2_pbf_ret(kRetSlept) == 1) {
    return deep_sleep_exit();
  }
  if (boot == 1 && *eg2_pbf_ret(kRetSlept) == 2) {
    eg2_pbf_note("reset reason", eg2_pbf_reset_reason());
    return eg2_pbf_step("no reset after a normal sleep (the wake-up reset the chip)", false);
  }
  return eg2_pbf_step("no further boot expected", false);
}
