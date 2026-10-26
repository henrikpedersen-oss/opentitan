// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_rram/eg2_rram.h"

uint32_t eg2_rram_reg(uint32_t offset) { return eg2_dev_read32(kEg2DevRramCtrlCore, offset); }

void eg2_rram_reg_wr(uint32_t offset, uint32_t value) {
  eg2_dev_write32(kEg2DevRramCtrlCore, offset, value);
}

volatile uint32_t *eg2_rram_host(uint32_t offset) {
  return eg2_chk_ptr(kEg2RramHostBase + offset);
}

bool eg2_rram_wait_init(uint32_t polls) {
  const uint32_t ready =
      (1u << RRAM_CTRL_STATUS_INIT_DONE_BIT) | (1u << RRAM_CTRL_STATUS_KEYS_VALID_BIT);
  if ((eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & ready) != ready) {
    eg2_rram_reg_wr(RRAM_CTRL_INIT_REG_OFFSET, 1u << RRAM_CTRL_INIT_VAL_BIT);
  }
  for (uint32_t i = 0; i < polls; ++i) {
    if ((eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & ready) == ready) {
      return true;
    }
  }
  return false;
}

static uint32_t mubi(uint32_t attrs, uint32_t bit) {
  return (attrs & bit) ? kEg2RramMubiTrue : kEg2RramMubiFalse;
}

uint32_t eg2_rram_region_cfg(uint32_t attrs) {
  return (uint32_t)kEg2RramMubiTrue << RRAM_CTRL_MP_REGION_CFG_0_EN_0_OFFSET |
         mubi(attrs, kEg2RramAttrRd) << RRAM_CTRL_MP_REGION_CFG_0_RD_EN_0_OFFSET |
         mubi(attrs, kEg2RramAttrWr) << RRAM_CTRL_MP_REGION_CFG_0_WR_EN_0_OFFSET |
         mubi(attrs, kEg2RramAttrScr) << RRAM_CTRL_MP_REGION_CFG_0_SCRAMBLE_EN_0_OFFSET |
         mubi(attrs, kEg2RramAttrEcc) << RRAM_CTRL_MP_REGION_CFG_0_ECC_EN_0_OFFSET;
}

uint32_t eg2_rram_default_cfg(uint32_t attrs) {
  return mubi(attrs, kEg2RramAttrRd) << RRAM_CTRL_DEFAULT_REGION_RD_EN_OFFSET |
         mubi(attrs, kEg2RramAttrWr) << RRAM_CTRL_DEFAULT_REGION_WR_EN_OFFSET |
         mubi(attrs, kEg2RramAttrScr) << RRAM_CTRL_DEFAULT_REGION_SCRAMBLE_EN_OFFSET |
         mubi(attrs, kEg2RramAttrEcc) << RRAM_CTRL_DEFAULT_REGION_ECC_EN_OFFSET;
}

uint32_t eg2_rram_cfg_attrs(uint32_t cfg, bool is_default) {
  // DEFAULT_REGION has no EN field: its attributes start at bit 0, the others' at bit 4.
  uint32_t sh = is_default ? 0 : 4;
  uint32_t a = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (((cfg >> (sh + 4 * i)) & 0xf) == kEg2RramMubiTrue) {
      a |= 1u << i;
    }
  }
  return a;
}

uint32_t eg2_rram_free_region(void) {
  for (uint32_t i = kEg2RramRegions; i-- > 0;) {
    uint32_t cfg = eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * i);
    uint32_t regwen = eg2_rram_reg(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * i);
    if ((cfg & 0xf) != kEg2RramMubiTrue && (regwen & 1) != 0) {
      return i;
    }
  }
  return kEg2RramRegions;
}

void eg2_rram_region_set(uint32_t idx, uint32_t base_page, uint32_t pages, uint32_t attrs) {
  // Disabled while the placement changes, so no half-written region is ever active.
  eg2_rram_region_off(idx);
  eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4 * idx,
                  (base_page & RRAM_CTRL_MP_REGION_0_BASE_0_MASK)
                          << RRAM_CTRL_MP_REGION_0_BASE_0_OFFSET |
                      ((pages - 1) & RRAM_CTRL_MP_REGION_0_SIZE_0_MASK)
                          << RRAM_CTRL_MP_REGION_0_SIZE_0_OFFSET);
  eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * idx, eg2_rram_region_cfg(attrs));
}

void eg2_rram_region_off(uint32_t idx) {
  eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * idx, RRAM_CTRL_MP_REGION_CFG_0_REG_RESVAL);
}

void eg2_rram_info_set(uint32_t page, uint32_t attrs) {
  eg2_rram_reg_wr(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * page, eg2_rram_region_cfg(attrs));
}

void eg2_rram_start(uint32_t op, uint32_t part, uint32_t byte_addr, uint32_t words) {
  eg2_rram_reg_wr(RRAM_CTRL_OP_STATUS_REG_OFFSET, 0);
  eg2_rram_reg_wr(RRAM_CTRL_ADDR_REG_OFFSET, byte_addr);
  uint32_t ctrl = (op & RRAM_CTRL_CONTROL_OP_MASK) << RRAM_CTRL_CONTROL_OP_OFFSET |
                  (part & 1) << RRAM_CTRL_CONTROL_PARTITION_BIT |
                  ((words - 1) & RRAM_CTRL_CONTROL_NUM_MASK) << RRAM_CTRL_CONTROL_NUM_OFFSET;
  // The fields first, then START with them (CONTROL.START: "only ... at the same time or after
  // the other fields").
  eg2_rram_reg_wr(RRAM_CTRL_CONTROL_REG_OFFSET, ctrl);
  eg2_rram_reg_wr(RRAM_CTRL_CONTROL_REG_OFFSET, ctrl | 1u << RRAM_CTRL_CONTROL_START_BIT);
}

