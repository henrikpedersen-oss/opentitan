// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_readback (EG2 verification spec REQ_PBF_19, "Memory after boot": SRAM readback;
 * the chip_sram_ctrl testplan's chip_sw_sram_readback testpoint, in both modes): with the
 * readback mode enabled, then disabled, data written to the SRAM reads back; an INIT wipes it;
 * READBACK_REGWEN cleared locks READBACK (sram_ctrl registers.md: READBACK, READBACK_REGWEN rw0c,
 * STATUS.READBACK_ERROR; interfaces.md SRAM_CTRL.MEM.READBACK "Each read and write is checked with
 * a readback").
 *
 * On sec SRAM and retention SRAM, which the test owns (main SRAM holds its stack and globals and
 * is never initialised here). The retention SRAM's first bytes are the boot services' area
 * (retention_sram_t), which the INIT of this testpoint wipes as the legacy test does; nothing in
 * this run uses it.
 *
 * Knobs:
 *   mems         bit 0 sec SRAM, bit 1 retention SRAM (at least one)
 *   n_words      words written per memory and round (1-256)
 *   window       0: the memory's first words, 1: its last words, 2: drawn from the seed
 *   init_check   1: the INIT that must wipe the data (the testpoint's last step)
 *   model_gaps   skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1 | Why                                                       |
 * |-----------------------------|-----------------------------------------------------------|
 * | (nothing)                   | eg2sim's sram_ctrl keeps READBACK and its lock as storage, |
 * |                             |   INIT overwrites the RAM; the readback check itself (no   |
 * |                             |   READBACK_ERROR) holds trivially there                    |
 *
 * Built with EG2_SRAM_READBACK_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `MEM FAIL`.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define READBACK_PARAMS(X) \
  X(mems, 3, 1, 3)         \
  X(n_words, 16, 1, 256)   \
  X(window, 2, 0, 2)       \
  X(init_check, 1, 0, 1)   \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(READBACK_PARAMS)

enum {
  kMubiTrue = 0x6,
  kMubiFalse = 0x9,
  kPolls = 200000,
  kStatusErrs = (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT) |
                (1u << SRAM_CTRL_STATUS_INIT_ERROR_BIT) |
                (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT) |
                (1u << SRAM_CTRL_STATUS_SRAM_ALERT_BIT),
};

typedef struct mem_desc {
  eg2_dev_id_t regs;
  uint32_t base;
  uint32_t size;
} mem_desc_t;

static mem_desc_t mem_of(uint32_t i) {
  mem_desc_t m;
  if (i == 0) {
    m.regs = kEg2DevSramCtrlSecRegs;
    m.base = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR;
    m.size = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES;
  } else {
    m.regs = kEg2DevSramCtrlRetRegs;
    m.base = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR;
    m.size = TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES;
  }
  return m;
}

static const char *mem_name(uint32_t i) { return i == 0 ? "sec" : "ret"; }

static bool init_mem(const mem_desc_t *m) {
  eg2_dev_write32(m->regs, SRAM_CTRL_CTRL_REG_OFFSET, 1u << SRAM_CTRL_CTRL_INIT_BIT);
  for (uint32_t i = 0; i < kPolls; ++i) {
    uint32_t st = eg2_dev_read32(m->regs, SRAM_CTRL_STATUS_REG_OFFSET);
    if (st & (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)) {
      return eg2_chk_eq("status errors after INIT", 0, st & kStatusErrs);
    }
  }
  return eg2_chk_true("INIT_DONE", false);
}

static bool one_mem(uint32_t idx) {
  mem_desc_t m = mem_of(idx);
  eg2_chk_step(mem_name(idx), m.base);
  bool ok = true;
  uint32_t n = EG2_PARAM(n_words);
  uint32_t words = m.size / 4;
  uint32_t first = EG2_PARAM(window) == 0 ? 0
                   : EG2_PARAM(window) == 1 ? words - n
                                            : eg2_rand_range(0, words - n);
  volatile uint32_t *p = eg2_chk_ptr(m.base + 4 * first);
  uint32_t seed = eg2_rand();

  // The testpoint's start: a fresh INIT, so no read meets a word nothing has written.
  ok = init_mem(&m) && ok;
  // Round 0 with readback on, round 1 with it off.
  for (uint32_t round = 0; round < 2; ++round) {
    uint32_t rb = round == 0 ? kMubiTrue : kMubiFalse;
    eg2_dev_write32(m.regs, SRAM_CTRL_READBACK_REG_OFFSET, rb);
    ok = eg2_chk_eq("READBACK as written", rb,
                    eg2_dev_read32(m.regs, SRAM_CTRL_READBACK_REG_OFFSET) & 0xf) &&
         ok;
    for (uint32_t i = 0; i < n; ++i) {
      p[i] = seed ^ (i * 0x9e3779b9u) ^ round;
    }
    for (uint32_t i = 0; i < n; ++i) {
      uint32_t exp = seed ^ (i * 0x9e3779b9u) ^ round;
      uint32_t got = p[i];
      if (got != exp) {
        ok = eg2_chk_eq("word read back", exp, got) && ok;
        break;
      }
    }
    ok = eg2_chk_eq("status errors after the round", 0,
                    eg2_dev_read32(m.regs, SRAM_CTRL_STATUS_REG_OFFSET) & kStatusErrs) &&
         ok;
  }
  // Lock READBACK and try to set it.
  eg2_dev_write32(m.regs, SRAM_CTRL_READBACK_REGWEN_REG_OFFSET, 0);
  eg2_dev_write32(m.regs, SRAM_CTRL_READBACK_REG_OFFSET, kMubiTrue);
  ok = eg2_chk_eq("READBACK unchanged under its lock", kMubiFalse,
                  eg2_dev_read32(m.regs, SRAM_CTRL_READBACK_REG_OFFSET) & 0xf) &&
       ok;
  ok = eg2_chk_eq("READBACK_REGWEN stays cleared after a 1", 0,
                  (eg2_dev_write32(m.regs, SRAM_CTRL_READBACK_REGWEN_REG_OFFSET, 1),
                   eg2_dev_read32(m.regs, SRAM_CTRL_READBACK_REGWEN_REG_OFFSET) & 1)) &&
       ok;
  if (EG2_PARAM(init_check)) {
    // The INIT wipes the data: at least one word differs (pseudo-random fill).
    ok = init_mem(&m) && ok;
    uint32_t same = 0;
    for (uint32_t i = 0; i < n; ++i) {
      if (p[i] == (seed ^ (i * 0x9e3779b9u) ^ 1u)) {
        ++same;
      }
    }
    ok = eg2_chk_true("data wiped by INIT", same < n || n == 0) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  bool ok = true;
  for (uint32_t i = 0; i < 2; ++i) {
    if (EG2_PARAM(mems) & (1u << i)) {
      ok = one_mem(i) && ok;
    }
  }
  return eg2_chk_summary(6) && ok;
}
