// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_lc_rram_rollback (EG2 verification spec REQ_SEC_05, "Security in RISC-V mode"): no
 * path rewrites or erases the life-cycle partition in RRAM. Both modes.
 *
 * Oracle:
 *  - lc_ctrl "OTP Accessibility Summary": LC_STATE and LC_TRANSITION_CNT are neither programmable
 *    nor readable by software ("Advanced only by HW"); otp_ctrl's DAI refuses them with
 *    ACCESS_ERROR;
 *  - rram_ctrl "Memory Protection for OTP Hardware Plug": the OTP pages (data pages 4090-4095,
 *    which hold the LIFE_CYCLE partition) are refused to the software controller path (mp_err)
 *    and to the host window ("Memory-Mapped Reads via host_tl": the OTP region is not reachable;
 *    the host window takes no writes at all);
 *  - device life cycle, "Architecture": OTP state transitions cannot be reverted, so LC_STATE and
 *    LC_TRANSITION_CNT read the same after every attempt.
 *
 * `paths` bits: 0 DAI write of LC_STATE and LC_TRANSITION_CNT, 1 DAI read of them, 2 RRAM
 * controller write and read of the OTP pages that hold them, 3 a host store into them.
 * `model_gaps` (0 on the chip) skips path 3 in the SystemC model, whose RRAM host window takes
 * stores.
 * `value`: what is written (0 the erased value, an earlier state's encoding; 1 all ones; 2
 * random).
 * The bench (chip_sw_eg2_lc_rram_rollback_vseq) compares the partition's RRAM contents before and
 * after by backdoor, and adds the paths software cannot take: the rram_macro's JTAG (REQ_RRAM_20)
 * where the life-cycle state enables it.
 *
 * Built with EG2_LC_RRAM_ROLLBACK_INJECT_FAIL the DAI write is expected to be accepted: the run
 * must fail with `EG2CHK FAIL DAI write LC_STATE refused`.
 */

#include "hw/top/otp_ctrl_regs.h"   // Generated.
#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define ROLLBACK_PARAMS(X) \
  X(paths, 15, 1, 15)      \
  X(value, 0, 0, 2)        \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(ROLLBACK_PARAMS)

enum {
  kOtpDataPageFirst = 4090,
  kOtpPages = 6,
};

static uint32_t new_value(void) {
  switch (EG2_PARAM(value)) {
    case 1:
      return 0xffffffffu;
    case 2:
      return eg2_rand();
    default:
      return 0;
  }
}

static bool unchanged(const char *what, uint32_t state, uint32_t count) {
  bool ok = eg2_boot_check(what, state, eg2_lc_state());
  return eg2_boot_check("LC_TRANSITION_CNT unchanged", count, eg2_lc_count()) && ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_boot_irq_off();
  const uint32_t state = eg2_lc_state();
  const uint32_t count = eg2_lc_count();
  eg2_boot_note("LC_STATE", state);
  eg2_boot_note("LC_TRANSITION_CNT", count);
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  if (recov >= 0) {
    eg2_status_expect_alert((size_t)recov);
  }

  if (EG2_PARAM(paths) & 1) {
    uint32_t w[2] = {new_value(), new_value()};
    uint32_t err = eg2_otp_dai(kEg2OtpDaiWrite, OTP_CTRL_PARAM_LC_STATE_OFFSET, w, NULL);
#ifdef EG2_LC_RRAM_ROLLBACK_INJECT_FAIL
    ok = eg2_boot_check("DAI write LC_STATE refused", kEg2OtpErrNone, err) && ok;
#else
    ok = eg2_boot_check("DAI write LC_STATE refused", kEg2OtpErrAccess, err) && ok;
#endif
    err = eg2_otp_dai(kEg2OtpDaiWrite, OTP_CTRL_PARAM_LC_TRANSITION_CNT_OFFSET, w, NULL);
    ok = eg2_boot_check("DAI write LC_TRANSITION_CNT refused", kEg2OtpErrAccess, err) && ok;
    ok = unchanged("LC_STATE unchanged after DAI writes", state, count) && ok;
  }
  if (EG2_PARAM(paths) & 2) {
    uint32_t r[2];
    uint32_t err = eg2_otp_dai(kEg2OtpDaiRead, OTP_CTRL_PARAM_LC_STATE_OFFSET, NULL, r);
    ok = eg2_boot_check("DAI read LC_STATE refused", kEg2OtpErrAccess, err) && ok;
    ok = eg2_boot_check("refused DAI read returns zero", 0, r[0] | r[1]) && ok;
  }
  if (EG2_PARAM(paths) & 3) {
    eg2_otp_dai_clear_error();
  }
  if (EG2_PARAM(paths) & 4) {
    for (uint32_t page = kOtpDataPageFirst; page < kOtpDataPageFirst + kOtpPages; ++page) {
      uint32_t addr = page * RRAM_CTRL_PARAM_BYTES_PER_PAGE + 16 * eg2_rand_range(0, 31);
      uint32_t w[4] = {new_value(), new_value(), new_value(), new_value()};
      eg2_rram_res_t res = eg2_rram_op(kEg2RramOpWrite, kEg2RramPartData, addr, 4, w);
      ok = eg2_boot_check("controller write to an OTP page: mp_err", kEg2RramErrMp,
                          res.err_code & kEg2RramErrMp) &&
           ok;
    }
    ok = unchanged("LC_STATE unchanged after controller writes", state, count) && ok;
  }
  // model_gaps: the SystemC model's RRAM host window takes stores (dv_todo G2); stays 0 on the
  // chip.
  if ((EG2_PARAM(paths) & 8) && !EG2_PARAM(model_gaps)) {
    for (uint32_t page = kOtpDataPageFirst; page < kOtpDataPageFirst + kOtpPages; ++page) {
      eg2_boot_out_t out;
      uint32_t addr = TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + page * RRAM_CTRL_PARAM_BYTES_PER_PAGE;
      uint32_t cause = eg2_boot_probe(addr, kEg2BootStore, new_value(), &out);
      ok = eg2_boot_check("host store into an OTP page: store access fault", kEg2CauseStoreFault,
                          cause) &&
           ok;
    }
    ok = unchanged("LC_STATE unchanged after host stores", state, count) && ok;
  }
  return eg2_boot_summary() && ok;
}
