// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_sram_rekey_tag: a locked SRAM controller preserves capabilities.
 *
 * The CHERIoT boot stage clears CTRL_REGWEN (REQ_SOC_08, REQ_CMS_16). SRAM's register description says
 * writes to CTRL then have no effect until reset. Store capabilities in sec and main SRAM,
 * attempt INIT with key renewal or renewal alone, and check that sec SRAM's words and tags
 * stay equal to the main-SRAM copies. The controller's status must stay unchanged too.
 *
 * The locked-write check covers REQ_CMS_16's "unreachable in CHERIoT mode" branch.
 * INIT/rekey functionality is tested before the lock in the RISC-V SRAM tests. This test does
 * not decide the open REQ_CMS_16 question about tags after an actually executed INIT/rekey.
 * `model_gaps` is retained for existing runners; these lock checks run in every model mode.
 * The _inject_fail build still corrupts the expected first main tag and must fail there.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define SRAM_REKEY_TAG_PARAMS(X) \
  X(writer, 0, 0, 2)             \
  X(n_caps, 0, 0, 64)            \
  X(range_class, 0, 0, 4)        \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(SRAM_REKEY_TAG_PARAMS);

enum {
  kMaxCaps = 64,
  // The sec SRAM slots' addresses are kept in the main SRAM arena's second half (sec SRAM is wiped).
  kMainSlots = 0,
  kAddrTable = kEg2MainArenaSize / 2,
};

static eg2_tally_t tally;

static uint32_t sec_slot(uint32_t cls, uint32_t i, uint32_t n) {
  switch (cls) {
    case 1:  // one granule
      return kEg2SecBase + 8 * 100;
    case 2:  // a granule and its neighbours
      return kEg2SecBase + 8 * (200 + i);
    case 3:  // many granules, a run of them
      return kEg2SecBase + 8 * (1000 + 3 * i);
    default:  // spread over the whole of sec SRAM
      return kEg2SecBase + 8 * ((i * (kEg2SecSize / 8 - 1)) / (n > 1 ? n - 1 : 1));
  }
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  uint32_t writer = EG2_PARAM(writer) != 0 ? EG2_PARAM(writer) : eg2_rand_range(1, 2);
  uint32_t cls = EG2_PARAM(range_class);
  if (cls == 0) {
    uint32_t d = eg2_rand_range(0, 99);
    cls = d < 30 ? 1 : d < 50 ? 2 : d < 80 ? 3 : 4;
  }
  uint32_t n = EG2_PARAM(n_caps) != 0 ? EG2_PARAM(n_caps) : eg2_rand_range(1, 64);
  if (cls == 1) {
    n = 1;
  } else if (cls == 2 && n > 3) {
    n = 3;
  }
  eg2_puts(writer == 1 ? "EG2 sram_rekey_tag: INIT" : "EG2 sram_rekey_tag: key renewal");
  eg2_put_kv(" range_class", cls);
  eg2_put_kv(" n_caps", n);
  eg2_puts("\r\n");
  uint32_t arena = eg2_main_arena();
  for (uint32_t i = 0; i < n; ++i) {
    void *c = eg2_ms_object_cap(kEg2MsHeapBase + 8 * eg2_rand_range(0, 1000), 16, false);
    uint32_t s = sec_slot(cls, i, n);
    eg2_ms_csc(s, c);
    eg2_ms_write32(arena + kAddrTable + 4 * i, s);
    eg2_ms_csc(arena + kMainSlots + 8 * i, c);
  }

  eg2_check(&tally, "CTRL_REGWEN locked at boot", 0,
            eg2_dev_read32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_CTRL_REGWEN_REG_OFFSET), 0);
  uint32_t status = eg2_dev_read32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_STATUS_REG_OFFSET);
  uint32_t command = 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT;
  if (writer == 1) command |= 1u << SRAM_CTRL_CTRL_INIT_BIT;
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_CTRL_REG_OFFSET, command);
  eg2_check(&tally, "locked command preserves status", 0,
            eg2_dev_read32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_STATUS_REG_OFFSET), status);
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t sec = eg2_ms_read32(arena + kAddrTable + 4 * i);
    uint32_t main = arena + kMainSlots + 8 * i;
    eg2_check(&tally, "locked command preserves sec tag", i, eg2_ms_tag(sec), 1);
    for (uint32_t word = 0; word < 2; ++word) {
      eg2_check(&tally, "locked command preserves sec word", 2 * i + word,
                eg2_ms_read32(sec + 4 * word), eg2_ms_read32(main + 4 * word));
    }
    ++tally.done;
  }
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t exp = 1;
#ifdef EG2_SRAM_REKEY_TAG_INJECT_FAIL
    if (i == 0) {
      eg2_inject_armed("main tag");
      exp = 0;
    }
#endif
    eg2_check(&tally, "main tag", i, eg2_ms_tag(arena + kMainSlots + 8 * i), exp);
    ++tally.done;
  }
  eg2_tally_print("sram_rekey_tag", &tally);
  return eg2_tally_ok(&tally);
}
