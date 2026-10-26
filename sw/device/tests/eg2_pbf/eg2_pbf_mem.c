// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/dt/api.h"         // Generated.
#include "hw/top/dt/rram_ctrl.h"   // Generated.
#include "hw/top/lc_ctrl_regs.h"   // Generated.
#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"    // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/dif/dif_rram_ctrl.h"
#include "sw/device/lib/testing/rram_ctrl_testutils.h"

enum {
  kPolls = 200000,
  kBenchSpin = 64,
  kRegions = 10,
  // The alert crash dump (alert_crashdump_t), from the least significant member: the class
  // states, escalation counters, accumulators, local alert causes, alert causes (alert handler
  // theory of operation, "Crash Dump").
  kDumpStateBits = 3,
  kDumpCausePos = 4 * kDumpStateBits + 4 * ALERT_HANDLER_PARAM_ESC_CNT_DW +
                  4 * ALERT_HANDLER_PARAM_ACCU_CNT_DW + ALERT_HANDLER_PARAM_N_LOC_ALERT,
};

// The OTTF non-volatile scratch area (ottf_common.ld), by address: a 32-bit constant, so the
// CHERIoT build reaches it through a window capability like any other bus address.
extern char _non_volatile_scratch_start[];
extern char _non_volatile_scratch_end[];
static const volatile uint32_t kNvScratch[2] = {
    (uint32_t)(uintptr_t)_non_volatile_scratch_start,
    (uint32_t)(uintptr_t)_non_volatile_scratch_end,
};

uint32_t eg2_pbf_nv_scratch(void) { return kNvScratch[0]; }
uint32_t eg2_pbf_nv_scratch_size(void) { return kNvScratch[1] - kNvScratch[0]; }

void eg2_pbf_bench_step(uint32_t step) {
  eg2_puts("PBF bench step ");
  eg2_putdec(step);
  eg2_puts("\r\n");
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET,
                  kEg2PbfBenchCode | (step & 0xff));
  eg2_pbf_spin(kBenchSpin);
  eg2_result_in_test();
}

uint32_t eg2_pbf_lc_state(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_STATE_REG_OFFSET);
}

const char *eg2_pbf_lc_name(uint32_t lc) {
  switch (lc) {
    case LC_CTRL_LC_STATE_STATE_VALUE_DEV:
      return "DEV";
    case LC_CTRL_LC_STATE_STATE_VALUE_PROD:
      return "PROD";
    case LC_CTRL_LC_STATE_STATE_VALUE_PROD_END:
      return "PROD_END";
    case LC_CTRL_LC_STATE_STATE_VALUE_RMA:
      return "RMA";
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED0:
      return "TEST_UNLOCKED0";
    default:
      return "other";
  }
}

bool eg2_pbf_lc_debug_enabled(uint32_t lc) {
  // lc_ctrl theory of operation, the life cycle signal table: HW_DEBUG_EN in the TEST_UNLOCKED
  // states, DEV and RMA; off in PROD, PROD_END and the locked, RAW and SCRAP states.
  if (lc == LC_CTRL_LC_STATE_STATE_VALUE_DEV || lc == LC_CTRL_LC_STATE_STATE_VALUE_RMA) {
    return true;
  }
  // TEST_UNLOCKED0..7 are the odd states 1, 3, ..., 15, each a 5-bit value repeated 6 times.
  uint32_t s = lc & 0x1f;
  return s <= 15 && (s & 1) && lc == s * 0x2108421u;
}

static dif_rram_ctrl_state_t rram;

static bool rram_init(void) {
  if (dif_rram_ctrl_init_state_from_dt(&rram, kDtRramCtrl) != kDifOk) {
    return false;
  }
  return status_ok(rram_ctrl_testutils_wait_for_init(&rram));
}

bool eg2_pbf_rram_region(uint32_t addr, uint32_t npages, bool scramble, bool ecc) {
  if (!rram_init()) {
    return false;
  }
  for (int r = kRegions - 1; r >= 0; --r) {
    uint32_t regwen = eg2_dev_read32(kEg2DevRramCtrlCore,
                                     RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * (uint32_t)r);
    dif_toggle_t en = kDifToggleDisabled;
    if ((regwen & 1) == 0 ||
        dif_rram_ctrl_get_data_region_enablement(&rram, (uint32_t)r, &en) != kDifOk ||
        en == kDifToggleEnabled) {
      continue;
    }
    dif_rram_ctrl_data_region_properties_t cfg = {
        .base = (addr - TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR) / kEg2PbfRramPage,
        .size = npages - 1,  // SIZE = pages - 1 (programmer's guide)
        .properties =
            {
                .rd_en = kMultiBitBool4True,
                .wr_en = kMultiBitBool4True,
                .scramble_en = scramble ? kMultiBitBool4True : kMultiBitBool4False,
                .ecc_en = ecc ? kMultiBitBool4True : kMultiBitBool4False,
            },
    };
    if (dif_rram_ctrl_set_data_region_properties(&rram, (uint32_t)r, cfg) != kDifOk ||
        dif_rram_ctrl_set_data_region_enablement(&rram, (uint32_t)r, kDifToggleEnabled) !=
            kDifOk) {
      return false;
    }
    eg2_pbf_note("rram data region", (uint32_t)r);
    return true;
  }
  return false;
}

