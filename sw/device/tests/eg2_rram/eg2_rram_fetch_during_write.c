// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_fetch_during_write (EG2 verification spec REQ_RRAM_04, "RRAM controller in RISC-V
 * mode"): host reads make progress while a controller write and an OTP write use the macro.
 *
 * RFC RRAM §4.1 p7: "the phy-arbiter can interleave latency critical host requests with
 * controller requests. This is required to avoid deadlock situations such as the processor
 * waiting for instructions from the host interface while the controller is locked to the software
 * interface." The test itself runs from RRAM and feeds the write FIFO from there, so a write of up
 * to 1024 words (the most one operation takes, programmer's guide) only completes if fetches go on
 * between the macro writes. Between pushes it also calls a routine and loads data from a probe
 * block in RRAM (at the drawn distance from the written pages) and checks both. An OTP
 * direct-access write (otp_ctrl DAI, to the VENDOR_TEST partition, unprogrammed in the simulation
 * images) runs at the same time when `otp_write` is set; it must complete without DAI_ERROR and
 * read back.
 *
 * Progress is the watchdog: every push must find FIFO room within a bounded number of polls, and
 * the operation must finish. Afterwards every written word is read back through the controller
 * and compared with the seed-determined pattern written.
 *
 * Knobs:
 *   n_words       the write's length in bus words (a multiple of 4; 1024 the spec's case)
 *   distance      the probe block from the written pages: 0 the same page (before the written
 *                 range), 1 the adjacent page, 2 a far page, 3 drawn
 *   probe_every   pushes between two probes (fetch + load)
 *   otp_write     1: an OTP DAI write during the RRAM write
 *   icache        0 as booted, 1 off, 2 on
 *   model_gaps    skips what the SystemC model does not model: nothing (its writes take no time,
 *                 so progress holds trivially there)
 *
 * Built with EG2_RRAM_FETCH_DURING_WRITE_INJECT_FAIL the first value check expects bit 0 flipped:
 * the run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

#include "hw/top/otp_ctrl_regs.h"  // Generated.

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define FETCH_DURING_WRITE_PARAMS(X) \
  X(n_words, 1024, 4, 1024)          \
  X(distance, 3, 0, 3)               \
  X(probe_every, 4, 1, 64)           \
  X(otp_write, 1, 0, 1)              \
  X(icache, 1, 0, 2)                 \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(FETCH_DURING_WRITE_PARAMS)

enum {
  kPolls = 200000,
  kPages = 32,
  kSalt = 0x46445752,  // "FDWR"
  kOtpVendorTestWords = (OTP_CTRL_PARAM_VENDOR_TEST_SIZE -
                         OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_SIZE) /
                        4,
};

static uint32_t otp(uint32_t off) {
  return eg2_dev_read32(kEg2DevOtpCtrlCore, off);
}
static void otp_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevOtpCtrlCore, off, v);
}

static bool otp_dai_idle(void) {
  return (otp(OTP_CTRL_STATUS_REG_OFFSET) >> OTP_CTRL_STATUS_DAI_IDLE_BIT) & 1;
}

static bool otp_wait_idle(void) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (otp_dai_idle()) {
      return true;
    }
  }
  return false;
}

/** Starts a DAI command (otp_ctrl programmer's guide "Direct Access
 * Interface"). */
