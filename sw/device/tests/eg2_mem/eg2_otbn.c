// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_mem/eg2_otbn.h"

uint32_t eg2_otbn_reg(uint32_t off) { return eg2_dev_read32(kEg2DevOtbn, off); }
void eg2_otbn_reg_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevOtbn, off, v); }

bool eg2_otbn_wait_idle(uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    if ((eg2_otbn_reg(OTBN_STATUS_REG_OFFSET) & 0xff) == kEg2OtbnStatusIdle) {
      return true;
    }
  }
  return false;
}

uint32_t eg2_otbn_imem_bytes(const eg2_otbn_app_t *app) { return 4 * app->imem_words; }

void eg2_otbn_load(const eg2_otbn_app_t *app, uint32_t dmem_clear) {
  for (uint32_t i = 0; i < app->imem_words; ++i) {
    eg2_otbn_reg_wr(OTBN_IMEM_REG_OFFSET + 4 * i, app->imem[i]);
  }
  for (uint32_t i = 0; i < app->dmem_words; ++i) {
    eg2_otbn_reg_wr(OTBN_DMEM_REG_OFFSET + 4 * i, app->dmem[i]);
  }
  for (uint32_t a = 4 * app->dmem_words; a < dmem_clear; a += 4) {
    eg2_otbn_reg_wr(OTBN_DMEM_REG_OFFSET + a, 0);
  }
}

uint32_t eg2_otbn_run(uint32_t polls) {
  eg2_otbn_reg_wr(OTBN_INTR_STATE_REG_OFFSET, 1u << OTBN_INTR_STATE_DONE_BIT);
  eg2_otbn_reg_wr(OTBN_CMD_REG_OFFSET, kEg2OtbnCmdExecute);
  if (!eg2_otbn_wait_idle(polls)) {
    return 0xffffffffu;
  }
  return eg2_otbn_reg(OTBN_ERR_BITS_REG_OFFSET);
}

void eg2_otbn_dmem_read(uint32_t addr, uint32_t *out, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    out[i] = eg2_otbn_reg(OTBN_DMEM_REG_OFFSET + addr + 4 * i);
  }
}

uint32_t eg2_otbn_dmem_word(uint32_t addr) { return eg2_otbn_reg(OTBN_DMEM_REG_OFFSET + addr); }
uint32_t eg2_otbn_imem_word(uint32_t addr) { return eg2_otbn_reg(OTBN_IMEM_REG_OFFSET + addr); }

bool eg2_otbn_sec_wipe(uint32_t cmd, uint32_t polls) {
  eg2_otbn_reg_wr(OTBN_CMD_REG_OFFSET, cmd);
  return eg2_otbn_wait_idle(polls);
}
