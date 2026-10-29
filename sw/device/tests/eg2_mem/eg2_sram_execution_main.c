// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_execution_main_cheriot (EG2 verification spec REQ_SRAM_01, REQ_TL_05, "OTP and
 * SRAM in Both Modes"), CHERIoT mode only: code runs from main SRAM only within PCC's bounds and
 * only while sram_ctrl_main's execution enable allows it; a fetch outside either faults, exactly
 * at the first instruction either forbids.
 *
 * The documents the checks rest on:
 *  - sram_ctrl theory_of_operation.md "Code Execution from SRAM": with OTP HW_CFG1.EN_SRAM_IFETCH
 *    = MuBi8True execution is enabled exactly when EXEC = MuBi4True; otherwise it follows the life
 *    cycle's HW_DEBUG_EN (On in TEST_UNLOCKED0-7, DEV and RMA; lc_ctrl theory of operation). A
 *    refused fetch is answered with d_error (spec REQ_TL_05): an instruction access fault, mcause
 *    1, mtval and mepc the fetch address.
 *  - CHERIoT ISA (cheriot-sail ext_fetch_check_pc, CJAL, CJALR): a jump does not check the
 *    target's bounds; the fetch does, before the memory access, and a fetch outside PCC's bounds
 *    raises a CHERI exception (mcause 0x1c) with mtval = PCC (0x20) << 5 | BoundsViolation (1) =
 *    0x401 and mepcc's address the fetch address. CJALR to an untagged capability raises
 *    TagViolation (2) at the CJALR, with the register (ct1, x6): mtval 0xc2.
 *
 * Per call the test writes a routine to a buffer on the stack (main SRAM), draws EXEC and a PCC
 * class, and calls the routine through a capability with those bounds (eg2_xm_call). The
 * routine's entry is its third word, which jumps back to its first, so every class meets its
 * fault inside the routine's own flow:
 *
 *   w0  addi a0, a0, kH      (start late: the first fetch outside the bounds)
 *   w1  jal  x0, w3
 *   w2  jal  x0, w0          <- entry
 *   w3  addi a0, a0, k1 ... (n_body instructions)
 *   wN  c.jr ra ; c.nop      (end early: the fetch outside the bounds)
 *
 * | PCC class   | bounds                 | EXEC allows               | EXEC refuses             |
 * |-------------|------------------------|---------------------------|--------------------------|
 * | covering    | [w0, wN + 4)           | returns arg + kH + sum(k) | access fault at w2       |
 * | end early   | [w0, wN)               | CHERI 0x401 at wN         | access fault at w2       |
 * | start late  | [w1, wN + 4)           | CHERI 0x401 at w0         | access fault at w2       |
 * | untagged    | as covering, tag clear | CHERI 0xc2 at the CJALR   | CHERI 0xc2 at the CJALR  |
 *
 * The capability's tag, base and length are checked before each jump. The life-cycle state is
 * read from lc_ctrl's LC_STATE. Not covered: HW_DEBUG_EN Off with the OTP switch off (a
 * life-cycle state without debug; the legacy chip_sw_sram_ctrl_execution_main covers it in RISC-V
 * mode).
 *
 * Knobs:
 *   n_calls           calls (one routine, EXEC value and PCC class each)
 *   n_body            the routine's body instructions (at most; drawn per call from 1)
 *   w_exec_on         weights of EXEC: MuBi4True,
 *   w_exec_off          MuBi4False or another value that is not MuBi4True
 *   w_pcc_cover       weights of the PCC class (spec 55 : 15 : 15 : 15)
 *   w_pcc_end_early
 *   w_pcc_start_late
 *   w_pcc_untagged
 *   otp_ifetch        the vseq's OTP HW_CFG1.EN_SRAM_IFETCH: 1 MuBi8True (the image's value), 0
 *                     another value
 *   model_gaps        skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | the outcome of a call whose routine overlaps | MPACT-CHERIoT keeps decoded instructions     |
 * |   words an earlier call wrote (routines are  |   across a store to them and fence.i, and    |
 * |   placed one after another until the buffer  |   runs the old routine                       |
 * |   is full)                                   |                                              |
 * | start late: mtval's cause code               | MPACT-CHERIoT clears PCC's tag on the jump   |
 * |                                              |   below its base (TagViolation, 0x402); the  |
 * |                                              |   ISA checks bounds at the fetch (0x401)     |
 *
 * Built with EG2_SRAM_EXECUTION_MAIN_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `MEM FAIL`.
 */

