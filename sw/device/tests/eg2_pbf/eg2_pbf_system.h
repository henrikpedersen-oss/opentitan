// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_SYSTEM_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_SYSTEM_H_

/**
 * @file
 * @brief What the system group of the post-boot feature tests shares (REQ_PBF_16, 17, 18, 21):
 * the always-on timer's wake-up and watchdog, the power manager's low-power entry, the PLIC line
 * of the power manager's wake-up interrupt, rstmgr's crash dump, life-cycle state and transition
 * requests, and OTP's direct access interface. Every register comes from its IP's documentation
 * (hw/ip/aon_timer, hw/top_earlgrey/ip_autogen/{pwrmgr,rstmgr,alert_handler,otp_ctrl},
 * hw/ip/lc_ctrl), reached through eg2_dev().
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---- Reset reasons (rstmgr RESET_INFO; HW_REQ in the top's reset_requests order) ----
enum {
  kSysRstPor = 1u << 0,
  kSysRstLowPowerExit = 1u << 1,
  kSysRstSw = 1u << 2,
  kSysRstSysrstCtrl = 1u << 3,
  kSysRstWatchdog = 1u << 4,
  kSysRstPowerUnstable = 1u << 5,
  kSysRstEscalation = 1u << 6,
  kSysRstNdm = 1u << 7,
};

// ---- pwrmgr wake-up sources (top_earlgrey.hjson wakeups order) ----
enum {
  kSysWakeSysrstCtrl = 0,
  kSysWakeAdcCtrl = 1,
  kSysWakePin = 2,
  kSysWakeUsb = 3,
  kSysWakeAonTimer = 4,
  kSysWakeSensorCtrl = 5,
};

/** AON clock cycles in `us` microseconds (200 kHz in sim_dv, the clock the model uses too). */
uint32_t sys_aon_cycles(uint32_t us);

/** The watchdog: bite (a pwrmgr reset request) after `bite` AON cycles; bark never. */
void sys_wdog_bite_after(uint32_t bite);
/** Stops the watchdog and clears its count. */
void sys_wdog_off(void);

/** The wake-up timer: expires after `cycles` AON cycles (prescaler 0). */
void sys_wkup_after(uint32_t cycles);
/** Stops the wake-up timer, clears its count, cause and interrupt. */
void sys_wkup_clear(void);

/**
 * Low-power entry, as pwrmgr's programmer's guide orders it: the wake-up sources `wake_mask`
 * (bit i: source i), CONTROL with LOW_POWER_HINT, all clocks off in low power, MAIN_PD_N = !deep,
 * then CFG_CDC_SYNC and its completion. The caller then executes WFI. Returns false if the sync
 * did not complete.
 */
bool sys_low_power_arm(uint32_t wake_mask, bool deep);
/** Clears LOW_POWER_HINT (an aborted or completed entry). */
void sys_low_power_disarm(void);
/** Enables pwrmgr's wake-up interrupt at the PLIC and in `mie`, with `mstatus.MIE` off, so a WFI
 * returns on it without a trap. */
void sys_wake_irq_on(void);
/** Clears pwrmgr's wake-up interrupt and completes it at the PLIC. Returns the claimed ID. */
uint32_t sys_wake_irq_ack(void);
/** Executes WFI. */
static inline void sys_wfi(void) { __asm__ volatile("wfi"); }

/** pwrmgr WAKE_INFO (REASONS, FALL_THROUGH bit 6, ABORT bit 7); clears it if `clear`. */
uint32_t sys_wake_info(bool clear);

// ---- Crash dump (rstmgr ALERT_INFO; alert_handler alert_crashdump_t) ----
enum { kSysDumpMaxWords = 16 };
/** Reads the crash dump rstmgr captured; returns its size in words (ALERT_INFO_ATTR). */
uint32_t sys_crashdump_read(uint32_t *dump, uint32_t max_words);
/** Enables the crash dump capture at the next reset (ALERT_INFO_CTRL.EN). */
void sys_crashdump_enable(void);
/** A class's escalation state (0 Idle, 4-7 Phase0-3) in the dump. */
uint32_t sys_dump_state(const uint32_t *dump, uint32_t cls);
/** A class's accumulator in the dump. */
uint32_t sys_dump_accum(const uint32_t *dump, uint32_t cls);
/** Whether alert `alert`'s cause is set in the dump. */
bool sys_dump_cause(const uint32_t *dump, uint32_t alert);

/** A class's escalation counter (CLASSx_ESC_CNT) and its CLR_REGWEN. */
uint32_t sys_class_esc_cnt(uint32_t cls);
uint32_t sys_class_clr_regwen(uint32_t cls);

// ---- Life cycle (lc_ctrl) ----
/** LC_STATE, LC_TRANSITION_CNT and STATUS. */
uint32_t sys_lc_state(void);
uint32_t sys_lc_count(void);
uint32_t sys_lc_status(void);
/** The state's name ("DEV"), or "other". */
const char *sys_lc_name(uint32_t state);
/**
 * Claims the transition interface and starts a transition to `target` with `token` (4 words):
 * lc_ctrl's programmer's guide, steps 3, 5 and 7. Returns false if the mutex could not be
 * claimed (TRANSITION_REGWEN stays 0). After the start the core is disabled (POST_TRANSITION):
 * the caller arranges a reset first.
 */
bool sys_lc_request(uint32_t target, const uint32_t *token);

// ---- OTP direct access (otp_ctrl programmer's guide, "Direct Access Interface") ----
/** Waits for DAI idle. False on a timeout. */
bool sys_otp_dai_idle(void);
/** A 32-bit DAI read at OTP byte address `addr`. Returns STATUS's error bits (0 on success). */
uint32_t sys_otp_read32(uint32_t addr, uint32_t *value);
/** Starts a 32-bit DAI write; does not wait. */
void sys_otp_write32_start(uint32_t addr, uint32_t value);
/** The DAI's ERR_CODE (the direct access interface's error code register). */
uint32_t sys_otp_dai_err(void);
/** A word through the software config window (unbuffered software partitions). */
uint32_t sys_otp_window32(uint32_t addr);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_SYSTEM_H_
