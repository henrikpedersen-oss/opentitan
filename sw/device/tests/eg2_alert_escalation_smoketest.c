// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * Alert escalation through the NMI, the life-cycle escalation and the escalation reset, with the
 * crash dump (REQ_PBF_17). RISC-V mode only: the CHERIoT build's start-up code ends the test on
 * any trap, an NMI included.
 *
 * Boot 1 (power-on): rv_core_ibex's fatal_sw_err and fatal_hw_err go to class A, which escalates on
 * the first alert (threshold 0) and cannot be stopped (CTRL.LOCK): phase 0 the alert NMI
 * (escalation signal 0, NMI_ENABLE.ALERT_EN), phases 1 and 2 the life-cycle escalation (signals 1
 * and 2: lc_ctrl's ESCALATE state, which disables the core's fetch), phase 3 the escalation reset
 * (signal 3, pwrmgr). The crash dump is latched on entering phase 1 (CRASHDUMP_TRIGGER) and
 * captured by rstmgr at the reset (ALERT_INFO_CTRL.EN). The test raises fatal_sw_err through
 * ALERT_TEST (`escalation_trigger`); the NMI handler counts the NMI in retention SRAM and waits for
 * the reset.
 *
 * Boot 2 (escalation reset): the reset reason is the escalation's alone, the NMI came exactly once,
 * and rstmgr's alert crash dump holds fatal_sw_err's cause, class A's count 1 and Phase1;
 * ALERT_INFO_CTRL.EN is clear again.
 *
 * A fault injected after the configuration (eg2sim --fault lockstep at `escalation_trigger`) makes
 * fatal_hw_err the alert that escalates and halts the core before the NMI: boot 2 then fails.
 */

#include "hw/top/alert_handler_regs.h"
#include "hw/top/rstmgr_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/dif/dif_rstmgr.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/lib/testing/ret_sram_testutils.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/lib/testing/test_framework/ottf_isrs.h"

EG2_TEST_CONFIG();

enum {
  // Retention SRAM counter: NMIs taken from the alert escalation.
  kNmiCounter = 0,
  // Class A's escalation phases, in alert handler (io_div4) cycles. Phase 0 is long enough for
  // the NMI handler to print its line (the UART's 10 us per character).
  kPhase0Cycles = 24000,
  kPhase1Cycles = 100,
  kPhase2Cycles = 100,
  kPhase3Cycles = 100,
  // Polls of the NMI counter before boot 1 gives up waiting.
  kNmiPolls = 2000000,
  // The crash dump (alert_crashdump_t): bit positions, from the least significant member.
  kDumpStateBits = 3,
  kDumpEscCntBits = ALERT_HANDLER_PARAM_ESC_CNT_DW,
  kDumpAccumBits = ALERT_HANDLER_PARAM_ACCU_CNT_DW,
  kDumpAccumPos = 4 * kDumpStateBits + 4 * kDumpEscCntBits,
  kDumpLocCausePos = kDumpAccumPos + 4 * kDumpAccumBits,
  kDumpCausePos = kDumpLocCausePos + ALERT_HANDLER_PARAM_N_LOC_ALERT,
  kDumpBits = kDumpCausePos + ALERT_HANDLER_PARAM_N_ALERTS,
  kDumpWords = (kDumpBits + 31) / 32,
  kPhase1State = 0x5,  // CLASSA_STATE Phase1
};

static bool step(const char *name, bool ok) {
  eg2_puts(ok ? "step ok: " : "step FAILED: ");
  eg2_puts(name);
  eg2_puts("\r\n");
  return ok;
}