#include "hw/top/lc_ctrl_regs.h"    // Generated.
#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

#ifndef __CHERIOT__
#error "chip_sw_sram_ctrl_execution_main_cheriot is a CHERIoT-mode test"
#endif

// CHERIoT mode: the boot stage leaves the REQ_SOC_08 locks open for this test (eg2_cheriot_start.S
// step 0b): it checks sram_ctrl's EXEC and EXEC_REGWEN themselves.
const volatile uint8_t eg2_cheriot_boot_locks_open __attribute__((used)) = 1;

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define XM_PARAMS(X)             \
  X(n_calls, 24, 1, 128)         \
  X(n_body, 16, 1, 32)           \
  X(w_exec_on, 50, 0, 100)       \
  X(w_exec_off, 50, 0, 100)      \
  X(w_pcc_cover, 55, 0, 100)     \
  X(w_pcc_end_early, 15, 0, 100) \
  X(w_pcc_start_late, 15, 0, 100) \
  X(w_pcc_untagged, 15, 0, 100)  \
  X(otp_ifetch, 1, 0, 1)         \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(XM_PARAMS)

/** The record of eg2_xm_call (eg2_sram_exec_call.S). */
typedef struct eg2_xm_trap {
  uint32_t mcause;
  uint32_t value;
  uint32_t mtval;
  uint32_t mepc;
  uint32_t site;
  uint32_t tag;
  uint32_t base;
  uint32_t len;
} eg2_xm_trap_t;

uint32_t eg2_xm_call(uint32_t base, uint32_t len, uint32_t entry, uint32_t untag, uint32_t arg,
                     eg2_xm_trap_t *out);

enum {
  kBufWords = 96,
  kHeadWords = 3,  // w0..w2
  kMtvalPccBounds = 0x20u << 5 | 0x1,
  kMtvalCt1Tag = 6u << 5 | 0x2,
  kPccCover = 0,
  kPccEndEarly = 1,
  kPccStartLate = 2,
  kPccUntagged = 3,
};

// The routine buffer is on the stack (main SRAM): in .bss it would take the globals over 4 KiB.

static uint32_t insn_addi_a0(uint32_t k) { return (k & 0xfff) << 20 | 10u << 15 | 10u << 7 | 0x13; }

/** jal x0, `off` bytes (J-type). */
static uint32_t insn_jal_x0(int32_t off) {
  uint32_t u = (uint32_t)off;
  return ((u >> 20) & 1) << 31 | ((u >> 1) & 0x3ff) << 21 | ((u >> 11) & 1) << 20 |
         ((u >> 12) & 0xff) << 12 | 0x6f;
}

/** True if the life cycle's HW_DEBUG_EN is On (lc_ctrl: TEST_UNLOCKED0-7, DEV, RMA). */
static bool lc_hw_debug_en(uint32_t st) {
  switch (st) {
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED0:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED1:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED2:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED3:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED4:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED5:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED6:
    case LC_CTRL_LC_STATE_STATE_VALUE_TEST_UNLOCKED7:
    case LC_CTRL_LC_STATE_STATE_VALUE_DEV:
    case LC_CTRL_LC_STATE_STATE_VALUE_RMA:
      return true;
    default:
      return false;
  }
}

