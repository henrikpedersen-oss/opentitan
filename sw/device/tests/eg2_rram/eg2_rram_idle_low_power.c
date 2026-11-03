// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_idle_low_power (EG2 verification spec REQ_RRAM_26, REQ_SEC_21, "RRAM controller in
 * RISC-V mode"): the power manager does not power down while a controller write is in flight, so
 * no word is left half written.
 *
 * Expected behaviour: RFC RRAM §3.1 p7 ("rram_ctrl indicates to the power manager when it is safe
 * to turn the power off. This is the case when no write operation is in progress"; the top
 * connects pwrmgr.pwr_nvm to rram_ctrl.pwrmgr); pwrmgr theory of operation "Abort Handling" ("If
 * any module is active, the fast FSM aborts entry handling and returns the system to active
 * state"; WAKE_INFO.ABORT). The exit of the two sleeps (pwrmgr programmers_guide.md): a deep
 * sleep (CONTROL.MAIN_PD_N 0) restarts the chip through a reset recorded as
 * RESET_INFO.LOW_POWER_EXIT ("Exiting from Deep Sleep"); a normal sleep (MAIN_PD_N 1) does not
 * reset, software "continues executing the instruction after the wait-for-interrupt invocation"
 * ("Exiting from Normal Sleep"; theory_of_operation.md "Normal Sleep": "All power domains are
 * kept on for fast resume"; pwrmgr_fsm.sv FastPwrStateLowPowerPrep asserts the non-always-on
 * domains' resets only as ~main_pd_ni; OpenTitan's pwrmgr_normal_sleep_all_wake_ups and
 * pwrmgr_lowpower_cancel_test continue after the WFI). The core leaves the WFI on an enabled
 * interrupt (the aon timer's and pwrmgr's wake-up, at the PLIC and mie.MEIE; mstatus.MIE 0).
 * CONTROL is written read-modify-write: only LOW_POWER_HINT and MAIN_PD_N change, so
 * USB_CLK_EN_ACTIVE (reset value 1) keeps the USB clock on in the active state.
 *
 * Boot 1 (no LOW_POWER_EXIT in the recorded reset reason):
 *   - starts a controller write of `n_words` words (seed-determined data) to scratch pages;
 *   - with `entry` 0 (start) or 1 (middle) it requests low power (pwrmgr CONTROL.LOW_POWER_HINT,
 *     the aon timer's wake-up armed) and executes WFI with the write still in flight: the entry
 *     must abort (WAKE_INFO.ABORT), the core goes on after the wake-up interrupt, and the write is
 *     completed and read back;
 *   - then (and directly with `entry` 2, end) it requests low power with the controller idle: the
 *     entry must go through. Deep sleep: the chip restarts into boot 2. Normal sleep: the core
 *     continues after the WFI in this boot: WAKE_INFO is the aon timer alone (no abort, no
 *     fall-through), rstmgr's RESET_INFO has no low-power exit and the recorded reset reason is
 *     unchanged, the mode (misa) is the build's, and every written word reads back again.
 * Boot 2 (LOW_POWER_EXIT, deep sleep only): the aon timer is the wake-up reason; every written
 * word reads back through the controller with ECC on and no error (data and integrity both
 * written); WAKE_INFO is cleared. The test ROM records RESET_INFO in the retention SRAM and clears
 * the register, so the boot is told by the record and the status sweep sees a clean reset reason.
 * A boot 2 after a normal sleep fails (the wake-up reset the chip).
 *
 * An OTP write in flight (`operation` 1; the `eg2_rram_idle_low_power_otp` builds, test
 * chip_sw_rram_idle_low_power_otp). In Earl Grey 2 the OTP is the top pages of the RRAM: an OTP
 * write through otp_ctrl's direct access interface (DAI) is an OtpWrite of rram_ctrl's OTP plug,
 * which writes the data word and then, in a separate RRAM write, the integrity word (RFC RRAM §4.3
 * p9 "Each OtpWrite operation will write to two different RRAM words to store the data and the
 * integrity value", table: "4. Write data word ... 7. Write updated integrity word"; rram_ctrl
 * theory_of_operation.md "Integrity Scheme": the data word is written in `StWrite`, then the
 * integrity word in `StWriteIntg`). Expected behaviour: RFC RRAM §4.3 p9 ("otp_ctrl will indicate
 * to pwrmgr when it is safe to turn power off. This ensures that the power is not removed during a
 * write operation, avoiding an invalid OTP partition state where data updates have occurred
 * without corresponding integrity value updates"); otp_ctrl interfaces.md "Power Manager
 * Interface" (`otp_idle` "indicates whether there is an ongoing write operation in the Direct
 * Access Interface (DAI) ... and the power manager uses that indication to determine whether a
 * power down request needs to be aborted"); pwrmgr "Abort Handling" (above). An OtpRead recomputes
 * the integrity and returns an error if it does not match the data ("Integrity Scheme"), so a read
 * back without an error says that data and integrity were both written.
 *   The word is CREATOR_SW_CFG_ROM_EXT_SKU (otp_ctrl_mmap.hjson: "Unused by SiliconCreator
 *   software"; CREATOR_SW_CFG has integrity and is unlocked in the test images, digest 0). Boot 1
 *   reads it, then makes `otp_writes` DAI writes, each setting one more drawn bit (OTP only sets
 *   bits). For each write the low-power request lands in one step: `otp_step` 0 the data word's
 *   RRAM write, 1 the integrity word's, 2 the two in turn from a drawn first. The software prints
 *   `EG2 RRAM otp sleep step <s> flag 0x<addr>` and starts the write; the vseq holds the OTP plug
 *   inside that step's RRAM write (before its data reaches the RRAM) and writes s + 1 to the flag
 *   over SBA; only then the software executes WFI with low power requested, so the request is
 *   evaluated while the step is in flight. After the WFI the software prints `EG2 RRAM otp
 *   release` and the vseq lets the write go on. Checks for each write: the word reads back as
 *   written with no DAI error (data and integrity both written), the write's ERR_CODE is clear,
 *   the flag is the step (the bench held the step that was asked for), WAKE_INFO.ABORT. Then the
 *   same idle low-power entry as the controller write: deep sleep reads the word back in boot 2
 *   (every written bit set, no error), normal sleep in boot 1.
 *
 * Knobs:
 *   n_words     the write's length in bus words (a multiple of 4)
 *   entry       the low-power request: 0 at the start of the write, 1 in the middle, 2 after it,
 *               3 drawn
 *   sleep_type  0 normal sleep (MAIN_PD_N = 1), 1 deep sleep, 2 drawn
 *   wake_ticks  aon timer ticks to the wake-up
 *   operation   the write in flight: 0 a controller write, 1 an OTP write (default 1 in the
 *               `_otp` builds)
 *   otp_writes  OTP writes (operation 1)
 *   otp_step    the OTP write's step at the low-power request: 0 data, 1 integrity, 2 in turn
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why (eg2_aon.h, Eg2Pwrmgr)                       |
 * |-------------------------------------------|--------------------------------------------------|
 * | the abort with a write in flight (entry   | "Aborts (a non-volatile operation in flight) are |
 * |   0 and 1 run as entry 2)                 |   not modelled"                                  |
 * | the OTP write steps held and the request  | the same, and no bench (eg2sim's DAI completes   |
 * |   in each (the OTP writes and read-backs  |   at once, ip/eg2_mem_otp.h)                     |
 * |   run)                                    |                                                  |
 *
 * Built with EG2_RRAM_IDLE_LOW_POWER_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `RRAM FAIL`. Built with EG2_RRAM_IDLE_LOW_POWER_OTP_INJECT_FAIL that check is
 * `OTP word read back after the write` (the first OTP write's value).
 *
 * EG2_NEEDS_BENCH: on the Verilator top no sleep entered by WFI came back within the hour
 * (regr-vlt 2026-10-26: TIMEOUT after `RRAM deep sleep`; see eg2_pbf_power_sb.c).
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/aon_timer_regs.h"  // Generated.
#include "hw/top/otp_ctrl_regs.h"   // Generated.
#include "hw/top/pwrmgr_regs.h"     // Generated.
#include "hw/top/rstmgr_regs.h"     // Generated.
#include "hw/top/rv_plic_regs.h"    // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

// The write in flight by default: the controller's, the OTP's in the `_otp` builds.
#ifdef EG2_RRAM_IDLE_LOW_POWER_OTP
#define IDLE_LOW_POWER_OPERATION 1
#else
#define IDLE_LOW_POWER_OPERATION 0
#endif

#define IDLE_LOW_POWER_PARAMS(X)               \
  X(n_words, 512, 4, 1024)                     \
  X(entry, 3, 0, 3)                            \
  X(sleep_type, 2, 0, 2)                       \
  X(wake_ticks, 20, 2, 1000)                   \
  X(operation, IDLE_LOW_POWER_OPERATION, 0, 1) \
  X(otp_writes, 4, 1, 16)                      \
  X(otp_step, 2, 0, 2)                         \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(IDLE_LOW_POWER_PARAMS)

enum {
  kPolls = 200000,
  kSalt = 0x49444c45,  // "IDLE"
  kAonWake = kTopEarlgreyPowerManagerWakeUpsAonTimerWkupReq,
  kIrqAon = kTopEarlgreyPlicIrqIdAonTimerWkupTimerExpired,
  kIrqPwrmgr = kTopEarlgreyPlicIrqIdPwrmgrWakeup,
  kOpOtp = 1,
  kOtpMaxWrites = 16,
  kOtpWord = OTP_CTRL_PARAM_CREATOR_SW_CFG_ROM_EXT_SKU_OFFSET,
  kDaiErr = OTP_CTRL_ERR_CODE_0_REG_OFFSET +
            4 * OTP_CTRL_PARAM_NUM_PART,  // ERR_CODE_11: the DAI's
};

static uint32_t pw(uint32_t off) { return eg2_dev_read32(kEg2DevPwrmgr, off); }
static void pw_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevPwrmgr, off, v);
}
static void aon_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevAonTimer, off, v);
}
static void plic_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevRvPlic, off, v);
}

