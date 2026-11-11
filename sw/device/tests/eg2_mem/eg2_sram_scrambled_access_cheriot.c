// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_scrambled_access_cheriot: SRAM commands after the boot lock.
 *
 * CHERIoT start-up clears each SRAM controller's CTRL_REGWEN (REQ_SOC_08). The sram_ctrl
 * register description says this locks CTRL until reset. Write full-word patterns, request
 * INIT, key renewal, and both together, then require the data and status to stay unchanged.
 * This covers main, sec and retention SRAM without destroying the stack or boot record.
 *
 * Actual renewal, scrambling and INIT remain covered by the RISC-V
 * chip_sw_sram_ctrl_scrambled_access and sram_readback tests, before the lock is closed.
 * The CHERIoT test does not request a key or rely on a bench backdoor. All checks run with
 * model_gaps too. n_words chooses the pattern size; rekey adds attempts; bd_seed now seeds
 * the ordinary stored pattern, retaining the existing runner's parameter interface.
 *
 * The _inject_fail build flips the first expected value through eg2_chk and must report MEM FAIL.
 */
#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define SCR_PARAMS(X)               \
  X(n_words, 16, 16, 1024)          \
  X(rekey, 0, 0, 4)                 \
  X(bd_seed, 0x5eed, 0, 0xffffffff) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(SCR_PARAMS)

static uint32_t pattern(uint32_t word) {
  return EG2_PARAM(bd_seed) ^ (word * 0x9e3779b9u);
}

static bool locked_commands(eg2_dev_id_t regs, volatile uint32_t *words, uint32_t n) {
  bool ok = eg2_chk_eq("CTRL_REGWEN locked at boot", 0,
                       eg2_dev_read32(regs, SRAM_CTRL_CTRL_REGWEN_REG_OFFSET));
  for (uint32_t i = 0; i < n; ++i) words[i] = pattern(i);
  uint32_t status = eg2_dev_read32(regs, SRAM_CTRL_STATUS_REG_OFFSET);
  // The lock is rw0c: software cannot reopen it by writing one.
  eg2_dev_write32(regs, SRAM_CTRL_CTRL_REGWEN_REG_OFFSET, 1);
  ok = eg2_chk_eq("CTRL_REGWEN cannot reopen", 0,
                  eg2_dev_read32(regs, SRAM_CTRL_CTRL_REGWEN_REG_OFFSET)) && ok;
  for (uint32_t attempt = 0; attempt <= EG2_PARAM(rekey); ++attempt) {
    for (uint32_t command = 1; command <= 3; ++command) {
      uint32_t ctrl = ((command & 1) ? 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT : 0) |
                      ((command & 2) ? 1u << SRAM_CTRL_CTRL_INIT_BIT : 0);
      eg2_dev_write32(regs, SRAM_CTRL_CTRL_REG_OFFSET, ctrl);
      ok = eg2_chk_eq("locked CTRL leaves STATUS unchanged", status,
                      eg2_dev_read32(regs, SRAM_CTRL_STATUS_REG_OFFSET)) && ok;
      for (uint32_t i = 0; i < n; ++i) {
        ok = eg2_chk_eq("locked CTRL preserves SRAM word", pattern(i), words[i]) && ok;
      }
    }
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) return false;
  // A bounded stack buffer is owned by this test. Retention's creator and owner boot records
  // occupy the first half; use only the last KiB for its pattern.
  volatile uint32_t main_words[128];
  uint32_t n = EG2_PARAM(n_words);
  bool ok = locked_commands(kEg2DevSramCtrlMainRegs, main_words, n < 128 ? n : 128);
  ok = locked_commands(kEg2DevSramCtrlSecRegs,
                        eg2_chk_ptr(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR), n) && ok;
  ok = locked_commands(kEg2DevSramCtrlRetRegs,
                        eg2_chk_ptr(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR +
                                    TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES - 1024),
                        n < 256 ? n : 256) && ok;
  return eg2_chk_summary(6) && ok;
}
