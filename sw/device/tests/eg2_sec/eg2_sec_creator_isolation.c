// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_creator_isolation and _cheriot (EG2 verification spec REQ_SEC_03, "Security in
 * RISC-V mode"): Silicon Owner software, in either mode, cannot read Silicon Creator secrets or
 * change regions the Silicon Creator controls. Behind the full secure-boot chain (sim_dv_rom_ext).
 *
 * Oracle: logical security model "Silicon Creator", "Silicon Owner"; ROM_EXT's documented
 * handover (rom_ext.c: rom_ext_nvm_protect_self, nvm_ctrl_creator_info_pages_lockdown,
 * otp_creator_sw_cfg_lockdown "Disable access to silicon creator info pages, the OTP creator
 * partition and the OTP direct access interface until the next reset"). Every Creator region,
 * every path (`paths` bits):
 *   0 host       ROM_EXT's slot: a store faults (7), the word unchanged;
 *   1 rram_ctrl  ROM_EXT's slot: write refused (mp_err); the Creator info pages (real: FactoryId,
 *                CreatorSecret, WaferAuthSecret; emulated: BootData0, BootData1, CreatorReserved0):
 *                read and write refused (mp_err), no data returned;
 *   2 otp_ctrl   CREATOR_SW_CFG through the software window: read refused (load fault); the
 *                direct access interface: locked (DIRECT_ACCESS_REGWEN 0), or, if it is not,
 *                reads of SECRET0/1/2 refused (ACCESS_ERROR);
 *   3 SBA        the bench (chip_sw_eg2_sec_creator_isolation_vseq, the same SBA path as
 *                chip_sw_eg2_boot_handover_protection_vseq): system bus reads of the Creator info
 *                pages are not possible (no host window), so it writes ROM_EXT's slot and the
 *                RRAM controller's region configuration and reads them back, and compares
 *                ROM_EXT's slot by backdoor; its verdict comes back in `kEg2BenchVerdict`.
 * A write that would be accepted writes the word's own value back.
 *
 * Built with EG2_SEC_CREATOR_ISOLATION_INJECT_FAIL the CreatorSecret read is expected to be
 * allowed: the run must fail with `EG2CHK FAIL read CreatorSecret (info 5)`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/otp_ctrl_regs.h"   // Generated.
#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define ISOLATION_PARAMS(X) X(paths, 15, 1, 15)
EG2_RM_PARAMS(ISOLATION_PARAMS)

enum {
  kRram = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR,
  kPage = RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kEmulBaseB = RRAM_CTRL_PARAM_NUM_DATA_PAGES - 0x8000 / RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kEmulBaseA = RRAM_CTRL_PARAM_NUM_DATA_PAGES / 2 - 0x8000 / RRAM_CTRL_PARAM_BYTES_PER_PAGE,
  kSbaReady = 0x41425353,
  kBenchDone = 0x454e4f44,
  kRegions = 6,
  kLabel = 40,
};

volatile uint32_t kEg2SbaReady;
volatile uint32_t kEg2BenchDone;
volatile uint32_t kEg2BenchVerdict;

typedef struct region {
  uint32_t partition;
  uint32_t addr;
} region_t;

static const region_t kRegionsTable[kRegions] = {
    {kEg2RramPartInfo, 0 * kPage},
    {kEg2RramPartInfo, 5 * kPage},
    {kEg2RramPartInfo, 7 * kPage},
    {kEg2RramPartData, (kEmulBaseB + 32) * kPage},
    {kEg2RramPartData, (kEmulBaseB + 36) * kPage},
    {kEg2RramPartData, kEmulBaseA * kPage},
};

static const char kReadLabels[kRegions][kLabel] = {
    "read FactoryId (info 0)",  "read CreatorSecret (info 5)", "read WaferAuthSecret (info 7)",
    "read BootData0 (emulated)", "read BootData1 (emulated)",  "read CreatorReserved0 (emulated)",
};
static const char kWriteLabels[kRegions][kLabel] = {
    "write FactoryId (info 0)",  "write CreatorSecret (info 5)", "write WaferAuthSecret (info 7)",
    "write BootData0 (emulated)", "write BootData1 (emulated)",  "write CreatorReserved0 (emulated)",
};