/** pwrmgr's register to its slow clock domain (programmer's guide:
 * CFG_CDC_SYNC). */
static bool pw_sync(void) {
  pw_wr(PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  for (uint32_t i = 0; i < kPolls; ++i) {
    if ((pw(PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1) == 0) {
      return true;
    }
  }
  return false;
}

static void plic_enable(uint32_t irq, bool on) {
  plic_wr(RV_PLIC_PRIO_0_REG_OFFSET + 4 * irq, on ? 1 : 0);
  uint32_t off = RV_PLIC_IE0_0_REG_OFFSET + 4 * (irq / 32);
  uint32_t ie = eg2_dev_read32(kEg2DevRvPlic, off);
  plic_wr(off, on ? ie | 1u << (irq % 32) : ie & ~(1u << (irq % 32)));
}

/** Claims and completes every pending PLIC source. */
static void plic_drain(void) {
  for (int i = 0; i < 8; ++i) {
    uint32_t id = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
    if (id == 0) {
      return;
    }
    plic_wr(RV_PLIC_CC0_REG_OFFSET, id);
  }
}

/**
 * Requests low power with the aon timer as the wake-up, all but starting the
 * timer (sleep_wfi does that); `*control_out` is pwrmgr's CONTROL without the
 * request, for sleep_undo.
 */
static bool sleep_arm(bool deep, uint32_t *control_out) {
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, EG2_PARAM(wake_ticks));
  // An earlier expiry's wake-up request withdrawn (it follows WKUP_CAUSE).
  aon_wr(AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET,
         1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  pw_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, 1u << kAonWake);
  pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);  // rw1c: a clean record
  // Read-modify-write: the other fields (USB_CLK_EN_ACTIVE, the clock enables) as they are.
  const uint32_t kHint = 1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT;
  const uint32_t kPdN = 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT;
  uint32_t control = pw(PWRMGR_CONTROL_REG_OFFSET) & ~(kHint | kPdN);
  pw_wr(PWRMGR_CONTROL_REG_OFFSET, control | kHint | (deep ? 0 : kPdN));
  bool ok = eg2_chk_true("pwrmgr CDC sync", pw_sync());
  plic_enable(kIrqAon, true);
  plic_enable(kIrqPwrmgr, true);
  pw_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 1u << PWRMGR_INTR_ENABLE_WAKEUP_BIT);
  __asm__ volatile("csrs mie, %0" ::"r"(1u << 11));  // MEIE
  *control_out = control;
  return ok;
}