bool eg2_rram_push(uint32_t word, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    if (!(eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & (1u << RRAM_CTRL_STATUS_WR_FULL_BIT))) {
      eg2_rram_reg_wr(RRAM_CTRL_WR_FIFO_REG_OFFSET, word);
      return true;
    }
  }
  return false;
}

bool eg2_rram_pop(uint32_t *word, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    if (!(eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & (1u << RRAM_CTRL_STATUS_RD_EMPTY_BIT))) {
      *word = eg2_rram_reg(RRAM_CTRL_RD_FIFO_REG_OFFSET);
      return true;
    }
  }
  return false;
}

bool eg2_rram_wait_done(eg2_rram_result_t *r, uint32_t polls) {
  for (uint32_t i = 0; i < polls; ++i) {
    uint32_t st = eg2_rram_reg(RRAM_CTRL_OP_STATUS_REG_OFFSET);
    if (st & (1u << RRAM_CTRL_OP_STATUS_DONE_BIT)) {
      r->op_status = st;
      r->err_code = eg2_rram_reg(RRAM_CTRL_ERR_CODE_REG_OFFSET);
      r->err_addr = eg2_rram_reg(RRAM_CTRL_ERR_ADDR_REG_OFFSET);
      eg2_rram_reg_wr(RRAM_CTRL_ERR_CODE_REG_OFFSET, r->err_code);
      eg2_rram_reg_wr(RRAM_CTRL_OP_STATUS_REG_OFFSET, 0);
      eg2_rram_reg_wr(RRAM_CTRL_INTR_STATE_REG_OFFSET, 1u << RRAM_CTRL_INTR_STATE_OP_DONE_BIT);
      return true;
    }
  }
  return false;
}

enum { kPolls = 200000 };

bool eg2_rram_write(uint32_t part, uint32_t byte_addr, const uint32_t *data, uint32_t words,
                    eg2_rram_result_t *r) {
  r->words = 0;
  eg2_rram_start(kEg2RramOpWrite, part, byte_addr, words);
  for (uint32_t i = 0; i < words; ++i) {
    if (!eg2_rram_push(data[i], kPolls)) {
      break;
    }
    ++r->words;
  }
  return eg2_rram_wait_done(r, kPolls);
}

bool eg2_rram_read(uint32_t part, uint32_t byte_addr, uint32_t *data, uint32_t words,
                   eg2_rram_result_t *r) {
  r->words = 0;
  eg2_rram_start(kEg2RramOpRead, part, byte_addr, words);
  for (uint32_t i = 0; i < words; ++i) {
    if (!eg2_rram_pop(&data[i], kPolls)) {
      break;
    }
    ++r->words;
  }
  return eg2_rram_wait_done(r, kPolls);
}

bool eg2_rram_rewrite(uint32_t part, uint32_t byte_addr, eg2_rram_result_t *r) {
  r->words = 0;
  eg2_rram_start(kEg2RramOpRewrite, part, byte_addr, 4);
  return eg2_rram_wait_done(r, kPolls);
}

void eg2_rram_clear_intrs(void) {
  eg2_rram_reg_wr(RRAM_CTRL_INTR_STATE_REG_OFFSET,
                  (1u << RRAM_CTRL_INTR_STATE_OP_DONE_BIT) |
                      (1u << RRAM_CTRL_INTR_STATE_CORR1_ERR_BIT) |
                      (1u << RRAM_CTRL_INTR_STATE_CORR2_ERR_BIT));
}

uint32_t eg2_rram_code_block(uint32_t *words, uint32_t nwords, uint32_t off, const uint32_t *insns,
                             uint32_t n) {
  if ((off & 1) != 0 || off + 4 * n + 2 > 4 * nwords) {
    return 0;
  }
  for (uint32_t i = 0; i < nwords; ++i) {
    words[i] = 0;  // c.unimp everywhere no code belongs
  }
  // Halfword stores into the word array, little-endian.
  for (uint32_t k = 0; k < 2 * n + 1; ++k) {
    uint32_t hw = k < 2 * n ? (insns[k / 2] >> (16 * (k & 1))) & 0xffff : kEg2RramInsnCJrRa;
    uint32_t pos = off / 2 + k;
    words[pos / 2] |= hw << (16 * (pos & 1));
  }
  return off + 4 * n + 2;
}

bool eg2_rram_icache_set(bool on) {
  uint32_t old;
  // cpuctrlsts (0x7c0), bit 0 icache_enable (Ibex CSR documentation).
  __asm__ volatile("csrr %0, 0x7c0" : "=r"(old));
  if (on) {
    __asm__ volatile("csrs 0x7c0, %0" ::"r"(1u));
  } else {
    __asm__ volatile("csrc 0x7c0, %0" ::"r"(1u));
  }
  __asm__ volatile("fence.i" ::: "memory");
  return (old & 1) != 0;
}
