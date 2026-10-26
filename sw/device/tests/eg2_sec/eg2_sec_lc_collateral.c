// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_secret_partitions (EG2 verification spec REQ_SEC_07, "Security in RISC-V
 * mode"; Bazel target eg2_sec_lc_collateral): root secrets cannot be provisioned in TEST states,
 * and once provisioned they are unreadable by software through every path to the RRAM's OTP pages
 * and seed pages. Both modes; the life-cycle state is the OTP image the dvsim entry loads.
 *
 * Oracle, per path:
 *  - otp_ctrl's direct access interface (DAI) to the secret partitions. lc_ctrl's "OTP
 *    Accessibility Summary": TEST_UNLOCK_TOKEN and TEST_EXIT_TOKEN (SECRET0) are software
 *    programmable and readable only while SECRET0_DIGEST == 0; RMA_TOKEN and CREATOR_ROOT_KEY
 *    (SECRET2) only while SECRET2_DIGEST == 0 and CREATOR_SEED_SW_RW_EN is on. otp_ctrl's
 *    partition table: SECRET1 is read- and write-locked by its digest, except that its read lock
 *    is ignored in RMA (`ignore_read_lock_in_rma`). A refused access ends with the DAI's ERR_CODE
 *    ACCESS_ERROR and RDATA zero.
 *  - CREATOR_SEED_SW_RW_EN and OWNER_SEED_SW_RW_EN per state (lc_ctrl "Life Cycle Decoded
 *    Outputs" table): creator on in RMA, and in DEV / PROD / PROD_END only before personalization
 *    (SECRET2_DIGEST == 0); owner on in DEV, PROD, PROD_END and RMA; both off otherwise.
 *  - The RRAM controller to the creator and owner seed pages (info pages 5 and 6; rram_ctrl
 *    "Memory Protection for LCMGR Hardware Plug"): the page's INFO_PAGE_CFG AND the life-cycle
 *    signal; a refused operation ends with ERR_CODE.mp_err.
 *  - The RRAM controller and the host window to the OTP pages (data pages 4090-4095): refused in
 *    every state (rram_ctrl "Memory Protection for OTP Hardware Plug", "Memory-Mapped Reads").
 *
 * `paths` (bits: 0 DAI read, 1 DAI write, 2 seed pages, 3 OTP pages through the controller and the
 * host window) and `part` (bits: SECRET0, SECRET1, SECRET2) select the checks. An access the
 * oracle allows is only read (a write would provision the device); an access it refuses is tried
 * as a read and as a write.
 *
 * `model_gaps` (0 on the chip) skips, in the SystemC model only: the SECRET1 read in RMA (the model
 * does not implement `ignore_read_lock_in_rma`) and the host-window reads of the OTP pages (the
 * model's window answers them).
 *
 * For SECRET0 and SECRET2 in RMA, lc_ctrl's table (refused once the digest is set) and otp_ctrl's
 * `ignore_read_lock_in_rma` (readable) disagree; this test follows lc_ctrl's table and REQ_SEC_07
 * (rtl_tickets.md F27).
 *
 * Built with EG2_SEC_LC_COLLATERAL_INJECT_FAIL every DAI read is expected to succeed: the run must
 * fail with `EG2CHK FAIL DAI read SECRET<n> error code` (the default image, RMA, has SECRET2
 * provisioned, so at least that read is refused).
 */

#include "hw/top/otp_ctrl_regs.h"   // Generated.
#include "hw/top/rram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define COLLATERAL_PARAMS(X) \
  X(paths, 15, 1, 15)        \
  X(part, 7, 1, 7)           \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(COLLATERAL_PARAMS)

enum {
  kCreatorPage = 5,
  kOwnerPage = 6,
  kOtpDataPageFirst = 4090,
};

static uint32_t part_offset(uint32_t p) {
  return p == 0   ? OTP_CTRL_PARAM_SECRET0_OFFSET
         : p == 1 ? OTP_CTRL_PARAM_SECRET1_OFFSET
                  : OTP_CTRL_PARAM_SECRET2_OFFSET;
}

static uint32_t part_digest(uint32_t p) {
  return p == 0   ? OTP_CTRL_SECRET0_DIGEST_0_REG_OFFSET
         : p == 1 ? OTP_CTRL_SECRET1_DIGEST_0_REG_OFFSET
                  : OTP_CTRL_SECRET2_DIGEST_0_REG_OFFSET;
}

static bool is_dev_prod(uint32_t s) {
  return s == kEg2LcDev || s == kEg2LcProd || s == kEg2LcProdEnd;
}

static bool dai_checks(uint32_t state, bool creator_en) {
  bool ok = true;
  bool any_error = false;
  for (uint32_t p = 0; p < 3; ++p) {
    if (!(EG2_PARAM(part) & (1u << p))) {
      continue;
    }
    bool provisioned = eg2_otp_digest(part_digest(p)) != 0;
    bool allowed = !provisioned && (p != 2 || creator_en);
    bool read_allowed = allowed || (p == 1 && state == kEg2LcRma);
    eg2_boot_note("partition", p);
    eg2_boot_note(" provisioned", provisioned);
    eg2_boot_note(" read allowed by the documents", read_allowed);
    // model_gaps: the SystemC model refuses SECRET1 in RMA (it does not model otp_ctrl's
    // `ignore_read_lock_in_rma`); stays 0 on the chip.
    bool model_skip = EG2_PARAM(model_gaps) && p == 1 && state == kEg2LcRma;
    if ((EG2_PARAM(paths) & 1) && !model_skip) {
      uint32_t r[2];
      uint32_t err = eg2_otp_dai(kEg2OtpDaiRead, part_offset(p), NULL, r);
      bool expect_ok = read_allowed;
#ifdef EG2_SEC_LC_COLLATERAL_INJECT_FAIL
      expect_ok = true;
#endif
      ok = eg2_boot_check(p == 0   ? "DAI read SECRET0 error code"
                          : p == 1 ? "DAI read SECRET1 error code"
                                   : "DAI read SECRET2 error code",
                          expect_ok ? kEg2OtpErrNone : kEg2OtpErrAccess, err) &&
           ok;
      if (!read_allowed) {
        ok = eg2_boot_check("refused DAI read returns zero", 0, r[0] | r[1]) && ok;
        any_error = true;
      }
    }
    if ((EG2_PARAM(paths) & 2) && !allowed) {
      static const uint32_t kData[2] = {0x5a5a5a5au, 0xa5a5a5a5u};
      uint32_t err = eg2_otp_dai(kEg2OtpDaiWrite, part_offset(p), kData, NULL);
      ok = eg2_boot_check(p == 0   ? "DAI write SECRET0 refused"
                          : p == 1 ? "DAI write SECRET1 refused"
                                   : "DAI write SECRET2 refused",
                          kEg2OtpErrAccess, err) &&
           ok;
      any_error = true;
    }
  }
  if (any_error) {
    eg2_otp_dai_clear_error();
  }
  return ok;
}

static bool seed_page(uint32_t page, bool enabled) {
  bool ok = true;
  // The page's own configuration permits the access; only the life-cycle signal decides.
  uint32_t cfg = eg2_rram_info_cfg(page, true, true, true, true);
  ok = eg2_boot_check("seed page INFO_PAGE_CFG writable (test ROM: nothing locked it)", cfg,
                      eg2_rram_reg(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * page)) &&
       ok;
  eg2_boot_note("seed page", page);
  eg2_boot_note(" life cycle allows software access", enabled);
  uint32_t buf[4] = {0, 0, 0, 0};
  eg2_rram_res_t r = eg2_rram_op(kEg2RramOpRead, kEg2RramPartInfo,
                                 page * RRAM_CTRL_PARAM_BYTES_PER_PAGE, 4, buf);
  ok = eg2_boot_check(page == kCreatorPage ? "creator seed page read: mp_err"
                                           : "owner seed page read: mp_err",
                      enabled ? 0 : kEg2RramErrMp, r.err_code & kEg2RramErrMp) &&
       ok;
  if (!enabled) {
    ok = eg2_boot_check("refused seed read returns no data", 0, buf[0] | buf[1]) && ok;
    uint32_t w[4] = {1, 2, 3, 4};
    r = eg2_rram_op(kEg2RramOpWrite, kEg2RramPartInfo, page * RRAM_CTRL_PARAM_BYTES_PER_PAGE, 4,
                    w);
    ok = eg2_boot_check(page == kCreatorPage ? "creator seed page write: mp_err"
                                             : "owner seed page write: mp_err",
                        kEg2RramErrMp, r.err_code & kEg2RramErrMp) &&
         ok;
  }
  return ok;
}

static bool otp_pages(void) {
  bool ok = true;
  for (uint32_t page = kOtpDataPageFirst; page < RRAM_CTRL_PARAM_NUM_DATA_PAGES; ++page) {
    uint32_t addr = page * RRAM_CTRL_PARAM_BYTES_PER_PAGE + 16 * eg2_rand_range(0, 31);
    uint32_t buf[4] = {0, 0, 0, 0};
    eg2_rram_res_t r = eg2_rram_op(kEg2RramOpRead, kEg2RramPartData, addr, 4, buf);
    ok = eg2_boot_check("OTP page controller read: mp_err", kEg2RramErrMp,
                        r.err_code & kEg2RramErrMp) &&
         ok;
    r = eg2_rram_op(kEg2RramOpWrite, kEg2RramPartData, addr, 4, buf);
    ok = eg2_boot_check("OTP page controller write: mp_err", kEg2RramErrMp,
                        r.err_code & kEg2RramErrMp) &&
         ok;
    if (!EG2_PARAM(model_gaps)) {
      // model_gaps: the SystemC model's host window reads the OTP pages; 0 on the chip.
      eg2_boot_out_t out;
      uint32_t cause =
          eg2_boot_probe(TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + addr, kEg2BootLoad, 0, &out);
      ok = eg2_boot_check("OTP page host read: load access fault", kEg2CauseLoadFault, cause) &&
           ok;
    }
  }
  return ok;
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_boot_irq_off();
  uint32_t state = eg2_lc_state_decoded();
  eg2_boot_note("life-cycle state", state);
  bool secret2_locked = eg2_otp_digest(OTP_CTRL_SECRET2_DIGEST_0_REG_OFFSET) != 0;
  bool creator_en = state == kEg2LcRma || (is_dev_prod(state) && !secret2_locked);
  bool owner_en = state == kEg2LcRma || is_dev_prod(state);
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  if (recov >= 0) {
    eg2_status_expect_alert((size_t)recov);
  }

  ok = dai_checks(state, creator_en) && ok;
  if (EG2_PARAM(paths) & 4) {
    ok = seed_page(kCreatorPage, creator_en) && ok;
    ok = seed_page(kOwnerPage, owner_en) && ok;
  }
  if (EG2_PARAM(paths) & 8) {
    ok = otp_pages() && ok;
  }
  return eg2_boot_summary() && ok;
}