/** Starts the aon timer and executes WFI with interrupts off at mstatus
 * (mie.MEIE wakes the core without a trap). */
static void sleep_wfi(void) {
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  __asm__ volatile("wfi");
}

/** Still running: the entry did not go through, or a normal sleep ended. Undoes
 * the request and the wake-up sources. */
static bool sleep_undo(uint32_t control) {
  const uint32_t kPdN = 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT;
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  pw_wr(PWRMGR_CONTROL_REG_OFFSET, control | kPdN);
  bool ok = eg2_chk_true("pwrmgr CDC sync", pw_sync());
  __asm__ volatile("csrc mie, %0" ::"r"(1u << 11));
  aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET,
         1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  pw_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
  plic_drain();
  plic_enable(kIrqAon, false);
  plic_enable(kIrqPwrmgr, false);
  return ok;
}

/**
 * Requests low power and executes WFI. Returns when the core runs on: after an
 * abort, a fall through, or a normal sleep's wake-up (a deep sleep's exit
 * restarts the chip instead).
 */
static bool sleep_request(bool deep) {
  uint32_t control;
  bool ok = sleep_arm(deep, &control);
  sleep_wfi();
  return sleep_undo(control) && ok;
}

static uint32_t otp_rd(uint32_t off) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, off);
}
static void otp_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevOtpCtrlCore, off, v);
}