static uint32_t draw_exec(void) {
  const uint32_t w[2] = {EG2_PARAM(w_exec_on), EG2_PARAM(w_exec_off)};
  if (eg2_chk_pick(w, 2) == 0) {
    return kMultiBitBool4True;
  }
  if (eg2_rand_range(0, 1) == 0) {
    return kMultiBitBool4False;
  }
  uint32_t v = eg2_rand_range(0, 15);
  return v == kMultiBitBool4True ? kMultiBitBool4False : v;
}

/** Writes the routine at `w`; returns its word count and its result for `arg`. */
static uint32_t write_routine(volatile uint32_t *w, uint32_t n_body, uint32_t arg,
                              uint32_t *result) {
  uint32_t kh = eg2_rand_range(1, 2047);
  uint32_t sum = arg + kh;
  w[0] = insn_addi_a0(kh);
  w[1] = insn_jal_x0(8);   // w1 -> w3
  w[2] = insn_jal_x0(-8);  // w2 -> w0
  for (uint32_t i = 0; i < n_body; ++i) {
    uint32_t k = eg2_rand_range(1, 2047);
    w[kHeadWords + i] = insn_addi_a0(k);
    sum += k;
  }
  w[kHeadWords + n_body] = kEg2ChkStubRetNop;
  *result = sum;
  return kHeadWords + n_body + 1;
}

// model_gaps: the next word no call has written yet.
static uint32_t cursor_;