static void otp_dai_start(uint32_t cmd_bit, uint32_t addr, uint32_t wdata) {
  otp_wr(OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  if (cmd_bit == OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT) {
    otp_wr(OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET, wdata);
  }
  otp_wr(OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET, 1u << cmd_bit);
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  uint32_t n = EG2_PARAM(n_words) & ~3u;
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t first = eg2_rand_range(kEg2RramScratchFirstPage,
                                  kEg2RramScratchLastPage - kPages + 1);
  eg2_rram_region_set(region, first, kPages,
                      kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc);
  bool icache_boot = eg2_rram_icache_set(true);
  if (EG2_PARAM(icache) != 0) {
    eg2_rram_icache_set(EG2_PARAM(icache) == 2);
  } else {
    eg2_rram_icache_set(icache_boot);
  }

  // The written range starts half way into page first + 1 (so the first page
  // has an unwritten half), and spans up to 1024 words = 8 pages. The probe
  // block (code, then data words) is in the first page's unwritten half, the
  // page after the range, or far away.
  uint32_t w_page = first + 1;
  uint32_t w_addr = w_page * kEg2RramPageBytes + kEg2RramPageBytes / 2;
  uint32_t w_last_page = (w_addr + 4 * n - 1) / kEg2RramPageBytes;
  uint32_t d =
      EG2_PARAM(distance) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(distance);
  uint32_t probe = d == 0   ? w_page * kEg2RramPageBytes
                   : d == 1 ? (w_last_page + 1) * kEg2RramPageBytes
                            : (first + kPages - 1) * kEg2RramPageBytes;
  eg2_chk_step("write at", kEg2RramHostBase + w_addr);
  eg2_chk_step("write words", n);
  eg2_chk_step("probe at", kEg2RramHostBase + probe);

  bool ok = true;
  int32_t k = (int32_t)eg2_rand_range(1, 2047);
  uint32_t insn = eg2_rram_insn_addi_a0(k);
  uint32_t pblk[8];
  eg2_rram_code_block(pblk, 4, 0, &insn, 1);
  for (uint32_t i = 4; i < 8; ++i) {
    pblk[i] = eg2_rand();
  }
  eg2_rram_result_t r;
  ok = eg2_chk_true("probe written",
                    eg2_rram_write(kEg2RramPartData, probe, pblk, 8, &r)) &&
       ok;
  ok = eg2_chk_eq("probe write op_status", 1, r.op_status) && ok;
  eg2_chk_fence_i();

  // The OTP write starts first and runs on beside the RRAM write.
  uint32_t otp_addr = OTP_CTRL_PARAM_VENDOR_TEST_OFFSET +
                      4 * eg2_rand_range(0, kOtpVendorTestWords - 1);
  uint32_t otp_data = eg2_rand() | 1;
  bool otp_on = EG2_PARAM(otp_write) != 0;
  if (otp_on) {
    ok = eg2_chk_true("OTP DAI idle before the write", otp_wait_idle()) && ok;
    eg2_chk_step("OTP write at", otp_addr);
    otp_dai_start(OTP_CTRL_DIRECT_ACCESS_CMD_WR_BIT, otp_addr, otp_data);
  }

  eg2_rram_start(kEg2RramOpWrite, kEg2RramPartData, w_addr, n);
  uint32_t probes = 0;
  uint32_t pushed = 0;
  uint32_t t0 = eg2_chk_cycles();
  for (uint32_t i = 0; i < n; ++i) {
    if (!eg2_chk_true("write FIFO room within the poll limit (progress)",
                      eg2_rram_push(eg2_rram_pattern(kSalt, i), kPolls))) {
      ok = false;
      break;
    }
    ++pushed;
    if (i % EG2_PARAM(probe_every) == 0) {
      uint32_t arg = eg2_rand();
      eg2_chk_trap_t t;
      uint32_t cause = eg2_chk_call(kEg2RramHostBase + probe, arg, &t);
      ok = eg2_chk_eq("probe fetch during the write", arg + (uint32_t)k,
                      cause == 0 ? t.value : ~(arg + (uint32_t)k)) &&
           ok;
      uint32_t j = 4 + eg2_rand_range(0, 3);
      ok = eg2_chk_eq("probe load during the write", pblk[j],
                      *eg2_rram_host(probe + 4 * j)) &&
           ok;
      ++probes;
    }
  }
  ok = eg2_chk_true("write finished", eg2_rram_wait_done(&r, kPolls)) && ok;
  uint32_t cycles = eg2_chk_cycles() - t0;
  ok = eg2_chk_eq("write op_status (done, no error)", 1, r.op_status) && ok;
  ok = eg2_chk_eq("write ERR_CODE", 0, r.err_code) && ok;
  ok = eg2_chk_eq("words pushed", n, pushed) && ok;
  eg2_chk_step("probes during the write", probes);
  eg2_chk_step("cycles of the write", cycles);

  if (otp_on) {
    ok = eg2_chk_true("OTP write finished", otp_wait_idle()) && ok;
    ok = eg2_chk_eq("OTP DAI_ERROR after the write", 0,
                    (otp(OTP_CTRL_STATUS_REG_OFFSET) >> 1) & 1) &&
         ok;
    otp_dai_start(OTP_CTRL_DIRECT_ACCESS_CMD_RD_BIT, otp_addr, 0);
    ok = eg2_chk_true("OTP read finished", otp_wait_idle()) && ok;
    ok = eg2_chk_eq("OTP word read back", otp_data,
                    otp(OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET)) &&
         ok;
    // The DAI's operation-done interrupt (an event, not an error) is cleared.
    otp_wr(OTP_CTRL_INTR_STATE_REG_OFFSET,
           1u << OTP_CTRL_INTR_STATE_OTP_OPERATION_DONE_BIT);
  }

  // Every written word, through the controller, 16 at a time.
  uint32_t buf[16];
  for (uint32_t i = 0; i < n; i += 16) {
    uint32_t m = n - i < 16 ? n - i : 16;
    ok = eg2_chk_true(
             "read back done",
             eg2_rram_read(kEg2RramPartData, w_addr + 4 * i, buf, m, &r)) &&
         ok;
    for (uint32_t j = 0; j < m; ++j) {
      if (buf[j] != eg2_rram_pattern(kSalt, i + j)) {
        ok = eg2_chk_eq("written word read back",
                        eg2_rram_pattern(kSalt, i + j), buf[j]) &&
             ok;
      }
    }
  }
  ok = eg2_chk_eq("read-back ERR_CODE", 0, r.err_code) && ok;
  eg2_rram_icache_set(icache_boot);
  eg2_rram_region_off(region);
  return eg2_chk_summary(8) && ok;
}