static bool dai_wait_idle(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (otp_rd(OTP_CTRL_STATUS_REG_OFFSET) &
        (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      return true;
    }
  }
  return false;
}

/** A DAI read of the OTP word; returns the DAI's ERR_CODE, 0xff on a timeout. */
static uint32_t dai_read(uint32_t *v) {
  if (!dai_wait_idle()) {
    return 0xff;
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, kOtpWord);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
         1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  if (!dai_wait_idle()) {
    return 0xff;
  }
  *v = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  return otp_rd(kDaiErr) & OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
}

/**
 * The OTP writes' draws, the same in both boots: write k sets the bits
 * `otp_acc[k]` (one more than write k - 1) and has the low-power request in
 * step `otp_step_of[k]` (0 data, 1 integrity). `otp_exp` under `otp_mask` is
 * what a read-back must give: in boot 2 every written bit set (boot 1's word
 * before the writes is not known there), in boot 1 the exact word.
 */
static uint32_t otp_n, otp_acc[kOtpMaxWrites], otp_step_of[kOtpMaxWrites];
static uint32_t otp_base, otp_exp, otp_mask;
/** Written over SBA by the bench while it holds the step: the step + 1. */
static volatile uint32_t otp_held;

static void otp_draw(void) {
  otp_n = EG2_PARAM(otp_writes);
  uint32_t first = eg2_rand_range(0, 1);
  uint32_t acc = 0;
  for (uint32_t k = 0; k < otp_n; ++k) {
    otp_step_of[k] =
        EG2_PARAM(otp_step) == 2 ? (first + k) % 2 : EG2_PARAM(otp_step);
    // A bit no earlier write set, at or above a drawn position.
    uint32_t b = eg2_rand_range(0, 31);
    for (uint32_t i = 0; i < 32; ++i) {
      uint32_t bit = 1u << ((b + i) % 32);
      if ((acc & bit) == 0) {
        acc |= bit;
        break;
      }
    }
    otp_acc[k] = acc;
  }
  otp_exp = acc;
  otp_mask = acc;
}

static bool otp_read_back(void) {
  uint32_t v = 0;
  uint32_t err = dai_read(&v);
  bool ok = eg2_chk_eq("OTP word read back", otp_exp, v & otp_mask);
  return eg2_chk_eq("OTP read back ERR_CODE (integrity matches the data)", 0,
                    err) &&
         ok;
}

/**
 * OTP write k with the low-power request in its step: with `bench`, the vseq
 * holds the OTP plug inside the step's RRAM write and sets `otp_held`; the
 * software then executes WFI. Without (the model) the write runs on its own.
 */
static bool otp_write(uint32_t k, bool deep, bool bench) {
  bool ok = eg2_chk_true("DAI idle before the OTP write", dai_wait_idle());
  uint32_t step = otp_step_of[k];
  uint32_t value = otp_base | otp_acc[k];
  uint32_t control = 0;
  otp_held = 0;
  eg2_chk_step("OTP write, value", value);
  if (bench) {
    eg2_chk_step("OTP write, step at the low-power request", step);
    ok = sleep_arm(deep, &control) && ok;
    // The vseq waits for this line, holds the step, then writes step + 1 to
    // `otp_held` over SBA.
    LOG_INFO("EG2 RRAM otp sleep step %d flag 0x%08x", step,
             eg2_chk_addr_of(&otp_held));
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, kOtpWord);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET, value);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_WDATA_1_REG_OFFSET, 0);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
         1u << OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT);
  uint32_t held = 0;
  uint32_t wake = 0;
  if (bench) {
    for (uint32_t i = 0; i < kPolls && otp_held == 0; ++i) {
    }
    held = otp_held;
    // The aon timer starts only now, so its wake-up cannot come before the
    // WFI. No WFI without the hold: the request would land anywhere.
    if (held == step + 1) {
      sleep_wfi();
    }
    ok = sleep_undo(control) && ok;
    wake = pw(PWRMGR_WAKE_INFO_REG_OFFSET);
    pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
    EG2_CHK_SYNC("EG2 RRAM otp release");
  }
  ok = eg2_chk_true("DAI idle after the OTP write", dai_wait_idle()) && ok;
  uint32_t werr = otp_rd(kDaiErr) & OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
  uint32_t v = 0;
  uint32_t rerr = dai_read(&v);
  // The first value check of the OTP operation: an _inject_fail build breaks
  // this one.
  ok = eg2_chk_eq("OTP word read back after the write", value, v) && ok;
  ok = eg2_chk_eq("OTP read back ERR_CODE (integrity matches the data)", 0,
                  rerr) &&
       ok;
  ok = eg2_chk_eq("OTP write ERR_CODE", 0, werr) && ok;
  if (bench) {
    ok = eg2_chk_eq("the bench held the OTP write's step (step + 1)", step + 1,
                    held) &&
         ok;
    ok = eg2_chk_eq("WAKE_INFO.ABORT (OTP write in flight)", 1,
                    (wake >> PWRMGR_WAKE_INFO_ABORT_BIT) & 1) &&
         ok;
  }
  return ok;
}