static bool one_call(volatile uint32_t *buf, uint32_t i, bool otp_ifetch, bool hw_debug_en,
                     bool gaps) {
  bool ok = true;
  const uint32_t wp[4] = {EG2_PARAM(w_pcc_cover), EG2_PARAM(w_pcc_end_early),
                          EG2_PARAM(w_pcc_start_late), EG2_PARAM(w_pcc_untagged)};
  uint32_t pcc = eg2_chk_pick(wp, 4);
  if (pcc >= 4) {
    pcc = kPccCover;
  }
  uint32_t exec = draw_exec();
  uint32_t n_body = eg2_rand_range(1, EG2_PARAM(n_body));
  const uint32_t need = kHeadWords + n_body + 1;
  uint32_t first = eg2_rand_range(0, kBufWords - need);
  bool fresh = true;
  if (gaps) {
    fresh = cursor_ + need <= kBufWords;
    if (fresh) {
      first = cursor_;
      cursor_ += need;
    }
  }
  uint32_t arg = eg2_rand();
  uint32_t result = 0;
  uint32_t words = write_routine(&buf[first], n_body, arg, &result);
  uint32_t code = eg2_chk_addr_of(&buf[first]);
  uint32_t entry = code + 4 * 2;
  uint32_t last = code + 4 * (words - 1);

  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, exec);
  ok = eg2_chk_eq("EXEC read back", exec,
                  eg2_dev_read32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET) & 0xf) &&
       ok;
  eg2_chk_fence_i();
  bool enabled = otp_ifetch ? exec == kMultiBitBool4True : hw_debug_en;

  uint32_t base = code;
  uint32_t len = 4 * words;
  if (pcc == kPccEndEarly) {
    len -= 4;
  } else if (pcc == kPccStartLate) {
    base += 4;
    len -= 4;
  }
  eg2_chk_step("call", i);
  eg2_chk_step("EXEC", exec);
  eg2_chk_step("PCC class", pcc);
  eg2_chk_step("routine at", code);
  eg2_chk_step("routine words", words);

  eg2_xm_trap_t t;
  uint32_t cause = eg2_xm_call(base, len, entry, pcc == kPccUntagged, arg, &t);
  ok = eg2_chk_eq("capability tag", pcc == kPccUntagged ? 0 : 1, t.tag) && ok;
  ok = eg2_chk_eq("capability base (exact bounds)", base, t.base) && ok;
  ok = eg2_chk_eq("capability length (exact bounds)", len, t.len) && ok;

  if (pcc != kPccUntagged && enabled && !fresh) {
    eg2_chk_skip("outcome over rewritten words (MPACT-CHERIoT runs the old routine)");
  } else if (pcc == kPccUntagged) {
    ok = eg2_chk_eq("untagged jump: mcause (CHERI)", kEg2ChkCauseCheri, cause) && ok;
    ok = eg2_chk_eq("untagged jump: mtval (ct1, TagViolation)", kMtvalCt1Tag, t.mtval) && ok;
    ok = eg2_chk_eq("untagged jump: mepc (the CJALR)", t.site, t.mepc) && ok;
  } else if (!enabled) {
    ok = eg2_chk_eq("fetch refused by EXEC: mcause (instruction access fault)",
                    kEg2ChkCauseFetchFault, cause) &&
         ok;
    ok = eg2_chk_eq("fetch refused by EXEC: mtval (the entry)", entry, t.mtval) && ok;
    ok = eg2_chk_eq("fetch refused by EXEC: mepc (the entry)", entry, t.mepc) && ok;
  } else if (pcc == kPccCover) {
    ok = eg2_chk_eq("routine in bounds: mcause (none)", 0, cause) && ok;
    ok = eg2_chk_eq("routine in bounds: result", result, t.value) && ok;
  } else {
    uint32_t at = pcc == kPccEndEarly ? last : code;
    ok = eg2_chk_eq("fetch outside PCC: mcause (CHERI)", kEg2ChkCauseCheri, cause) && ok;
    if (gaps && pcc == kPccStartLate) {
      eg2_chk_skip("start late: mtval (MPACT-CHERIoT reports TagViolation)");
    } else {
      ok = eg2_chk_eq("fetch outside PCC: mtval (PCC, BoundsViolation)", kMtvalPccBounds,
                      t.mtval) &&
           ok;
    }
    ok = eg2_chk_eq("fetch outside PCC: mepc (the first fetch outside)", at, t.mepc) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool ok = true;
  ok = eg2_chk_eq("EXEC_REGWEN at the start", 1,
                  eg2_dev_read32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REGWEN_REG_OFFSET) & 1) &&
       ok;
  const bool otp_ifetch = EG2_PARAM(otp_ifetch) != 0;
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  const uint32_t lc = eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_STATE_REG_OFFSET) &
                      LC_CTRL_LC_STATE_STATE_MASK;
  const bool hw_debug_en = lc_hw_debug_en(lc);
  eg2_chk_step("LC_STATE", lc);
  eg2_chk_step("OTP EN_SRAM_IFETCH MuBi8True", otp_ifetch);
  eg2_chk_step("HW_DEBUG_EN", hw_debug_en);
  volatile uint32_t buf[kBufWords];

  for (uint32_t i = 0; i < EG2_PARAM(n_calls); ++i) {
    ok = one_call(buf, i, otp_ifetch, hw_debug_en, gaps) && ok;
  }
  // Execution on again (EXEC's reset value refuses it), then one call that must run wherever
  // execution is enabled at all.
  const uint32_t at = gaps ? cursor_ : 0;
  uint32_t result = 0;
  uint32_t words = 0;
  if (at + kHeadWords + 2 <= kBufWords) {
    words = write_routine(&buf[at], 1, 0, &result);
  }
  uint32_t code = eg2_chk_addr_of(&buf[at]);
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMultiBitBool4True);
  eg2_chk_fence_i();
  if (words == 0) {
    eg2_chk_skip("final call (no unwritten words left for MPACT-CHERIoT)");
  } else if (otp_ifetch || hw_debug_en) {
    eg2_xm_trap_t t;
    uint32_t cause = eg2_xm_call(code, 4 * words, code + 8, 0, 0, &t);
    ok = eg2_chk_eq("final call with EXEC on: mcause", 0, cause) && ok;
    ok = eg2_chk_eq("final call with EXEC on: result", result, t.value) && ok;
  }
  return eg2_chk_summary(EG2_PARAM(n_calls) * 4 + 1) && ok;
}
