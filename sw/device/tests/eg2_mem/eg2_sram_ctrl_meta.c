// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_meta (EG2 verification spec REQ_CMS_14, REQ_OTP_02, REQ_SEC_18; "Memory
 * subsystem in RISC-V mode"): the procedures of the chip_sram_ctrl testpoints
 * chip_sw_sram_scrambled_access, chip_sw_sram_lc_escalation and chip_sw_sram_readback, run on
 * `sram_ctrl_meta`, the SRAM that holds the CHERIoT tags and the revocation bitmap.
 *
 * Software reaches the meta SRAM only through `cheriot_mem_sys.revbm` (0x11000000, 3 KiB: the
 * revocation bitmap at the base of the meta SRAM, top_earlgrey.hjson), and only in CHERIoT mode
 * (REQ_CMS_04, REQ_CMS_09; RFC CHERIoT in OpenTitan p4 "In ePMP mode, this memory will not be
 * accessible"). The expected values:
 *   - CTRL.RENEW_SCR_KEY fetches a key from OTP: STATUS.SCR_KEY_VALID, SCR_KEY_ROTATED MuBi4True
 *     (sram_ctrl registers.md); the key is OTP's slot 3 (the bench compares, REQ_OTP_02);
 *   - CTRL.INIT: STATUS.INIT_DONE without INIT_ERROR; SecZeroInit = 1 for this instance
 *     (top_earlgrey.hjson), so the bitmap reads zero afterwards (REQ_CMS_14 "INIT zeroes it");
 *   - lc_escalate_en: STATUS.ESCALATED, the key reset to its netlist constant and bus transactions
 *     blocked (sram_ctrl interfaces.md "Global and Local Escalation"); after the reset the
 *     controller works again (the testpoint: "Re-initialize the SRAMs and verify that they can now
 *     respond");
 *   - READBACK: enabled and disabled, the data still correct and STATUS.READBACK_ERROR clear;
 *     READBACK_REGWEN cleared locks it (registers.md).
 *
 * Each operation is drawn by its weight; `n_ops` of them in a run:
 *   access   CHERIoT mode: `n_words` words of the bitmap part that covers sec SRAM (bytes
 *            0x800-0xBFF: no capability of the test is based there) written with a pattern from
 *            the seed, read back, the bench checks the RAM holds ciphertext (`EG2 MEM meta
 *            written`), then the words cleared again. RISC-V mode: a load and a store to a drawn
 *            bitmap word must fault (access fault, the window refused), the RAM word unchanged
 *            (the bench, between `EG2 MEM meta snap` and `EG2 MEM meta same`).
 *   init     RISC-V mode: CTRL.INIT, INIT_DONE, no INIT_ERROR; the bench sees every sampled RAM
 *            word written. CHERIoT mode: replaced by `access`: an INIT clears every tag in the
 *            chip, the running test's own (its stack's return capabilities, its globals'
 *            pointers) included, so no software survives it; the CHERIoT start-up code's INIT is
 *            checked by the first `access` reading zero before it writes.
 *   renew    RISC-V mode: CTRL.RENEW_SCR_KEY; SCR_KEY_VALID, SCR_KEY_ROTATED; the bench checks the
 *            controller took OTP's new key for slot 3. CHERIoT mode: replaced by `access` (as
 *            INIT: the old tags turn to garbage).
 *   escalate RISC-V mode, at most once and last: the bench forces `lc_escalate_en` at
 *            sram_ctrl_meta (`EG2 MEM meta escalate`); STATUS.ESCALATED; then the bench resets
 *            the chip with boot_phase = 1, and the second boot checks a clean controller that
 *            renews its key and initialises. CHERIoT mode: replaced by `access` (with the meta
 *            SRAM blocked every store to tagged memory errors, the test's stack included).
 *   readback READBACK MuBi4True, an `access` (CHERIoT) under it, READBACK_ERROR clear, READBACK
 *            MuBi4False; the first one also locks READBACK_REGWEN and checks a write of
 *            MuBi4True no longer takes.
 *
 * Knobs: n_ops, w_op_access/init/renew/escalate/readback (40 : 15 : 15 : 10 : 20), n_words
 * (16-1024, capped at the 256 words of the test's bitmap part), boot_phase (the bench's),
 * model_gaps.
 *
 * | Skipped with model_gaps = 1                  | Why                                          |
 * |----------------------------------------------|----------------------------------------------|
 * | RISC-V mode: the window refusing loads and   | eg2sim maps the meta SRAM behind the bitmap  |
 * |   stores                                     |   window in both modes (model finding:       |
 * |                                              |   REQ_CMS_04 refuses it outside CHERIoT mode)|
 * | escalate: STATUS.ESCALATED and the second    | no bench: nothing forces lc_escalate_en      |
 * |   boot                                       |                                              |
 *
 * Built with EG2_SRAM_CTRL_META_INJECT_FAIL the first value check expects bit 0 flipped: the run
 * must fail with `MEM FAIL`.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define META_PARAMS(X)          \
  X(n_ops, 6, 1, 16)            \
  X(w_op_access, 40, 0, 100)    \
  X(w_op_init, 15, 0, 100)      \
  X(w_op_renew, 15, 0, 100)     \
  X(w_op_escalate, 10, 0, 100)  \
  X(w_op_readback, 20, 0, 100)  \
  X(n_words, 64, 16, 1024)      \
  X(boot_phase, 0, 0, 1)        \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(META_PARAMS)

enum {
  kMubiTrue = 0x6,
  kMubiFalse = 0x9,
  kPolls = 200000,
  kWin = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR,
  kWinBytes = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_SIZE_BYTES,
  // The bitmap bytes that cover sec SRAM: one bit per 8-byte granule from main SRAM's base.
  kOwnFirst = (TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR - TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR) /
              64,
  kOwnWords = (kWinBytes - kOwnFirst) / 4,
};

enum { kOpAccess, kOpInit, kOpRenew, kOpEscalate, kOpReadback, kOpCount };

static uint32_t meta_rd(uint32_t off) { return eg2_dev_read32(kEg2DevSramCtrlMetaRegs, off); }
static void meta_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevSramCtrlMetaRegs, off, v); }

