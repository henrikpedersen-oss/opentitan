// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_lc_otp_sb (EG2 verification spec REQ_PBF_16, "Post-boot features in CHERIoT
 * mode"): OTP reads and a software life-cycle transition request after secure boot, in RISC-V and
 * CHERIoT mode.
 *
 * References: lc_ctrl's programmer's guide (STATUS READY and INITIALIZED with no error bits
 * before a transition; the claim, target, token and start; "all life cycle state transition
 * increments the LC_TRANSITION_CNT and moves the life cycle state into the temporary
 * POST_TRANSITION state - even if the transition was unsuccessful", the processor disabled until a
 * reset), its theory of operation (the counter is incremented FIRST, before the token is checked),
 * the device life-cycle specification (DEV and PROD reach RMA only with the RMA_UNLOCK token; DEV,
 * PROD and RMA never reach each other otherwise), and otp_ctrl's programmer's guide (the software
 * partitions are read both through the direct access interface and the SW_CFG window; a partition
 * digest reads the same through DAI and its DIGEST registers).
 *
 * Boot 0:
 *   - lc_ctrl STATUS is READY | INITIALIZED, nothing else; LC_STATE is DEV, PROD or RMA, and the
 *     state the bench loaded (`lc_expect` 1 DEV, 2 PROD, 3 RMA; 0 behind secure boot, where the
 *     signed image keeps its defaults: any of the three).
 *   - `partition` (0 VENDOR_TEST, 1 CREATOR_SW_CFG, 2 OWNER_SW_CFG): a word drawn with the seed,
 *     read through DAI and through the SW_CFG window, must be equal; the digest read through DAI
 *     equals the partition's DIGEST registers. A read-locked partition (READ_LOCK 0) must refuse
 *     the window read (a load access fault); DAI locked by the boot code (DIRECT_ACCESS_REGWEN 0)
 *     is reported, and only the window is read.
 *   - `transition` 1 (a conditional transition, DEV/PROD -> RMA, with a wrong token drawn with the
 *     seed; in RMA, which has no conditional transition, an invalid one instead) or 2 (an invalid
 *     target: DEV -> PROD, PROD -> DEV, RMA -> DEV): the watchdog is armed to bite after
 *     kBiteCycles (the core stops in POST_TRANSITION and cannot reset the chip itself), the request
 *     is made. 0: no request.
 * Boot 1 (after a request): the reset reason is the watchdog alone; LC_STATE is the state before,
 * LC_TRANSITION_CNT one more than before; STATUS is READY | INITIALIZED again.
 *
 * Knobs: partition 0-2, transition 0-2, lc_expect 0-3, model_gaps (0 on the chip; eg2sim does not
 * check the token, and it ends the run in POST_TRANSITION instead of running on to the watchdog:
 * the request is skipped there and named).
 * Gaps: a request with the right token (a real transition is permanent: chip_sw_lc_ctrl_transition
 * and the walkthrough tests make them, from the TAP); the functions each state gates (the
 * lc_ctrl_function_signals_table rows: debug, DFT, NVM debug), REQ_PBF_20 and REQ_SEC_04.
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (lc_ctrl STATUS) expects bit 0 flipped: the
 * run must fail with `PBF FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/lc_ctrl_regs.h"   // Generated.
#include "hw/top/otp_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define LC_OTP_SB_PARAMS(X) \
  X(partition, 2, 0, 2)     \
  X(transition, 1, 0, 2)    \
  X(lc_expect, 0, 0, 3)     \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(LC_OTP_SB_PARAMS)

enum {
  kBiteCycles = 400,  // 2 ms of the AON clock
  kStatusReady = (1u << LC_CTRL_STATUS_INITIALIZED_BIT) | (1u << LC_CTRL_STATUS_READY_BIT),
  kRetPhase = 0,
  kRetState = 1,
  kRetCount = 2,
};

// The software partitions: data offset and size (digest excluded), digest offset, the READ_LOCK
// and DIGEST_0 registers (otp_ctrl's generated map).
typedef struct part {
  uint32_t off, size, digest, read_lock, digest_reg;
} part_t;

static const part_t kParts[3] = {
    {OTP_CTRL_PARAM_VENDOR_TEST_OFFSET, OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET,
     OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET, OTP_CTRL_VENDOR_TEST_READ_LOCK_REG_OFFSET,
     OTP_CTRL_VENDOR_TEST_DIGEST_0_REG_OFFSET},
    {OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
     OTP_CTRL_PARAM_CREATOR_SW_CFG_DIGEST_OFFSET - OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
     OTP_CTRL_PARAM_CREATOR_SW_CFG_DIGEST_OFFSET, OTP_CTRL_CREATOR_SW_CFG_READ_LOCK_REG_OFFSET,
     OTP_CTRL_CREATOR_SW_CFG_DIGEST_0_REG_OFFSET},
    {OTP_CTRL_PARAM_OWNER_SW_CFG_OFFSET,
     OTP_CTRL_PARAM_OWNER_SW_CFG_DIGEST_OFFSET - OTP_CTRL_PARAM_OWNER_SW_CFG_OFFSET,
     OTP_CTRL_PARAM_OWNER_SW_CFG_DIGEST_OFFSET, OTP_CTRL_OWNER_SW_CFG_READ_LOCK_REG_OFFSET,
     OTP_CTRL_OWNER_SW_CFG_DIGEST_0_REG_OFFSET},
};

static uint32_t expected_state(void) {
  switch (EG2_PARAM(lc_expect)) {
    case 1:
      return LC_CTRL_LC_STATE_STATE_VALUE_DEV;
    case 2:
      return LC_CTRL_LC_STATE_STATE_VALUE_PROD;
    case 3:
      return LC_CTRL_LC_STATE_STATE_VALUE_RMA;
    default:
      return 0;
  }
}

static bool otp_reads(void) {
  const part_t *p = &kParts[EG2_PARAM(partition)];
  uint32_t addr = p->off + 4 * eg2_rand_range(0, p->size / 4 - 1);
  eg2_pbf_note("OTP word address", addr);
  bool ok = true;
  bool read_unlocked = eg2_dev_read32(kEg2DevOtpCtrlCore, p->read_lock) & 1u;
  bool dai = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_REGWEN_REG_OFFSET) & 1u;
  if (!read_unlocked) {
    eg2_rm_out_t out;
    volatile uint8_t *win_ptr = (volatile uint8_t *)eg2_dev(kEg2DevOtpCtrlCore) +
                                OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET + addr;
    eg2_rm_irq_off();
    uint32_t cause = eg2_rm_probe(win_ptr, kEg2RmLw, 0, &out);
    ok = eg2_pbf_check("a read-locked partition refuses the window read (mcause)",
                       kEg2RmCauseLoadFault, cause) &&
         ok;
    eg2_pbf_activity("OTP locked-partition reads refused", 1);
    return ok;
  }
  uint32_t win = sys_otp_window32(addr);
  eg2_pbf_note("SW_CFG window word", win);
  if (!dai) {
    eg2_pbf_gap("the DAI read of the partition",
                "DIRECT_ACCESS_REGWEN is 0: the boot code locked the direct access interface");
    return ok;
  }
  uint32_t v = 0;
  ok = eg2_pbf_check("DAI read status errors", 0, sys_otp_read32(addr, &v) & 0xffu) && ok;
  ok = eg2_pbf_check("DAI read equals the SW_CFG window", win, v) && ok;
  uint32_t d0 = 0;
  uint32_t err = sys_otp_read32(p->digest, &d0) & 0xffu;
  uint32_t d1 = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET);
  ok = eg2_pbf_check("DAI digest read status errors", 0, err) && ok;
  ok = eg2_pbf_check("digest low word: DAI equals DIGEST_0", eg2_dev_read32(kEg2DevOtpCtrlCore,
                                                                           p->digest_reg),
                     d0) &&
       ok;
  ok = eg2_pbf_check("digest high word: DAI equals DIGEST_1",
                     eg2_dev_read32(kEg2DevOtpCtrlCore, p->digest_reg + 4), d1) &&
       ok;
  eg2_pbf_activity("OTP reads compared", 3);
  return ok;
}

static bool request_transition(uint32_t state) {
  uint32_t kind = EG2_PARAM(transition);
  if (kind == 1 && EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("a wrong RMA token (eg2sim does not check tokens): an invalid target");
    kind = 2;
  }
  uint32_t target;
  if (kind == 1 && state != LC_CTRL_LC_STATE_STATE_VALUE_RMA) {
    target = LC_CTRL_TRANSITION_TARGET_STATE_VALUE_RMA;
    eg2_pbf_note("request: RMA with a wrong token", target);
  } else {
    target = state == LC_CTRL_LC_STATE_STATE_VALUE_DEV ? LC_CTRL_TRANSITION_TARGET_STATE_VALUE_PROD
                                                       : LC_CTRL_TRANSITION_TARGET_STATE_VALUE_DEV;
    eg2_pbf_note("request: an invalid target", target);
  }
  uint32_t token[4];
  for (uint32_t i = 0; i < 4; ++i) {
    token[i] = eg2_rand();
  }
  if ((eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET) & 1u) ==
      0) {
    eg2_pbf_gap("the software transition request",
                "CLAIM_TRANSITION_IF_REGWEN is 0: the boot code locked the transition interface");
    return true;
  }
  *eg2_pbf_ret(kRetPhase) = 1;
  *eg2_pbf_ret(kRetState) = state;
  *eg2_pbf_ret(kRetCount) = sys_lc_count();
  sys_wdog_bite_after(kBiteCycles);
  if (!eg2_pbf_step("transition interface claimed", sys_lc_request(target, token))) {
    sys_wdog_off();
    return false;
  }
  // The core stops in POST_TRANSITION; the watchdog's bite resets the chip.
  for (uint32_t i = 0; i < 1000000; ++i) {
    eg2_pbf_spin(10);
  }
  return eg2_pbf_step("the core stopped after the request", false);
}

static bool boot_1(void) {
  bool ok = eg2_pbf_check("reset reason: the watchdog alone", kSysRstWatchdog,
                          eg2_pbf_reset_reason());
  sys_wdog_off();
  ok = eg2_pbf_check("LC_STATE unchanged by the refused request", *eg2_pbf_ret(kRetState),
                     sys_lc_state()) &&
       ok;
  ok = eg2_pbf_check("LC_TRANSITION_CNT: the attempt counted", *eg2_pbf_ret(kRetCount) + 1,
                     sys_lc_count()) &&
       ok;
  ok = eg2_pbf_check("lc_ctrl STATUS ready again", kStatusReady, sys_lc_status()) && ok;
  eg2_pbf_activity("life-cycle requests refused", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("lc_otp", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  eg2_pbf_boot_stage();
  uint32_t boot = eg2_pbf_boot_index();
  if (boot == 1 && *eg2_pbf_ret(kRetPhase) == 1) {
    return boot_1();
  }
  if (boot != 0) {
    return eg2_pbf_step("no further boot expected", false);
  }
  bool ok = eg2_pbf_check("lc_ctrl STATUS: READY, INITIALIZED, no error", kStatusReady,
                          sys_lc_status());
  uint32_t state = sys_lc_state();
  eg2_puts("PBF note lc_otp: life-cycle state ");
  eg2_puts(sys_lc_name(state));
  eg2_puts("\r\n");
  ok = eg2_pbf_step("the state is DEV, PROD or RMA",
                    state == LC_CTRL_LC_STATE_STATE_VALUE_DEV ||
                        state == LC_CTRL_LC_STATE_STATE_VALUE_PROD ||
                        state == LC_CTRL_LC_STATE_STATE_VALUE_RMA) &&
       ok;
  if (expected_state() != 0) {
    ok = eg2_pbf_check("the state of the OTP image the bench loaded", expected_state(), state) && ok;
  }
  eg2_pbf_note("LC_TRANSITION_CNT", sys_lc_count());
  ok = otp_reads() && ok;
  eg2_pbf_gap("a request with the right token, and the functions each state gates",
              "a real transition is permanent (chip_sw_lc_ctrl_transition, the walkthroughs); "
              "the gated functions are REQ_PBF_20 and REQ_SEC_04");
  if (!ok || EG2_PARAM(transition) == 0) {
    return ok;
  }
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("the transition request (eg2sim ends the run in POST_TRANSITION instead of "
                      "running on to the watchdog's bite)");
    return ok;
  }
  return request_transition(state);
}
