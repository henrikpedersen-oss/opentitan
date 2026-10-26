// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_lc_otp_fault (EG2 verification spec REQ_PBF_16, "Post-boot faults"): a reset
 * during an OTP write and during a life-cycle transition. Check: the partition intact or the error
 * reported, never a silently wrong word; the transition counter counts the attempt or not, never
 * anything else, and the state is unchanged.
 *
 * References: otp_ctrl's programmer's guide (DAI writes; an OTP word is programmed once, and a
 * read reports its ECC error in ERR_CODE and STATUS.DAI_ERROR), lc_ctrl's theory of operation (the
 * counter is incremented FIRST; the counter and state encodings are written stroke by stroke so a
 * partial write never decodes as another valid value) and its programmer's guide (a software
 * request ends with the core disabled in POST_TRANSITION).
 *
 *   fault 0, OTP write: a word of VENDOR_TEST's scratch area that reads 0 (drawn with the seed;
 *     the partition must be unlocked) is written with a value drawn with the seed, and the
 *     software reset is requested at `fault_point`: right after the write command (0), after a few
 *     status polls (1), once the DAI is idle again (2). Boot 1: the reset reason is the software
 *     reset alone; the word reads 0 or the value written, or the read reports an error; with
 *     fault_point 2 it must read the value written.
 *   fault 1, life-cycle transition: an invalid transition (DEV -> PROD, PROD -> DEV, RMA -> DEV)
 *     is requested with the watchdog set to bite after 1 (0), 20 (1) or 400 (2) AON cycles. Boot 1:
 *     the reset reason is the watchdog alone; LC_STATE is the state before; LC_TRANSITION_CNT is
 *     the count before or one more (fault_point 2: one more).
 *
 * Knobs: fault 0-1, fault_point 0-2, model_gaps (0 on the chip; the model programs OTP at once, so
 * fault points 0 and 1 cannot fall inside the write there; and it ends the run in POST_TRANSITION
 * instead of running on to the watchdog, so fault 1 is skipped there and named).
 *
 * Built with EG2_PBF_INJECT_FAIL the first value check (boot 1's reset reason) expects bit 0
 * flipped: the run must fail with `PBF FAIL`.
 */

#include "hw/top/lc_ctrl_regs.h"   // Generated.
#include "hw/top/otp_ctrl_regs.h"  // Generated.
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_system.h"

EG2_TEST_CONFIG();

#define LC_OTP_FAULT_PARAMS(X) \
  X(fault, 0, 0, 1)            \
  X(fault_point, 1, 0, 2)      \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(LC_OTP_FAULT_PARAMS)

enum {
  kRetPhase = 0,
  kRetAddr = 1,
  kRetValue = 2,
  kRetState = 3,
  kRetCount = 4,
  kScratchWords = OTP_CTRL_PARAM_SCRATCH_SIZE / 4,
};

static const uint32_t kBite[3] = {1, 20, 400};

static bool otp_write_boot_0(void) {
  uint32_t d0 = 0;
  if (!eg2_pbf_step("VENDOR_TEST DAI digest read",
                    sys_otp_read32(OTP_CTRL_PARAM_VENDOR_TEST_DIGEST_OFFSET, &d0) == 0)) {
    return false;
  }
  uint32_t d1 = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET);
  if (!eg2_pbf_step("VENDOR_TEST is unlocked (digest 0)", d0 == 0 && d1 == 0)) {
    return false;
  }
  uint32_t start = eg2_rand_range(0, kScratchWords - 1), addr = 0xffffffffu;
  for (uint32_t i = 0; i < kScratchWords; ++i) {
    uint32_t a = OTP_CTRL_PARAM_SCRATCH_OFFSET + 4 * ((start + i) % kScratchWords), v = 1;
    if (sys_otp_read32(a, &v) == 0 && v == 0) {
      addr = a;
      break;
    }
  }
  if (!eg2_pbf_step("a blank scratch word found", addr != 0xffffffffu)) {
    return false;
  }
  uint32_t value = eg2_rand() | 1u;
  *eg2_pbf_ret(kRetPhase) = 1;
  *eg2_pbf_ret(kRetAddr) = addr;
  *eg2_pbf_ret(kRetValue) = value;
  eg2_pbf_note("OTP word address", addr);
  eg2_pbf_note("OTP word value", value);
  uint32_t point = EG2_PARAM(fault_point);
  sys_otp_write32_start(addr, value);
  if (point == 1) {
    for (uint32_t i = 0; i < 4; ++i) {
      (void)eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_STATUS_REG_OFFSET);
    }
  } else if (point == 2) {
    if (!eg2_pbf_step("DAI idle after the write", sys_otp_dai_idle())) {
      return false;
    }
  }
  eg2_pbf_sw_reset();
}

