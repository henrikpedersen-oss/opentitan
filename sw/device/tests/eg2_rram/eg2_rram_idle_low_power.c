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
 * state"; WAKE_INFO.ABORT). A low-power exit restarts the chip through a reset recorded as
 * RESET_INFO.LOW_POWER_EXIT (pwrmgr programmer's guide, rstmgr).
 *
 * Boot 1 (no LOW_POWER_EXIT in the recorded reset reason):
 *   - starts a controller write of `n_words` words (seed-determined data) to scratch pages;
 *   - with `entry` 0 (start) or 1 (middle) it requests low power (pwrmgr CONTROL.LOW_POWER_HINT,
 *     the aon timer's wake-up armed) and executes WFI with the write still in flight: the entry
 *     must abort (WAKE_INFO.ABORT), the core goes on after the wake-up interrupt, and the write is
 *     completed and read back;
 *   - then (and directly with `entry` 2, end) it requests low power with the controller idle: the
 *     entry must go through, and the chip restarts.
 * Boot 2 (LOW_POWER_EXIT): the aon timer is the wake-up reason; every written word reads back
 * through the controller with ECC on and no error (data and integrity both written); WAKE_INFO
 * is cleared. The test ROM records RESET_INFO in the retention SRAM and clears the register, so
 * the boot is told by the record and the status sweep sees a clean reset reason.
 *
 * Not covered: an OTP write in flight at the low-power request (REQ_RRAM_26 names it too); an OTP
 * DAI write completes in a few microseconds, too soon to place a WFI inside it from software.
 *
 * Knobs:
 *   n_words     the write's length in bus words (a multiple of 4)
 *   entry       the low-power request: 0 at the start of the write, 1 in the middle, 2 after it,
 *               3 drawn
 *   sleep_type  0 normal sleep (MAIN_PD_N = 1), 1 deep sleep, 2 drawn
 *   wake_ticks  aon timer ticks to the wake-up
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why (eg2_aon.h, Eg2Pwrmgr)                       |
 * |-------------------------------------------|--------------------------------------------------|
 * | the abort with a write in flight (entry   | "Aborts (a non-volatile operation in flight) are |
 * |   0 and 1 run as entry 2)                 |   not modelled"                                  |
 *
 * Built with EG2_RRAM_IDLE_LOW_POWER_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/aon_timer_regs.h"  // Generated.
#include "hw/top/pwrmgr_regs.h"     // Generated.
#include "hw/top/rstmgr_regs.h"     // Generated.
#include "hw/top/rv_plic_regs.h"    // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define IDLE_LOW_POWER_PARAMS(X) \
  X(n_words, 512, 4, 1024)       \
  X(entry, 3, 0, 3)              \
  X(sleep_type, 2, 0, 2)         \
  X(wake_ticks, 20, 2, 1000)     \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(IDLE_LOW_POWER_PARAMS)

enum {
  kPolls = 200000,
  kSalt = 0x49444c45,  // "IDLE"
  kAonWake = kTopEarlgreyPowerManagerWakeUpsAonTimerWkupReq,
  kIrqAon = kTopEarlgreyPlicIrqIdAonTimerWkupTimerExpired,
  kIrqPwrmgr = kTopEarlgreyPlicIrqIdPwrmgrWakeup,
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
 * Requests low power with the aon timer as the wake-up and executes WFI with
 * interrupts off at mstatus (mie.MEIE wakes the core without a trap). Returns
 * when the core runs on: after an abort or a fall through (a low-power exit
 * restarts the chip instead).
 */
static bool sleep_request(bool deep) {
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, EG2_PARAM(wake_ticks));
  aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET,
         1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  pw_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, 1u << kAonWake);
  pw_wr(PWRMGR_WAKE_INFO_REG_OFFSET, 0xffffffffu);  // rw1c: a clean record
  pw_wr(PWRMGR_CONTROL_REG_OFFSET,
        1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT |
            (deep ? 0 : 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
  bool ok = eg2_chk_true("pwrmgr CDC sync", pw_sync());
  plic_enable(kIrqAon, true);
  plic_enable(kIrqPwrmgr, true);
  pw_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 1u << PWRMGR_INTR_ENABLE_WAKEUP_BIT);
  __asm__ volatile("csrs mie, %0" ::"r"(1u << 11));  // MEIE
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
  __asm__ volatile("wfi");
  // Still running: the entry did not go through. Undo the request and the
  // wake-up sources.
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  pw_wr(PWRMGR_CONTROL_REG_OFFSET, 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT);
  ok = eg2_chk_true("pwrmgr CDC sync", pw_sync()) && ok;
  __asm__ volatile("csrc mie, %0" ::"r"(1u << 11));
  aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET,
         1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
  pw_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
  plic_drain();
  plic_enable(kIrqAon, false);
  plic_enable(kIrqPwrmgr, false);
  return ok;
}

/** The write's place and length: the same draws in both boots. */
static uint32_t w_addr, w_words;

static bool read_back(void) {
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
  if (!ok) {
    return eg2_chk_summary(4) && ok;
  }
  // Idle now: the entry goes through and the chip restarts into boot 2.
  eg2_chk_step("low-power request with the controller idle", 0);
  sleep_request(deep);
  // Still here: the entry did not go through.
  ok = eg2_chk_true("low-power entry with the controller idle", false) && ok;
  eg2_chk_step("WAKE_INFO", pw(PWRMGR_WAKE_INFO_REG_OFFSET));
  return eg2_chk_summary(4) && ok;
}