bool eg2_pbf_rram_program(uint32_t addr, const uint32_t *words, uint32_t n) {
  return rram_init() &&
         status_ok(rram_ctrl_testutils_write(&rram, addr - TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
                                             words, kDifRramCtrlPartitionTypeData, n));
}

bool eg2_pbf_rram_ctrl_read(uint32_t addr, uint32_t *words, uint32_t n) {
  return rram_init() &&
         status_ok(rram_ctrl_testutils_read(&rram, addr - TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
                                            words, kDifRramCtrlPartitionTypeData, n, 0));
}

bool eg2_pbf_rram_rewrite(uint32_t addr) {
  if (!rram_init()) {
    return false;
  }
  dif_rram_ctrl_transaction_t t = {
      .op = kDifRramCtrlOpRewrite,
      .partition_type = kDifRramCtrlPartitionTypeData,
      .byte_address = addr - TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
      .word_count = 1,
  };
  return dif_rram_ctrl_start(&rram, t) == kDifOk &&
         status_ok(rram_ctrl_testutils_wait_transaction_end(&rram));
}

uint32_t eg2_pbf_rram_err_code(void) {
  uint32_t e = eg2_dev_read32(kEg2DevRramCtrlCore, RRAM_CTRL_ERR_CODE_REG_OFFSET);
  eg2_dev_write32(kEg2DevRramCtrlCore, RRAM_CTRL_ERR_CODE_REG_OFFSET, e);
  return e;
}

bool eg2_pbf_sram_op(eg2_dev_id_t regs, bool renew_key, bool init) {
  uint32_t ctrl = (renew_key ? 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT : 0) |
                  (init ? 1u << SRAM_CTRL_CTRL_INIT_BIT : 0);
  eg2_dev_write32(regs, SRAM_CTRL_CTRL_REG_OFFSET, ctrl);
  uint32_t want = (renew_key ? 1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT : 0) |
                  (init ? 1u << SRAM_CTRL_STATUS_INIT_DONE_BIT : 0);
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t st = eg2_dev_read32(regs, SRAM_CTRL_STATUS_REG_OFFSET);
    if (st & (1u << SRAM_CTRL_STATUS_INIT_ERROR_BIT)) {
      return false;
    }
    if ((st & want) == want) {
      return true;
    }
  }
  return false;
}

void eg2_pbf_crashdump_arm(void) {
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_CPU_INFO_CTRL_REG_OFFSET, 1u << RSTMGR_CPU_INFO_CTRL_EN_BIT);
}

bool eg2_pbf_crashdump_alert(size_t alert) {
  uint32_t pos = kDumpCausePos + (uint32_t)alert;
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  (pos / 32) << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
  uint32_t w = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_REG_OFFSET);
  return (w >> (pos % 32)) & 1;
}

uint32_t eg2_pbf_crashdump_cpu(uint32_t i) {
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_CPU_INFO_CTRL_REG_OFFSET,
                  i << RSTMGR_CPU_INFO_CTRL_INDEX_OFFSET);
  return eg2_dev_read32(kEg2DevRstmgr, RSTMGR_CPU_INFO_REG_OFFSET);
}

void eg2_pbf_escalate_reset(const uint32_t *alerts, size_t n, uint32_t cycles) {
  eg2_pbf_esc_t esc = {
      .cls = kEg2PbfClassA,
      .accum_thresh = 0,
      .phase_cycles = {cycles, 10, 10, 10},
      // CTRL.EN_Ex enables escalation signal x, MAP_Ex the phase it is asserted in (alert handler
      // registers): signal 3, the escalation reset, in phase 0; no other signal (no NMI).
      .signal_en = 1u << 3,
      .map = {0, 1, 2, 0},
      .crashdump_phase = 0,
      .lock = true,
  };
  eg2_pbf_escalate(&esc, alerts, n);
}

uint32_t eg2_pbf_alert(const char *name) {
  int a = eg2_status_alert_find(name);
  if (a < 0) {
    eg2_pbf_step(name, false);
    return 0;
  }
  return (uint32_t)a;
}