/** The pattern the bench recomputes (chip_sw_eg2_sram_ctrl_meta_vseq::pattern). */
static uint32_t pattern(uint32_t seed, uint32_t i) {
  uint32_t x = seed ^ (i * 0x9e3779b9u);
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  return x;
}

static bool wait_status(uint32_t bit, uint32_t *status) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    *status = meta_rd(SRAM_CTRL_STATUS_REG_OFFSET);
    if (*status & (1u << bit)) {
      return true;
    }
  }
  return false;
}

static const uint32_t kErrBits = (1u << SRAM_CTRL_STATUS_BUS_INTEG_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_INIT_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT) |
                                 (1u << SRAM_CTRL_STATUS_SRAM_ALERT_BIT);

/** CHERIoT mode: the scrambled access through the bitmap window. */
static bool access_cheriot(void) {
  bool ok = true;
  uint32_t n = EG2_PARAM(n_words);
  if (n > kOwnWords) {
    n = kOwnWords;
  }
  uint32_t first = kOwnFirst / 4 + eg2_rand_range(0, kOwnWords - n);
  uint32_t seed = eg2_rand();
  volatile uint32_t *w = eg2_chk_ptr(kWin);
  // Nothing of the test is revoked: its part of the bitmap holds zero (the start-up code's INIT,
  // or the previous access).
  ok = eg2_chk_eq("bitmap word before the write (INIT zeroed it)", 0, w[first]) && ok;
  for (uint32_t i = 0; i < n; ++i) {
    w[first + i] = pattern(seed, i);
  }
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t got = w[first + i];
    if (got != pattern(seed, i)) {
      ok = eg2_chk_eq("bitmap word read back", pattern(seed, i), got) && ok;
      break;
    }
  }
  ok = eg2_chk_true("bitmap words read back", ok);
  LOG_INFO("EG2 MEM meta written 0x%08x %u %u", seed, first, n);
  for (uint32_t i = 0; i < n; ++i) {
    w[first + i] = 0;
  }
  return ok;
}