/** Boot 1 of the OTP operation: the writes, each with a request in its step. */
static bool otp_boot1(bool deep) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  if (!bench) {
    eg2_chk_skip(
        "the OTP write steps held and the low-power request in each (the "
        "writes and read-backs run)");
  }
  uint32_t err = dai_read(&otp_base);
  bool ok = eg2_chk_true("DAI read of the OTP word before the writes", err == 0);
  eg2_chk_step("OTP word before the writes", otp_base);
  eg2_chk_step("deep sleep", deep);
  for (uint32_t k = 0; k < otp_n; ++k) {
    ok = otp_write(k, deep, bench) && ok;
  }
  if (bench) {
    EG2_CHK_SYNC("EG2 RRAM otp sleep done");
  }
  otp_exp = otp_base | otp_acc[otp_n - 1];
  otp_mask = 0xffffffffu;
  return otp_read_back() && ok;
}

/** The write's place and length: the same draws in both boots. */
static uint32_t w_addr, w_words;

static bool read_back(void) {
  if (EG2_PARAM(operation) == kOpOtp) {
    return otp_read_back();
  }
  bool ok = true;
  uint32_t buf[16];
  uint32_t bad = 0;
  eg2_rram_result_t r;
  for (uint32_t i = 0; i < w_words; i += 16) {
    uint32_t m = w_words - i < 16 ? w_words - i : 16;
    ok = eg2_chk_true(
             "read back done",
             eg2_rram_read(kEg2RramPartData, w_addr + 4 * i, buf, m, &r)) &&
         ok;
    ok = eg2_chk_eq("read back ERR_CODE (no ECC or integrity error)", 0,
                    r.err_code) &&
         ok;
    for (uint32_t j = 0; j < m; ++j) {
      bad += buf[j] != eg2_rram_pattern(kSalt, i + j);
    }
  }
  return eg2_chk_eq("written words that differ", 0, bad) && ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  // The same draws in both boots (eg2_rand restarts from the seed).
  w_words = EG2_PARAM(n_words) & ~3u;
  uint32_t pages =
      (4 * w_words + kEg2RramPageBytes - 1) / kEg2RramPageBytes + 1;
  uint32_t first = eg2_rand_range(kEg2RramScratchFirstPage,
                                  kEg2RramScratchLastPage - pages + 1);
  w_addr =
      first * kEg2RramPageBytes + kEg2RramWordBytes * eg2_rand_range(0, 31);
  uint32_t entry =
      EG2_PARAM(entry) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(entry);
  uint32_t st =
      EG2_PARAM(sleep_type) == 2 ? eg2_rand_range(0, 1) : EG2_PARAM(sleep_type);
  bool deep = st == 1;
  if (EG2_PARAM(operation) == kOpOtp) {
    otp_draw();
  }
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(region, first, pages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  eg2_chk_step("write at", kEg2RramHostBase + w_addr);
  eg2_chk_step("words", w_words);

  bool ok = true;
  // The test ROM records RESET_INFO in the retention SRAM
  // (creator.reset_reasons, at offset 4 of retention_sram_t) and clears the
  // register (test_rom.c), so the reason is read from there.
  uint32_t info = eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4);
  if (info & (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) {
    // Boot 2.
    eg2_chk_step("boot after the low-power exit, RESET_INFO", info);
    ok = eg2_chk_true("a reset only after deep sleep (normal sleep continues after the WFI)",
                      deep) &&
         ok;
    uint32_t wake = pw(PWRMGR_WAKE_INFO_REG_OFFSET);
    ok = eg2_chk_eq("WAKE_INFO: the aon timer woke the chip", 1u << kAonWake,
                    wake) &&
         ok;
    ok = read_back() && ok;
    eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET,
                    1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT);
    pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
    pw_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
    aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET,
           1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
    eg2_rram_region_off(region);
    return eg2_chk_summary(3) && ok;
  }

  // Boot 1.
  if (EG2_PARAM(operation) == kOpOtp) {
    ok = otp_boot1(deep) && ok;
  } else {
    if (entry != 2 && EG2_PARAM(model_gaps)) {
      eg2_chk_skip("the abort with a write in flight (entry at the end instead)");
      entry = 2;
    }
    eg2_chk_step("entry", entry);
    eg2_chk_step("deep sleep", deep);
    eg2_rram_start(kEg2RramOpWrite, kEg2RramPartData, w_addr, w_words);
    uint32_t pushed = 0;
    uint32_t stop = entry == 0 ? 0 : entry == 1 ? w_words / 2 : w_words;
    for (; pushed < stop; ++pushed) {
      if (!eg2_rram_push(eg2_rram_pattern(kSalt, pushed), kPolls)) {
        ok = eg2_chk_true("write FIFO room", false) && ok;
        break;
      }
    }
    if (entry != 2) {
      eg2_chk_step("low-power request with the write in flight, words pushed",
                   pushed);
      ok = sleep_request(deep) && ok;
      uint32_t wake = pw(PWRMGR_WAKE_INFO_REG_OFFSET);
      ok = eg2_chk_eq("WAKE_INFO.ABORT (rram_ctrl not idle)", 1,
                      (wake >> PWRMGR_WAKE_INFO_ABORT_BIT) & 1) &&
           ok;
      pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
      for (; pushed < w_words; ++pushed) {
        if (!eg2_rram_push(eg2_rram_pattern(kSalt, pushed), kPolls)) {
          ok = eg2_chk_true("write FIFO room", false) && ok;
          break;
        }
      }
    }
    eg2_rram_result_t r;
    ok = eg2_chk_true("write done", eg2_rram_wait_done(&r, kPolls)) && ok;
    ok = eg2_chk_eq("write OP_STATUS", 1, r.op_status) && ok;
    ok = eg2_chk_eq("write ERR_CODE", 0, r.err_code) && ok;
    ok = read_back() && ok;
  }
  if (!ok) {
    return eg2_chk_summary(4) && ok;
  }
  // Idle now: the entry goes through. Deep sleep restarts the chip into boot 2.
  eg2_chk_step("low-power request with the controller idle", 0);
  ok = sleep_request(deep) && ok;
  if (deep) {
    // Still here: the entry did not go through.
    ok = eg2_chk_true("deep sleep entry with the controller idle", false) && ok;
    eg2_chk_step("WAKE_INFO", pw(PWRMGR_WAKE_INFO_REG_OFFSET));
    return eg2_chk_summary(4) && ok;
  }
  // Normal sleep: here after the wake-up, in the same boot.
  uint32_t wake = pw(PWRMGR_WAKE_INFO_REG_OFFSET);
  ok = eg2_chk_eq("WAKE_INFO after the normal sleep: the aon timer alone (no abort, no "
                  "fall-through)",
                  1u << kAonWake, wake) &&
       ok;
  ok = eg2_chk_eq("rstmgr RESET_INFO: no low-power exit reset", 0,
                  eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET) &
                      (1u << RSTMGR_RESET_INFO_LOW_POWER_EXIT_BIT)) &&
       ok;
  ok = eg2_chk_eq("the boot's recorded reset reason unchanged", info,
                  eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4)) &&
       ok;
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
#ifdef __CHERIOT__
  const uint32_t kMisaMode = 1u << 4;  // E: the CHERIoT build
#else
  const uint32_t kMisaMode = 1u << 8;  // I: the RISC-V build
#endif
  ok = eg2_chk_eq("mode after the wake-up (misa): unchanged", kMisaMode,
                  misa & ((1u << 4) | (1u << 8))) &&
       ok;
  ok = read_back() && ok;
  pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);
  eg2_rram_region_off(region);
  return eg2_chk_summary(9) && ok;
}
