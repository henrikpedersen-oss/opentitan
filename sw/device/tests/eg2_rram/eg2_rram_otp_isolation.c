// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_otp_isolation (EG2 verification spec REQ_RRAM_16): the OTP pages
 * (the integrity page 4090 and the OTP pages 4091-4095) are unreachable from
 * software and from the host under every software configuration, by a hardwired
 * top-priority rule.
 *
 * Documents: RFC RRAM §4.3 p8; rram_ctrl programmers_guide.md "Memory-Mapped
 * Reads via host_tl"
 * ("The OTP region ... is not reachable"), theory_of_operation.md "Memory
 * Protection" (the fixed OTP exclusion first in the priority scan: no region,
 * emulated subregion or default can grant it), "Error Encountered by Software
 * Initiated Controller Operations" (mp_err, ERR_ADDR the first error, a read
 * returns all ones from that word on); README "Address Map".
 *
 * The region configuration is drawn to be as permissive as possible over the
 * OTP pages: cfg_class 0 an MP region with every attribute over the OTP pages,
 * 1 a region with every attribute from the scratch pages over the OTP pages, 2
 * the default region with read and write allowed (its scramble and ECC kept:
 * the test image reads through it), 3 all three. Then n_accesses accesses, each
 * to a drawn OTP-page word (the integrity page, the first and last OTP page,
 * random) by a drawn path: path 0 host load: a load access fault (mcause 5,
 * mtval the address), never a value; path 1 controller read of 1-16 words:
 * OP_STATUS.err, ERR_CODE.mp_err, ERR_ADDR the first word, every word read as
 * 0xFFFFFFFF; path 2 controller write of one RRAM word: OP_STATUS.err,
 * ERR_CODE.mp_err; the OTP contents unchanged: the OTP word at that place, read
 * through otp_ctrl's DAI before and after, is the same (a non-secret
 * partition's word; for the integrity page the DAI read of the word it protects
 * still has no error). path 3 (with `sba`, chip only) `EG2 RRAM otp_isolation
 * sba off=0x<o>`: the vseq reads the host address over SBA, expects sberror,
 * and checks the RRAM row unchanged. Every access ends (no hang); the recov_err
 * alert each controller error raises is expected.
 *
 * Knobs:
 *   n_accesses  1-32
 *   cfg_class   0-3 as above, 4 drawn
 *   w_path_host, w_path_read, w_path_write, w_path_sba   path weights
 *   sba         1: the SBA path runs (chip only: eg2sim has no debug module)
 *   model_gaps  skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1   | Why |
 * |-------------------------------|--------------------------------------------------------------|
 * | the host-load fault (path 0)  | eg2sim's rram_ctrl.host is a plain 2 MiB
 * memory: the OTP     | |                               |   pages read as
 * memory (model finding: the documents refuse  | | |   them); the access is
 * still made and must not hang          |
 *
 * Built with EG2_RRAM_OTP_ISOLATION_INJECT_FAIL the first value check expects
 * bit 0 flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/otp_ctrl_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define OTP_ISOLATION_PARAMS(X) \
  X(n_accesses, 12, 1, 32)      \
  X(cfg_class, 4, 0, 4)         \
  X(w_path_host, 30, 0, 100)    \
  X(w_path_read, 30, 0, 100)    \
  X(w_path_write, 30, 0, 100)   \
  X(w_path_sba, 10, 0, 100)     \
  X(sba, 0, 0, 1)               \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(OTP_ISOLATION_PARAMS)

enum {
  kPolls = 100000,
  kOtpIntgPageOff = kEg2RramOtpIntgPage * kEg2RramPageBytes,
  kOtpLastByte = kEg2RramDataPages * kEg2RramPageBytes - 1,
  kDaiErr = OTP_CTRL_ERR_CODE_0_REG_OFFSET + 4 * OTP_CTRL_PARAM_NUM_PART,
  kMpErr = 1u << RRAM_CTRL_ERR_CODE_MP_ERR_BIT,
  kOpErrBit = 1u << RRAM_CTRL_OP_STATUS_ERR_BIT,
  // Non-secret OTP data (VENDOR_TEST up to HW_CFG1): readable by the DAI in
  // this state.
  kOtpReadableEnd = OTP_CTRL_PARAM_SECRET0_OFFSET,
};

