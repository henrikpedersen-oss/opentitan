// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_rv_dm_lc_disabled_cheriot (EG2 verification spec REQ_CDM_05, REQ_PBF_20, REQ_SEC_04),
 * CHERIoT mode only: the chip_rv_dm_lc_disabled sweep with the core running in CHERIoT mode. The
 * debug module and its SBA are usable only in TEST_UNLOCKED0-7, DEV and RMA and disabled in every
 * other life-cycle state, whatever the core's mode.
 *
 * Only the life-cycle states in which the core runs can have a core in CHERIoT mode: TEST_UNLOCKED
 * 0-7, DEV, PROD, PROD_END and RMA (lc_ctrl's CPU_EN). RAW, TEST_LOCKED*, SCRAP and INVALID run no
 * code; chip_rv_dm_lc_disabled covers them on the stub CPU.
 *
 * The documents (rv_dm.hjson, DM_EN.CTRL.LC_GATED and LATE_DEBUG_ENABLE; lc_ctrl theory of
 * operation, HW_DEBUG_EN): the JTAG DMI is enabled in TEST_UNLOCKED0-7, DEV and RMA; the debug
 * memory's TL-UL port in TEST_UNLOCKED0-7 and RMA, and in DEV when OTP's DIS_RV_DM_LATE_DEBUG is
 * MuBi8True or LATE_DEBUG_ENABLE is MuBi32True. A gated debug memory answers with d_error: a load
 * access fault (mcause 5) on the core.
 *
 * The bench writes the life-cycle state and DIS_RV_DM_LATE_DEBUG into OTP before reset and the
 * expectations as knobs (env/seq_lib/chip_eg2_rv_dm_lc_disabled_vseq.sv). The software:
 *  1. writes LATE_DEBUG_ENABLE (MuBi32True if late_debug_sw, else MuBi32False) and reads it back;
 *  2. checks LC_STATE against the bench's state;
 *  3. makes n_loads loads from the debug memory (its ROM, offset 0x800 on) and checks each faults
 *     exactly when the debug memory is gated;
 *  4. prints `EG2 DEBUG lc ready` and stays in CHERIoT mode for hold_cycles while the bench makes
 *     its JTAG checks (dmactive, then a write and read-back of sbaddress0: all-zero when the DMI
 *     is gated, the written value otherwise).
 *
 * Knobs (the vseq writes them):
 *   lc_state_dec    the decoded life-cycle state the bench wrote (lc_ctrl LC_STATE's 5-bit code)
 *   late_debug_sw   1: LATE_DEBUG_ENABLE = MuBi32True
 *   exp_mem_gated   1: the debug memory must refuse the core's loads
 *   n_loads         loads from the debug memory
 *   hold_cycles     core cycles the software waits after its ready line
 *   model_gaps      skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1               | Why                                            |
 * |-------------------------------------------|------------------------------------------------|
 * | the debug memory loads                    | eg2sim has no debug module                     |
 * | LATE_DEBUG_ENABLE's read-back             | eg2sim has no debug module                     |
 *
 * Built with EG2_RV_DM_LC_DISABLED_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `DEBUG FAIL`.
 */

#include "hw/top/lc_ctrl_regs.h"  // Generated.
#include "hw/top/rv_dm_regs.h"    // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_debug/eg2_debug.h"

#ifndef __CHERIOT__
#error "chip_rv_dm_lc_disabled_cheriot is a CHERIoT-mode test"
#endif

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define LC_DISABLED_PARAMS(X)           \
  X(lc_state_dec, 19, 0, 31)            \
  X(late_debug_sw, 0, 0, 1)             \
  X(exp_mem_gated, 0, 0, 1)             \
  X(n_loads, 8, 1, 64)                  \
  X(hold_cycles, 200000, 0, 100000000)  \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(LC_DISABLED_PARAMS)

enum {
  kDmRomOffset = 0x800,
  kDmRomBytes = 0x800,
};

bool eg2_test_main(void) {
  eg2_chk_area("DEBUG");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = true;

  // 1. LATE_DEBUG_ENABLE (rv_dm.regs is never gated).
  const uint32_t late = EG2_PARAM(late_debug_sw) ? kMultiBitBool32True : kMultiBitBool32False;
  eg2_dev_write32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, late);
  if (gaps) {
    eg2_chk_skip("LATE_DEBUG_ENABLE read back (no debug module)");
  } else {
    ok = eg2_chk_eq("LATE_DEBUG_ENABLE read back", late,
                    eg2_dev_read32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET)) &&
         ok;
  }

  // 2. The life-cycle state the bench wrote: its 5-bit code in each of LC_STATE's six fields.
  const uint32_t dec = EG2_PARAM(lc_state_dec) & 0x1f;
  uint32_t exp_state = 0;
  for (uint32_t i = 0; i < 6; ++i) {
    exp_state |= dec << (5 * i);
  }
  ok = eg2_chk_eq("LC_STATE (the bench's state)", exp_state,
                  eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_STATE_REG_OFFSET) &
                      LC_CTRL_LC_STATE_STATE_MASK) &&
       ok;

  // 3. The core's loads from the debug memory.
  const bool gated = EG2_PARAM(exp_mem_gated) != 0;
  eg2_chk_step("debug memory gated (expected)", gated);
  for (uint32_t i = 0; i < EG2_PARAM(n_loads); ++i) {
    uint32_t addr = TOP_EARLGREY_RV_DM_MEM_BASE_ADDR + kDmRomOffset +
                    4 * eg2_rand_range(0, kDmRomBytes / 4 - 1);
    if (gaps) {
      eg2_chk_skip("debug memory load (no debug module)");
      continue;
    }
    eg2_chk_trap_t t;
    uint32_t cause = eg2_chk_access(eg2_chk_ptr(addr), kEg2ChkLw, 0, &t);
    eg2_chk_step("debug memory load at", addr);
    ok = eg2_chk_eq(gated ? "debug memory load refused (load access fault)"
                          : "debug memory load allowed (no trap)",
                    gated ? kEg2ChkCauseLoadFault : 0, cause) &&
         ok;
  }

  // 4. The bench's JTAG checks, while the core stays in CHERIoT mode.
  LOG_INFO("EG2 DEBUG lc ready");
  eg2_chk_delay(EG2_PARAM(hold_cycles));
  return eg2_chk_summary(1 + (gaps ? 0 : 1 + EG2_PARAM(n_loads))) && ok;
}
