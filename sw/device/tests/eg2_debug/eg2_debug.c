// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_debug/eg2_debug.h"

#include "hw/top/rv_dm_regs.h"  // Generated.
#include "sw/device/lib/base/multibits.h"

// Written by the bench over SBA. In .bss: main SRAM in both modes.
static volatile uint32_t done_;
static volatile uint32_t scratch_;

volatile uint32_t *eg2_debug_scratch(void) { return &scratch_; }

bool eg2_debug_run(const eg2_debug_cfg_t *cfg) {
  bool ok = true;
  if (cfg->late_debug_sw) {
    eg2_dev_write32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, kMultiBitBool32True);
    ok = eg2_chk_eq("late debug enable", kMultiBitBool32True,
                    eg2_dev_read32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET)) &&
         ok;
  }
  done_ = 0;
  scratch_ = 0;
  const uint32_t magic = kEg2DebugMagic + cfg->test_id;
  LOG_INFO("EG2 DEBUG ready 0x%08x 0x%08x", eg2_chk_addr_of(&done_), eg2_chk_addr_of(&scratch_));

  const uint32_t limit = cfg->model_gaps ? cfg->model_iters : cfg->max_iters;
  uint32_t sum = 0;
  uint32_t n = 0;
  while (n < limit) {
    sum += n;
    ++n;
    if (!cfg->model_gaps && done_ != 0) {
      break;
    }
  }
  eg2_chk_step("loop iterations", n);
  // The loop's own result, whatever halts and resumes happened in between.
  uint32_t exp_sum = (uint32_t)(((uint64_t)n * (uint64_t)(n - 1)) / 2);
  ok = eg2_chk_eq("loop sum", exp_sum, sum) && ok;
  if (cfg->model_gaps) {
    ok = eg2_chk_eq("loop iterations without the bench", cfg->model_iters, n) && ok;
    eg2_chk_skip("the bench's JTAG and SBA accesses and the done word (no debug module)");
    return ok;
  }
  ok = eg2_chk_eq("done word written over SBA", magic, done_) && ok;
  ok = eg2_chk_true("loop made progress before done", n >= cfg->min_iters) && ok;
  return ok;
}