/** RISC-V mode: the window refuses loads and stores. */
static bool access_riscv(bool gaps) {
  if (gaps) {
    eg2_chk_skip("bitmap window refused in RISC-V mode (eg2sim maps it in both modes)");
    return true;
  }
  bool ok = true;
  uint32_t word = eg2_rand_range(0, kWinBytes / 4 - 1);
  LOG_INFO("EG2 MEM meta snap %u", word);
  eg2_chk_trap_t t;
  uint32_t c = eg2_chk_access(eg2_chk_ptr(kWin + 4 * word), kEg2ChkLw, 0, &t);
  ok = eg2_chk_eq("bitmap load in RISC-V mode (access fault)", kEg2ChkCauseLoadFault, c) && ok;
  c = eg2_chk_access(eg2_chk_ptr(kWin + 4 * word), kEg2ChkSw, eg2_rand(), &t);
  ok = eg2_chk_eq("bitmap store in RISC-V mode (access fault)", kEg2ChkCauseStoreFault, c) && ok;
  LOG_INFO("EG2 MEM meta same %u", word);
  return ok;
}

static bool do_init(void) {
  uint32_t st = 0;
  LOG_INFO("EG2 MEM meta init begin");
  meta_wr(SRAM_CTRL_CTRL_REG_OFFSET, 1u << SRAM_CTRL_CTRL_INIT_BIT);
  bool ok = eg2_chk_true("INIT_DONE", wait_status(SRAM_CTRL_STATUS_INIT_DONE_BIT, &st));
  ok = eg2_chk_eq("status error bits after INIT", 0, st & kErrBits) && ok;
  LOG_INFO("EG2 MEM meta init done");
  return ok;
}

static bool do_renew(void) {
  uint32_t st = 0;
  LOG_INFO("EG2 MEM meta renew begin");
  meta_wr(SRAM_CTRL_CTRL_REG_OFFSET, 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT);
  bool ok = eg2_chk_true("SCR_KEY_VALID", wait_status(SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT, &st));
  ok = eg2_chk_eq("SCR_KEY_ROTATED after the renewal", kMubiTrue,
                  meta_rd(SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) & 0xf) &&
       ok;
  // rw1c of a MuBi4: writing MuBi4True clears it to MuBi4False.
  meta_wr(SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET, kMubiTrue);
  ok = eg2_chk_eq("SCR_KEY_ROTATED cleared", kMubiFalse,
                  meta_rd(SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) & 0xf) &&
       ok;
  ok = eg2_chk_eq("status error bits after the renewal", 0, st & kErrBits) && ok;
  LOG_INFO("EG2 MEM meta renew done");
  return ok;
}

static bool readback_locked;

static bool do_readback(bool cheriot) {
  bool ok = true;
  if (readback_locked) {
    meta_wr(SRAM_CTRL_READBACK_REG_OFFSET, kMubiTrue);
    return eg2_chk_eq("READBACK still off under its lock", kMubiFalse,
                      meta_rd(SRAM_CTRL_READBACK_REG_OFFSET) & 0xf);
  }
  meta_wr(SRAM_CTRL_READBACK_REG_OFFSET, kMubiTrue);
  ok = eg2_chk_eq("READBACK on", kMubiTrue, meta_rd(SRAM_CTRL_READBACK_REG_OFFSET) & 0xf) && ok;
  if (cheriot) {
    ok = access_cheriot() && ok;
  }
  ok = eg2_chk_eq("READBACK_ERROR after accesses under readback", 0,
                  meta_rd(SRAM_CTRL_STATUS_REG_OFFSET) &
                      (1u << SRAM_CTRL_STATUS_READBACK_ERROR_BIT)) &&
       ok;
  meta_wr(SRAM_CTRL_READBACK_REG_OFFSET, kMubiFalse);
  meta_wr(SRAM_CTRL_READBACK_REGWEN_REG_OFFSET, 0);
  readback_locked = true;
  meta_wr(SRAM_CTRL_READBACK_REG_OFFSET, kMubiTrue);
  ok = eg2_chk_eq("READBACK locked off", kMubiFalse,
                  meta_rd(SRAM_CTRL_READBACK_REG_OFFSET) & 0xf) &&
       ok;
  ok = eg2_chk_eq("READBACK_REGWEN cleared", 0,
                  meta_rd(SRAM_CTRL_READBACK_REGWEN_REG_OFFSET) & 1) &&
       ok;
  return ok;
}