static bool otp_write_boot_1(void) {
  bool ok = eg2_pbf_check("reset reason: the software reset alone", kSysRstSw,
                          eg2_pbf_reset_reason());
  uint32_t addr = *eg2_pbf_ret(kRetAddr), value = *eg2_pbf_ret(kRetValue), got = 0;
  uint32_t err = sys_otp_read32(addr, &got);
  eg2_pbf_note("DAI read status", err);
  eg2_pbf_note("DAI read value", got);
  if (err != 0) {
    eg2_pbf_note("DAI error code (an interrupted write reported)", sys_otp_dai_err());
    ok = eg2_pbf_step("an error is allowed only for an interrupted write",
                      EG2_PARAM(fault_point) != 2) &&
         ok;
  } else if (EG2_PARAM(fault_point) == 2) {
    ok = eg2_pbf_check("a completed write reads its value", value, got) && ok;
  } else {
    ok = eg2_pbf_step("the word is blank or the value written, nothing else",
                      got == 0 || got == value) &&
         ok;
  }
  eg2_pbf_activity("OTP writes through a reset", ok ? 1 : 0);
  return ok;
}

static bool lc_boot_0(void) {
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("the transition request (eg2sim ends the run in POST_TRANSITION instead of "
                      "running on to the watchdog's bite)");
    return eg2_pbf_check("lc_ctrl STATUS", 3, sys_lc_status());
  }
  uint32_t state = sys_lc_state();
  eg2_puts("PBF note lc_otp_fault: life-cycle state ");
  eg2_puts(sys_lc_name(state));
  eg2_puts("\r\n");
  if ((eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_CLAIM_TRANSITION_IF_REGWEN_REG_OFFSET) & 1u) ==
      0) {
    eg2_pbf_gap("the software transition request",
                "CLAIM_TRANSITION_IF_REGWEN is 0: the boot code locked the transition interface");
    return eg2_pbf_check("lc_ctrl STATUS", 3, sys_lc_status());
  }
  uint32_t target = state == LC_CTRL_LC_STATE_STATE_VALUE_DEV
                        ? LC_CTRL_TRANSITION_TARGET_STATE_VALUE_PROD
                        : LC_CTRL_TRANSITION_TARGET_STATE_VALUE_DEV;
  uint32_t token[4] = {0, 0, 0, 0};
  *eg2_pbf_ret(kRetPhase) = 2;
  *eg2_pbf_ret(kRetState) = state;
  *eg2_pbf_ret(kRetCount) = sys_lc_count();
  eg2_pbf_note("LC_TRANSITION_CNT before", sys_lc_count());
  sys_wdog_bite_after(kBite[EG2_PARAM(fault_point)]);
  (void)sys_lc_request(target, token);
  for (uint32_t i = 0; i < 1000000; ++i) {
    eg2_pbf_spin(10);
  }
  return eg2_pbf_step("the watchdog reset came", false);
}

static bool lc_boot_1(void) {
  bool ok = eg2_pbf_check("reset reason: the watchdog alone", kSysRstWatchdog,
                          eg2_pbf_reset_reason());
  sys_wdog_off();
  uint32_t c0 = *eg2_pbf_ret(kRetCount), c = sys_lc_count();
  eg2_pbf_note("LC_TRANSITION_CNT after", c);
  ok = eg2_pbf_check("LC_STATE unchanged", *eg2_pbf_ret(kRetState), sys_lc_state()) && ok;
  if (EG2_PARAM(fault_point) == 2) {
    ok = eg2_pbf_check("LC_TRANSITION_CNT: the attempt counted", c0 + 1, c) && ok;
  } else {
    ok = eg2_pbf_step("LC_TRANSITION_CNT: the count before or one more", c == c0 || c == c0 + 1) &&
         ok;
  }
  ok = eg2_pbf_check("lc_ctrl STATUS ready again", 3, sys_lc_status()) && ok;
  eg2_pbf_activity("life-cycle requests through a reset", ok ? 1 : 0);
  return ok;
}

bool eg2_test_main(void) {
  eg2_pbf_begin("lc_otp_fault", EG2_PBF_INJECT);
  if (!eg2_rm_params_begin()) {
    return false;
  }
  uint32_t boot = eg2_pbf_boot_index();
  uint32_t phase = *eg2_pbf_ret(kRetPhase);
  if (boot == 0) {
    return EG2_PARAM(fault) == 0 ? otp_write_boot_0() : lc_boot_0();
  }
  if (boot == 1 && phase == 1) {
    return otp_write_boot_1();
  }
  if (boot == 1 && phase == 2) {
    return lc_boot_1();
  }
  return eg2_pbf_step("no further boot expected", false);
}