static volatile uint32_t sba_done;

static uint32_t otp_rd(uint32_t off) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, off);
}
static void otp_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevOtpCtrlCore, off, v);
}

static bool dai_wait_idle(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (otp_rd(OTP_CTRL_STATUS_REG_OFFSET) &
        (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      return true;
    }
  }
  return false;
}

static uint32_t dai_read(uint32_t addr, uint32_t *v) {
  if (!dai_wait_idle()) {
    return 0xff;
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET,
         1u << OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT);
  if (!dai_wait_idle()) {
    return 0xff;
  }
  *v = otp_rd(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
  return otp_rd(kDaiErr) & OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
}

/** A word of the OTP pages: the integrity page, the first OTP page, the last,
 * or random. */
static uint32_t draw_otp_word(void) {
  uint32_t words = (kOtpLastByte + 1 - kOtpIntgPageOff) / 4;
  switch (eg2_rand_range(0, 3)) {
    case 0:
      return kOtpIntgPageOff + 4 * eg2_rand_range(0, kEg2RramPageBytes / 4 - 1);
    case 1:
      return kEg2RramOtpStartAddr +
             4 * eg2_rand_range(0, kEg2RramPageBytes / 4 - 1);
    case 2:
      return kOtpLastByte + 1 - 4 * eg2_rand_range(1, kEg2RramPageBytes / 4);
    default:
      return kOtpIntgPageOff + 4 * eg2_rand_range(0, words - 1);
  }
}

/** The OTP byte offset whose data or integrity this RRAM word holds (a word to
 * read back). */
static uint32_t otp_word_of(uint32_t rram_off) {
  uint32_t o = rram_off >= kEg2RramOtpStartAddr
                   ? rram_off - kEg2RramOtpStartAddr
                   : (rram_off - kOtpIntgPageOff) * 8;
  return o & ~3u;
}

static void configure(uint32_t cls, uint32_t *regions, uint32_t *nreg) {
  *nreg = 0;
  uint32_t all =
      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrScr | kEg2RramAttrEcc;
  if (cls == 0 || cls == 3) {
    uint32_t r = eg2_rram_free_region();
    if (r < kEg2RramRegions) {
      eg2_rram_region_set(r, kEg2RramOtpIntgPage,
                          kEg2RramDataPages - kEg2RramOtpIntgPage, all);
      regions[(*nreg)++] = r;
    }
  }
  if (cls == 1 || cls == 3) {
    uint32_t r = eg2_rram_free_region();
    if (r < kEg2RramRegions) {
      // From the scratch pages over the OTP pages: data and OTP pages in one
      // region.
      eg2_rram_region_set(r, kEg2RramScratchFirstPage,
                          kEg2RramDataPages - kEg2RramScratchFirstPage, all);
      regions[(*nreg)++] = r;
    }
  }
  if (cls == 2 || cls == 3) {
    // Read and write allowed; scramble and ECC as they were (the test image
    // reads through it).
    uint32_t d = eg2_rram_cfg_attrs(
        eg2_rram_reg(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET), true);
    eg2_rram_reg_wr(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET,
                    eg2_rram_default_cfg(d | kEg2RramAttrRd | kEg2RramAttrWr));
  }
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls));
  int recov = eg2_status_alert_find("rram_ctrl_recov_err");
  if (!eg2_chk_true("rram_ctrl_recov_err known", recov >= 0)) {
    return false;
  }
  uint32_t cls =
      EG2_PARAM(cfg_class) > 3 ? eg2_rand_range(0, 3) : EG2_PARAM(cfg_class);
  uint32_t saved_default = eg2_rram_reg(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET);
  uint32_t regions[2], nreg;
  configure(cls, regions, &nreg);
  eg2_chk_step("configuration class", cls);

  const uint32_t wp[4] = {EG2_PARAM(w_path_host), EG2_PARAM(w_path_read),
                          EG2_PARAM(w_path_write),
                          EG2_PARAM(sba) ? EG2_PARAM(w_path_sba) : 0};
  bool ctrl_error = false;
  for (uint32_t i = 0; i < EG2_PARAM(n_accesses); ++i) {
    uint32_t off = draw_otp_word();
    uint32_t path = eg2_chk_pick(wp, 4);
    eg2_chk_step("otp page word", off);
    eg2_chk_step("path", path);
    eg2_rram_result_t r;
    switch (path) {
      case 0: {
        eg2_chk_trap_t t;
        uint32_t cause = eg2_chk_access(eg2_rram_host(off), kEg2ChkLw, 0, &t);
        if (gaps) {
          eg2_chk_skip(
              "the host-load fault (the model's host window is plain memory)");
        } else {
          ok = eg2_chk_eq("host load: load access fault", kEg2ChkCauseLoadFault,
                          cause) &&
               ok;
          ok =
              eg2_chk_eq("host load: mtval", kEg2RramHostBase + off, t.mtval) &&
              ok;
        }
        break;
      }
      case 1: {
        uint32_t n = eg2_rand_range(1, 16);
        uint32_t buf[16];
        if (off + 4 * n > kOtpLastByte + 1) {
          n = (kOtpLastByte + 1 - off) / 4;
        }
        ok = eg2_chk_true("controller read ended",
                          eg2_rram_read(kEg2RramPartData, off, buf, n, &r)) &&
             ok;
        ok = eg2_chk_eq("controller read: OP_STATUS.err", kOpErrBit,
                        r.op_status & kOpErrBit) &&
             ok;
        ok = eg2_chk_eq("controller read: mp_err", kMpErr,
                        r.err_code & kMpErr) &&
             ok;
        ok = eg2_chk_eq("controller read: ERR_ADDR", off, r.err_addr) && ok;
        for (uint32_t j = 0; j < r.words; ++j) {
          ok =
              eg2_chk_eq("controller read: all ones", 0xffffffff, buf[j]) && ok;
        }
        ctrl_error = true;
        break;
      }
      case 2: {
        uint32_t rw = off & ~(kEg2RramWordBytes - 1);
        uint32_t otp_off = otp_word_of(off);
        uint32_t before = 0, after = 0, e0 = 0, e1 = 0;
        bool readable = otp_off < kOtpReadableEnd;
        if (readable) {
          e0 = dai_read(otp_off, &before);
        }
        uint32_t w[4] = {eg2_rand(), eg2_rand(), eg2_rand(), eg2_rand()};
        ok = eg2_chk_true("controller write ended",
                          eg2_rram_write(kEg2RramPartData, rw, w, 4, &r)) &&
             ok;
        ok = eg2_chk_eq("controller write: OP_STATUS.err", kOpErrBit,
                        r.op_status & kOpErrBit) &&
             ok;
        ok = eg2_chk_eq("controller write: mp_err", kMpErr,
                        r.err_code & kMpErr) &&
             ok;
        if (readable) {
          e1 = dai_read(otp_off, &after);
          ok =
              eg2_chk_eq("OTP word readable after the refused write", e0, e1) &&
              ok;
          ok = eg2_chk_eq("OTP word unchanged", before, after) && ok;
        }
        ctrl_error = true;
        break;
      }
      default: {
        sba_done = 0;
        LOG_INFO("EG2 RRAM otp_isolation sba off=0x%x flag=0x%x",
                 kEg2RramHostBase + off, eg2_chk_addr_of(&sba_done));
        uint32_t polls = 0;
        while (sba_done == 0 && polls < 2000000) {
          ++polls;
        }
        ok = eg2_chk_eq("the bench's SBA access done", 1, sba_done) && ok;
        break;
      }
    }
  }
  if (ctrl_error) {
    // Each refused controller operation raised recov_err
    // (theory_of_operation.md "RRAM Errors and Faults").
    ok = eg2_chk_true("rram_ctrl_recov_err at the alert handler",
                      eg2_status_wait_alert((size_t)recov, 10000)) &&
         ok;
    eg2_status_expect_alert((size_t)recov);
    eg2_status_clear_expected();
  }
  for (uint32_t i = 0; i < nreg; ++i) {
    eg2_rram_region_off(regions[i]);
  }
  eg2_rram_reg_wr(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET, saved_default);
  eg2_rram_reg_wr(RRAM_CTRL_INTR_STATE_REG_OFFSET,
                  1u << RRAM_CTRL_INTR_STATE_OP_DONE_BIT);
  return eg2_chk_summary(EG2_PARAM(n_accesses)) && ok;
}
