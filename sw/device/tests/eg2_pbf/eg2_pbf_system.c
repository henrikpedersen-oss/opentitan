// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/aon_timer_regs.h"      // Generated.
#include "hw/top/dt/pwrmgr.h"           // Generated.
#include "hw/top/lc_ctrl_regs.h"        // Generated.
#include "hw/top/otp_ctrl_regs.h"       // Generated.
#include "hw/top/pwrmgr_regs.h"         // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "hw/top/rv_plic_regs.h"        // Generated.
#include "sw/device/lib/base/multibits.h"

enum {
  kPolls = 200000,
  kAonHz = 200000,
  kClassStride =
      ALERT_HANDLER_CLASSB_REGWEN_REG_OFFSET - ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET,
  // alert_crashdump_t, least significant member first: four 3-bit class states, four escalation
  // counters, four accumulators, the local alert causes, the alert causes.
  kDumpAccumPos = 4 * 3 + 4 * ALERT_HANDLER_PARAM_ESC_CNT_DW,
  kDumpLocCausePos = kDumpAccumPos + 4 * ALERT_HANDLER_PARAM_ACCU_CNT_DW,
  kDumpCausePos = kDumpLocCausePos + ALERT_HANDLER_PARAM_N_LOC_ALERT,
  // The DAI's error code: after the partitions' (otp_ctrl registers: ERR_CODE_<NumPart>).
  kOtpDaiErrCode = OTP_CTRL_ERR_CODE_0_REG_OFFSET + 4 * OTP_CTRL_PARAM_NUM_PART,
};

uint32_t sys_aon_cycles(uint32_t us) { return (uint32_t)(((uint64_t)us * kAonHz) / 1000000u); }

static void aon_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevAonTimer, off, v); }
static void pm_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevPwrmgr, off, v); }
static uint32_t pm_rd(uint32_t off) { return eg2_dev_read32(kEg2DevPwrmgr, off); }

/** pwrmgr's slow-domain registers take effect after CFG_CDC_SYNC (programmer's guide). */
static bool pm_sync(void) {
  pm_wr(PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  for (uint32_t i = 0; i < kPolls; ++i) {
    if ((pm_rd(PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1) == 0) {
      return true;
    }
  }
  return false;
}

void sys_wdog_bite_after(uint32_t bite) {
  // The watchdog's reset request reaches the chip only with pwrmgr's RESET_EN for aon_timer.
  pm_wr(PWRMGR_RESET_EN_REG_OFFSET,
        pm_rd(PWRMGR_RESET_EN_REG_OFFSET) | (1u << PWRMGR_RESET_EN_EN_1_BIT));
  (void)pm_sync();
  aon_wr(AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 0xffffffffu);
  aon_wr(AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, bite);
  aon_wr(AON_TIMER_WDOG_CTRL_REG_OFFSET, 1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
}

void sys_wdog_off(void) {
  aon_wr(AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
}

void sys_wkup_after(uint32_t cycles) {
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, cycles);
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
}

void sys_wkup_clear(void) {
  aon_wr(AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
  aon_wr(AON_TIMER_WKUP_CAUSE_REG_OFFSET, 0);
  aon_wr(AON_TIMER_INTR_STATE_REG_OFFSET, 1u << AON_TIMER_INTR_STATE_WKUP_TIMER_EXPIRED_BIT);
}

bool sys_low_power_arm(uint32_t wake_mask, bool deep) {
  pm_wr(PWRMGR_WAKEUP_EN_REG_OFFSET, wake_mask);
  // Clocks off in low power (CORE_CLK_EN, IO_CLK_EN, USB_CLK_EN_LP 0), USB on when active.
  pm_wr(PWRMGR_CONTROL_REG_OFFSET, (1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT) |
                                       (1u << PWRMGR_CONTROL_USB_CLK_EN_ACTIVE_BIT) |
                                       (deep ? 0 : 1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
  return pm_sync();
}

void sys_low_power_disarm(void) {
  pm_wr(PWRMGR_CONTROL_REG_OFFSET, (1u << PWRMGR_CONTROL_USB_CLK_EN_ACTIVE_BIT) |
                                       (1u << PWRMGR_CONTROL_MAIN_PD_N_BIT));
  (void)pm_sync();
}

static uint32_t wake_plic_id(void) {
  return (uint32_t)dt_pwrmgr_irq_to_plic_id(kDtPwrmgr, kDtPwrmgrIrqWakeup);
}

void sys_wake_irq_on(void) {
  uint32_t id = wake_plic_id();
  __asm__ volatile("csrci mstatus, 8");
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * id, 1);
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_THRESHOLD0_REG_OFFSET, 0);
  uint32_t ie = RV_PLIC_IE0_0_REG_OFFSET + 4 * (id / 32);
  eg2_dev_write32(kEg2DevRvPlic, ie, eg2_dev_read32(kEg2DevRvPlic, ie) | (1u << (id % 32)));
  pm_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 1u << PWRMGR_INTR_ENABLE_WAKEUP_BIT);
  // mie.MEIE: a pending external interrupt ends the WFI; mstatus.MIE stays 0, so no trap.
  uint32_t meie = 1u << 11;
  __asm__ volatile("csrs mie, %0" : : "r"(meie));
}

uint32_t sys_wake_irq_ack(void) {
  pm_wr(PWRMGR_INTR_STATE_REG_OFFSET, 1u << PWRMGR_INTR_STATE_WAKEUP_BIT);
  uint32_t id = eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
  if (id != 0) {
    eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, id);
  }
  uint32_t meie = 1u << 11;
  __asm__ volatile("csrc mie, %0" : : "r"(meie));
  pm_wr(PWRMGR_INTR_ENABLE_REG_OFFSET, 0);
  return id;
}

uint32_t sys_wake_info(bool clear) {
  uint32_t v = pm_rd(PWRMGR_WAKE_INFO_REG_OFFSET);
  if (clear) {
    pm_wr(PWRMGR_WAKE_INFO_REG_OFFSET, v);
  }
  return v;
}

uint32_t sys_crashdump_read(uint32_t *dump, uint32_t max_words) {
  uint32_t n = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_ATTR_REG_OFFSET) &
               RSTMGR_ALERT_INFO_ATTR_CNT_AVAIL_MASK;
  for (uint32_t i = 0; i < n && i < max_words; ++i) {
    eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                    i << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
    dump[i] = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_REG_OFFSET);
  }
  return n;
}