/** RISC-V mode: the bench escalates the controller and resets the chip; never returns there. */
static bool do_escalate(bool gaps) {
  if (gaps) {
    eg2_chk_skip("escalation (no bench forces lc_escalate_en)");
    return true;
  }
  uint32_t st = 0;
  EG2_CHK_SYNC("EG2 MEM meta escalate");
  bool ok = eg2_chk_true("STATUS.ESCALATED", wait_status(SRAM_CTRL_STATUS_ESCALATED_BIT, &st));
  EG2_CHK_SYNC("EG2 MEM meta escalated");
  // The bench resets the chip now; the second boot (boot_phase 1) goes on.
  eg2_chk_delay(2000000);
  return eg2_chk_true("the bench's reset after the escalation", false) && ok;
}

/** The second boot after the escalation. */
static bool phase_after_escalation(void) {
  uint32_t st = meta_rd(SRAM_CTRL_STATUS_REG_OFFSET);
  bool ok = eg2_chk_eq("ESCALATED after the reset", 0,
                       st & (1u << SRAM_CTRL_STATUS_ESCALATED_BIT));
  ok = eg2_chk_eq("status error bits after the reset", 0, st & kErrBits) && ok;
  ok = do_renew() && ok;
  ok = do_init() && ok;
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
#ifdef __CHERIOT__
  const bool cheriot = true;
#else
  const bool cheriot = false;
#endif
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  if (EG2_PARAM(boot_phase) == 1) {
    eg2_chk_step("second boot after the escalation", 1);
    return phase_after_escalation() && eg2_chk_summary(3);
  }
  bool ok = true;
  const uint32_t w[kOpCount] = {EG2_PARAM(w_op_access), EG2_PARAM(w_op_init),
                                EG2_PARAM(w_op_renew), EG2_PARAM(w_op_escalate),
                                EG2_PARAM(w_op_readback)};
  bool escalate = false;
  for (uint32_t i = 0; i < EG2_PARAM(n_ops); ++i) {
    uint32_t op = eg2_chk_pick(w, kOpCount);
    if (op >= kOpCount) {
      op = kOpAccess;
    }
    if (cheriot && (op == kOpInit || op == kOpRenew || op == kOpEscalate)) {
      eg2_chk_step("CHERIoT mode: INIT, renewal and escalation clear the running test's tags; "
                   "access instead of op",
                   op);
      op = kOpAccess;
    }
    if (op == kOpEscalate) {
      escalate = true;  // last, once: it ends the first boot
      continue;
    }
    eg2_chk_step("op", op);
    switch (op) {
      case kOpAccess:
        ok = (cheriot ? access_cheriot() : access_riscv(gaps)) && ok;
        break;
      case kOpInit:
        ok = do_init() && ok;
        break;
      case kOpRenew:
        ok = do_renew() && ok;
        break;
      default:
        ok = do_readback(cheriot) && ok;
        break;
    }
  }
  if (escalate) {
    eg2_chk_step("op", kOpEscalate);
    ok = do_escalate(gaps) && ok;
  }
  return eg2_chk_summary(1) && ok;
}