static void report(const char *what, uint32_t value) {
  eg2_puts(what);
  eg2_puts(" = 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

static void ah_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

static uint32_t nmi_count(void) {
  uint32_t n = 0;
  if (!status_ok(ret_sram_testutils_counter_get(kNmiCounter, &n))) {
    return 0xffffffff;
  }
  return n;
}

/** Escalation signal 0: count it, then wait for the life-cycle escalation and the reset. */
void ottf_external_nmi_handler(uint32_t *exc_info) {
  (void)exc_info;
  uint32_t state = eg2_dev_read32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_STATE_REG_OFFSET);
  if (state & (1u << RV_CORE_IBEX_NMI_STATE_ALERT_BIT)) {
    (void)ret_sram_testutils_counter_increment(kNmiCounter);
  }
  report("NMI from alert escalation, NMI_STATE", state);
  while (true) {
  }
}

/** Class A: the first alert escalates, through every phase, and cannot be cleared. */
static void configure_class_a(size_t alert_sw, size_t alert_hw) {
  for (size_t i = 0; i < 2; ++i) {
    uint32_t a = (uint32_t)(i == 0 ? alert_sw : alert_hw);
    ah_write(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * a,
             ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
    ah_write(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * a, 1);
  }
  ah_write(ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET, kPhase0Cycles);
  ah_write(ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET, kPhase1Cycles);
  ah_write(ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET, kPhase2Cycles);
  ah_write(ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET, kPhase3Cycles);
  ah_write(ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET, 1);
  ah_write(ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
           (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_LOCK_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E0_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E1_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E2_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E3_BIT) |
               (0u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E0_OFFSET) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E1_OFFSET) |
               (2u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E2_OFFSET) |
               (3u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E3_OFFSET));
}

/** Raises the alert that escalates (a function of its own: a fault can be placed at it). */
static OT_NOINLINE void escalation_trigger(size_t alert) { eg2_status_raise_alert(alert); }

static bool boot_1(size_t alert_sw, size_t alert_hw) {
  bool ok = step("NMI counter cleared", status_ok(ret_sram_testutils_counter_clear(kNmiCounter)));
  configure_class_a(alert_sw, alert_hw);
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET,
                  1u << RV_CORE_IBEX_NMI_ENABLE_ALERT_EN_BIT);
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);
  eg2_puts("raising alert ");
  eg2_puts(eg2_status_alert_name(alert_sw));
  eg2_puts(": expecting the NMI, then the escalation reset\r\n");
  escalation_trigger(alert_sw);
  // The NMI handler never returns: the escalation reset ends this boot.
  for (uint32_t i = 0; i < kNmiPolls; ++i) {
    if (nmi_count() != 0) {
      break;
    }
  }
  return step("escalation reset before the NMI wait ends", false) && ok;
}

static bool boot_2(size_t alert_sw) {
  bool ok = step("the NMI came once", nmi_count() == 1);
  uint32_t ctrl = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET);
  ok = step("ALERT_INFO_CTRL.EN cleared by the reset",
            (ctrl & (1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT)) == 0) &&
       ok;
  uint32_t avail = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_ATTR_REG_OFFSET);
  report("ALERT_INFO_ATTR", avail);
  ok = step("crash dump size", avail == kDumpWords) && ok;
  uint32_t dump[kDumpWords];
  for (uint32_t i = 0; i < kDumpWords; ++i) {
    eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                    i << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
    dump[i] = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_REG_OFFSET);
    report("ALERT_INFO", dump[i]);
  }
  uint32_t state = dump[0] & 0x7;
  uint32_t accum = (dump[kDumpAccumPos / 32] >> (kDumpAccumPos % 32)) & 0xffff;
  uint32_t pos = kDumpCausePos + (uint32_t)alert_sw;
  bool cause = (dump[pos / 32] >> (pos % 32)) & 1;
  ok = step("crash dump: class A in Phase1", state == kPhase1State) && ok;
  ok = step("crash dump: class A count 1", accum == 1) && ok;
  ok = step("crash dump: the raised alert's cause", cause) && ok;
  return ok;
}

bool eg2_test_main(void) {
  ret_sram_testutils_init();
  int alert_sw = eg2_status_alert_find("rv_core_ibex_fatal_sw_err");
  int alert_hw = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (!step("alerts found", alert_sw >= 0 && alert_hw >= 0)) {
    return false;
  }
  // The test ROM moves RESET_INFO to retention SRAM and clears it.
  dif_rstmgr_reset_info_bitfield_t reason = rstmgr_testutils_reason_get();
  report("reset reason", reason);
  if (reason & kDifRstmgrResetInfoPor) {
    return boot_1((size_t)alert_sw, (size_t)alert_hw);
  }
  bool ok = step("reset reason: escalation", reason == kDifRstmgrResetInfoEscalation);
  return boot_2((size_t)alert_sw) && ok;
}