void sys_crashdump_enable(void) {
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);
}

static uint32_t dump_bits(const uint32_t *dump, uint32_t pos, uint32_t width) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < width; ++i) {
    uint32_t p = pos + i;
    v |= ((dump[p / 32] >> (p % 32)) & 1u) << i;
  }
  return v;
}

uint32_t sys_dump_state(const uint32_t *dump, uint32_t cls) {
  return dump_bits(dump, 3 * cls, 3);
}

uint32_t sys_dump_accum(const uint32_t *dump, uint32_t cls) {
  return dump_bits(dump, kDumpAccumPos + ALERT_HANDLER_PARAM_ACCU_CNT_DW * cls,
                   ALERT_HANDLER_PARAM_ACCU_CNT_DW);
}

bool sys_dump_cause(const uint32_t *dump, uint32_t alert) {
  return dump_bits(dump, kDumpCausePos + alert, 1) != 0;
}

uint32_t sys_class_esc_cnt(uint32_t cls) {
  return eg2_dev_read32(kEg2DevAlertHandler,
                        ALERT_HANDLER_CLASSA_ESC_CNT_REG_OFFSET + cls * kClassStride);
}

uint32_t sys_class_clr_regwen(uint32_t cls) {
  return eg2_dev_read32(kEg2DevAlertHandler,
                        ALERT_HANDLER_CLASSA_CLR_REGWEN_REG_OFFSET + cls * kClassStride) &
         1u;
}

uint32_t sys_lc_state(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_STATE_REG_OFFSET) &
         LC_CTRL_LC_STATE_STATE_MASK;
}

uint32_t sys_lc_count(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_TRANSITION_CNT_REG_OFFSET) &
         LC_CTRL_LC_TRANSITION_CNT_CNT_MASK;
}

uint32_t sys_lc_status(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_STATUS_REG_OFFSET);
}

const char *sys_lc_name(uint32_t s) {
  switch (s) {
    case LC_CTRL_LC_STATE_STATE_VALUE_DEV:
      return "DEV";
    case LC_CTRL_LC_STATE_STATE_VALUE_PROD:
      return "PROD";
    case LC_CTRL_LC_STATE_STATE_VALUE_RMA:
      return "RMA";
    case LC_CTRL_LC_STATE_STATE_VALUE_SCRAP:
      return "SCRAP";
    case LC_CTRL_LC_STATE_STATE_VALUE_ESCALATE:
      return "ESCALATE";
    case LC_CTRL_LC_STATE_STATE_VALUE_POST_TRANSITION:
      return "POST_TRANSITION";
    default:
      return "other";
  }
}

bool sys_lc_request(uint32_t target, const uint32_t *token) {
  eg2_dev_write32(kEg2DevLcCtrlRegs, LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET, kMultiBitBool8True);
  if ((eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_CLAIM_TRANSITION_IF_REG_OFFSET) & 0xff) !=
          kMultiBitBool8True ||
      (eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_TRANSITION_REGWEN_REG_OFFSET) & 1) == 0) {
    return false;
  }
  eg2_dev_write32(kEg2DevLcCtrlRegs, LC_CTRL_TRANSITION_TARGET_REG_OFFSET, target);
  for (uint32_t i = 0; i < 4; ++i) {
    eg2_dev_write32(kEg2DevLcCtrlRegs, LC_CTRL_TRANSITION_TOKEN_0_REG_OFFSET + 4 * i, token[i]);
  }
  eg2_dev_write32(kEg2DevLcCtrlRegs, LC_CTRL_TRANSITION_CMD_REG_OFFSET,
                  1u << LC_CTRL_TRANSITION_CMD_START_BIT);
  return true;
}

bool sys_otp_dai_idle(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_STATUS_REG_OFFSET) &
        (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      return true;
    }
  }
  return false;
}

uint32_t sys_otp_read32(uint32_t addr, uint32_t *value) {
  *value = 0;
  if (!sys_otp_dai_idle()) {
    return 0xffffffffu;
  }
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                  1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  if (!sys_otp_dai_idle()) {
    return 0xffffffffu;
  }
  uint32_t st = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_STATUS_REG_OFFSET) & 0xffu;
  *value = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  return st;
}

void sys_otp_write32_start(uint32_t addr, uint32_t value) {
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET, value);
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
                  1u << OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT);
}

uint32_t sys_otp_dai_err(void) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, kOtpDaiErrCode) & 0x7u;
}

uint32_t sys_otp_window32(uint32_t addr) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + addr);
}