static bool controller_paths(void) {
  bool ok = true;
  uint32_t words[4] = {0, 0, 0, 0};
  eg2_rram_res_t r = eg2_rram_op(kEg2RramOpWrite, kEg2RramPartData, 0x400, 4, words);
  ok = eg2_boot_check("write ROM_EXT's slot: mp_err", kEg2RramErrMp, r.err_code & kEg2RramErrMp) &&
       ok;
  for (uint32_t i = 0; i < kRegions; ++i) {
    for (uint32_t w = 0; w < 4; ++w) {
      words[w] = 0;
    }
    r = eg2_rram_op(kEg2RramOpRead, kRegionsTable[i].partition, kRegionsTable[i].addr, 4, words);
    uint32_t exp = kEg2RramErrMp;
#ifdef EG2_SEC_CREATOR_ISOLATION_INJECT_FAIL
    if (i == 1) {
      exp = 0;
    }
#endif
    ok = eg2_boot_check(kReadLabels[i], exp, r.err_code & kEg2RramErrMp) && ok;
    ok = eg2_boot_check("  refused read returns no data", 0, words[0] | words[1]) && ok;
    // Write back what a permitted read returned (nothing changes if the write is accepted).
    r = eg2_rram_op(kEg2RramOpWrite, kRegionsTable[i].partition, kRegionsTable[i].addr, 4, words);
    ok = eg2_boot_check(kWriteLabels[i], kEg2RramErrMp, r.err_code & kEg2RramErrMp) && ok;
  }
  return ok;
}

static bool otp_paths(void) {
  bool ok = true;
  eg2_boot_out_t out;
  uint32_t cause = eg2_boot_probe(TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR +
                                      OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                                      OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
                                  kEg2BootLoad, 0, &out);
  ok = eg2_boot_check("CREATOR_SW_CFG read through the window refused", kEg2CauseLoadFault,
                      cause) &&
       ok;
  uint32_t regwen = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_REGWEN_REG_OFFSET);
  ok = eg2_boot_check("direct access interface locked", 0, regwen) && ok;
  if (regwen != 0) {
    static const uint32_t kOffsets[3] = {OTP_CTRL_PARAM_SECRET0_OFFSET,
                                         OTP_CTRL_PARAM_SECRET1_OFFSET,
                                         OTP_CTRL_PARAM_SECRET2_OFFSET};
    for (uint32_t p = 0; p < 3; ++p) {
      uint32_t r[2];
      ok = eg2_boot_check("DAI read of a secret partition refused", kEg2OtpErrAccess,
                          eg2_otp_dai(kEg2OtpDaiRead, kOffsets[p], NULL, r)) &&
           ok;
    }
    eg2_otp_dai_clear_error();
  }
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA) && ok;
  eg2_boot_irq_off();
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  if (recov >= 0) {
    eg2_status_expect_alert((size_t)recov);
  }
  uint32_t paths = EG2_PARAM(paths);
  if (paths & 1) {
    eg2_boot_out_t out;
    uint32_t before = eg2_boot_rd32(kRram + 0x400);
    ok = eg2_boot_check("host store into ROM_EXT's slot", kEg2CauseStoreFault,
                        eg2_boot_probe(kRram + 0x400, kEg2BootStore, ~before, &out)) &&
         ok;
    ok = eg2_boot_check("ROM_EXT's word unchanged", before, eg2_boot_rd32(kRram + 0x400)) && ok;
  }
  if (paths & 2) {
    ok = controller_paths() && ok;
  }
  if (paths & 4) {
    ok = otp_paths() && ok;
  }
  if (paths & 8) {
    kEg2BenchDone = 0;
    kEg2SbaReady = kSbaReady;
    uint32_t polls = 0;
    while (kEg2BenchDone != kBenchDone && polls < 2000000) {
      ++polls;
    }
    ok = eg2_boot_check("bench's SBA path done", kBenchDone, kEg2BenchDone) && ok;
    ok = eg2_boot_check("bench's SBA checks", 0, kEg2BenchVerdict) && ok;
  }
  return eg2_boot_summary() && ok;
}
