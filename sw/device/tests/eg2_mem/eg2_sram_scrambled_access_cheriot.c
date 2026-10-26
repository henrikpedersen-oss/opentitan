// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_sram_ctrl_scrambled_access_cheriot (EG2 verification spec REQ_OTP_02, "Keys from OTP in
 * CHERIoT mode"; the chip_sw_sram_scrambled_access testpoint): what OpenTitan's
 * sram_ctrl_scrambled_access_test.c checks, in purecap CHERIoT code. That test is RV32 inline
 * assembly and rewrites OTTF's trap frame; it stays as it is for RISC-V mode. Here every access
 * goes through a capability, the main SRAM re-key is CHERIoT assembly working from registers only
 * (eg2_sram_scrambled_access_main.S), and a load that meets an integrity error is caught by
 * eg2_chk_access's own trap vector, not by OTTF's.
 *
 * For each SRAM a frame: `n` pattern words (the `n_words` knob, capped per SRAM below), then 16
 * "backdoor" words.
 *  0. Sec and ret SRAM only: `rekey` key renewals first (`EG2 MEM scr <sram> rekey`), each checked
 *     by the bench as in step 2. Main SRAM gets none: each would destroy the test's stack.
 *  1. Prepare (as the OpenTitan test): pattern 2 then pattern 1 written, pattern 1 read back.
 *  2. `EG2 MEM scr <sram> renew 0x<backdoor words>`, then CTRL.RENEW_SCR_KEY and
 *     STATUS.SCR_KEY_VALID. The bench (chip_sw_eg2_sram_scrambled_access_cheriot_vseq) takes the
 *     key and nonce otp_ctrl's key derivation hands that controller (its slot: main 0, sec 1, ret
 *     2) and checks the controller now holds exactly them (REQ_OTP_02: the key reaches its
 *     consumer). For main and ret it then writes the backdoor words through the RAM backdoor,
 *     scrambled with that key, with integrity: data the software must read back descrambled (the
 *     OpenTitan bench's kBackdoorExpectedBytes, here derived from `bd_seed`). Sec SRAM has no
 *     backdoor model in the bench, so the software writes its backdoor words itself under the new
 *     key.
 *  3. The pattern words are unreadable: neither pattern 1 nor pattern 2 as a whole, and few of the
 *     loads pass their integrity check: at most 3 for up to 32 words (the OpenTitan test's
 *     false-positive floor; a random 39-bit word is a valid code word with probability 1/128), at
 *     most n/8 above that. A load that fails it raises the core's internal interrupt or traps; both
 *     count.
 *  4. The backdoor words read back exactly, with no integrity error.
 *
 * Boot 1 (power-on): sec SRAM in place; then main SRAM, whose re-key destroys the stack, so the
 *   frame is copied by the assembly, after the renewal, to the retention SRAM's owner area,
 *   followed by the error counts, and a software reset ends the boot (as the OpenTitan test does).
 *   Boot 1's own failures go there too, as the core's console does not survive.
 * Boot 2 (software reset): main SRAM's copied frame checked (steps 3 and 4, from the counts the
 *   assembly kept); then retention SRAM in place, after CTRL.INIT (the OpenTitan test's wipe).
 *
 * The integrity errors raise rv_core_ibex's fatal_hw_err (the core's major bus alert); it is
 * declared to the status module when its cause is set, as the OpenTitan test tells OTTF to ignore
 * it. Its cause survives the software reset (the alert handler is reset with the life cycle).
 *
 * Knobs: n_words (16-1024; capped at 112 for main SRAM, the stack buffer, and 512 for retention
 * SRAM, 4 KiB), rekey (0-4), bd_seed (the bench's), model_gaps.
 *
 * | Skipped with model_gaps = 1               | Why                                          |
 * |-------------------------------------------|----------------------------------------------|
 * | the integrity-error counts (step 3)       | eg2sim garbles renewed data but models no    |
 * |                                           |   integrity errors                           |
 * | the main and ret backdoor words (step 4)  | no bench writes them                         |
 *
 * Built with EG2_SRAM_SCRAMBLED_ACCESS_CHERIOT_INJECT_FAIL the first value check of each boot
 * expects bit 0 flipped: the run must fail with `MEM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "hw/top/sram_ctrl_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_mem/eg2_chk.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define SCR_PARAMS(X)               \
  X(n_words, 16, 16, 1024)          \
  X(rekey, 0, 0, 4)                 \
  X(bd_seed, 0x5eed, 0, 0xffffffff) \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(SCR_PARAMS)

enum {
  kPatternLen = 16,
  kBdWords = 16,
  kPolls = 200000,
  kRetBase = TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR,
  kRetSize = TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES,
  kSecBase = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR,
  kSecSize = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES,
  kRetMaxWords = 512,
  // The main SRAM frame: somewhere in a stack buffer.
  kMainBufWords = 128,
  kMainMaxWords = kMainBufWords - kBdWords,
  // The main SRAM frame's copy in the retention SRAM's owner area (retention_sram.h: the owner
  // area starts at 2048; ROM, ROM_EXT and the test ROM leave it alone): magic, boot 1's failed
  // checks, the pattern word count, the frame, the interrupt count, the trap count.
  kRefAddr = kRetBase + 2048 + 256,
  kRefMagic = 0,
  kRefFails = 1,
  kRefWords = 2,
  kRefFrame = 3,
  kRefMagicValue = 0x5c4a3b1e,
  kResetPor = 1u << RSTMGR_RESET_INFO_POR_BIT,
  kResetSw = 1u << RSTMGR_RESET_INFO_SW_RESET_BIT,
};

// The SRAMs, by their key slot in otp_ctrl's key derivation (the bench's numbering).
enum { kMain = 0, kSec = 1, kRet = 2 };

// The OpenTitan test's patterns; word i of a longer frame takes entry i % 16.
static const uint32_t kPattern1[kPatternLen] = {
    0xA5A5A5A5, 0xA23DE94C, 0xD82A4FB0, 0xE3CA4D62, 0xA5A5A5A5, 0xA23DE94C,
    0xD82A4FB0, 0xE3CA4D62, 0xA5A5A5A5, 0xA23DE94C, 0xD82A4FB0, 0xE3CA4D62,
    0xA5A5A5A5, 0xA23DE94C, 0xD82A4FB0, 0xE3CA4D62,
};
static const uint32_t kPattern2[kPatternLen] = {
    0x5A5A5A5A, 0x3CFB4A77, 0x304C6528, 0xFAEFD5CC, 0x5A5A5A5A, 0x3CFB4A77,
    0x304C6528, 0xFAEFD5CC, 0x5A5A5A5A, 0x3CFB4A77, 0x304C6528, 0xFAEFD5CC,
    0x5A5A5A5A, 0x3CFB4A77, 0x304C6528, 0xFAEFD5CC,
};

/** The backdoor words the bench writes (chip_sw_eg2_sram_scrambled_access_cheriot_vseq::bd). */
static uint32_t backdoor(uint32_t sram, uint32_t i) {
  uint32_t x = EG2_PARAM(bd_seed) ^ (sram * 0x01000193u) ^ (i * 0x9e3779b9u);
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  return x;
}

/** The assembly (eg2_sram_scrambled_access_main.S); never returns. */
extern void eg2_scr_main_rekey(volatile uint32_t *sram_regs, volatile uint32_t *src,
                               volatile uint32_t *dst, uint32_t words, volatile uint32_t *rstmgr)
    __attribute__((noreturn));

/** What steps 3 and 4 count over a frame. */
typedef struct scr_tally {
  uint32_t n;          // pattern words
  uint32_t eq1, eq2;   // pattern words still equal to pattern 1, pattern 2
  uint32_t errors;     // integrity errors on the pattern words
  uint32_t bd_bad;     // backdoor words read back wrong
  uint32_t bd_errors;  // integrity errors on the backdoor words
} scr_tally_t;

static eg2_dev_id_t regs_of(uint32_t sram) {
  return sram == kMain ? kEg2DevSramCtrlMainRegs
                       : sram == kSec ? kEg2DevSramCtrlSecRegs : kEg2DevSramCtrlRetRegs;
}

static const char *name_of(uint32_t sram) {
  return sram == kMain ? "main" : sram == kSec ? "sec" : "ret";
}

static uint32_t words_for(uint32_t sram) {
  uint32_t n = EG2_PARAM(n_words);
  uint32_t cap = sram == kMain ? kMainMaxWords : sram == kRet ? kRetMaxWords : n;
  return n < cap ? n : cap;
}

static bool ctrl_and_wait(uint32_t sram, uint32_t ctrl_bit, uint32_t status_bit) {
  eg2_dev_write32(regs_of(sram), SRAM_CTRL_CTRL_REG_OFFSET, 1u << ctrl_bit);
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (eg2_dev_read32(regs_of(sram), SRAM_CTRL_STATUS_REG_OFFSET) & (1u << status_bit)) {
      return true;
    }
  }
  return false;
}

/** Step 1: pattern 2, then pattern 1, read back as pattern 1. */
static bool prepare(volatile uint32_t *f, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    f[i] = kPattern2[i % kPatternLen];
  }
  for (uint32_t i = 0; i < n; ++i) {
    f[i] = kPattern1[i % kPatternLen];
  }
  uint32_t bad = 0;
  for (uint32_t i = 0; i < n; ++i) {
    bad += f[i] != kPattern1[i % kPatternLen];
  }
  return eg2_chk_eq("pattern words read back wrong before the renewal", 0, bad);
}

/** Word i of the frame, read (`got`) with or without an integrity error (`error`). */
static void tally_word(scr_tally_t *t, uint32_t sram, uint32_t i, uint32_t got, bool error) {
  if (i < t->n) {
    t->eq1 += got == kPattern1[i % kPatternLen];
    t->eq2 += got == kPattern2[i % kPatternLen];
    t->errors += error;
  } else {
    t->bd_bad += got != backdoor(sram, i - t->n);
    t->bd_errors += error;
  }
}

/** Steps 3 and 4. */
static bool check_tally(uint32_t sram, const scr_tally_t *t, bool bench_backdoor, bool gaps) {
  bool ok = true;
  uint32_t floor = t->n <= 32 ? 3 : t->n / 8;
  eg2_chk_step(name_of(sram), sram);
  eg2_chk_step("pattern words", t->n);
  eg2_chk_step("pattern words still pattern 1", t->eq1);
  eg2_chk_step("integrity errors on the pattern words", t->errors);
  ok = eg2_chk_true("pattern words are not pattern 1 after the renewal", t->eq1 != t->n) && ok;
  ok = eg2_chk_true("pattern words are not pattern 2 after the renewal", t->eq2 != t->n) && ok;
  if (gaps) {
    eg2_chk_skip("integrity errors on renewed data (not modelled)");
  } else {
    ok = eg2_chk_true("no more integrity errors than pattern words", t->errors <= t->n) && ok;
    ok = eg2_chk_true("few pattern words pass their integrity check", t->n - t->errors <= floor) &&
         ok;
  }
  if (gaps && bench_backdoor) {
    eg2_chk_skip("backdoor words (no bench)");
    return ok;
  }
  ok = eg2_chk_eq("backdoor words read back wrong", 0, t->bd_bad) && ok;
  ok = eg2_chk_eq("integrity errors on the backdoor words", 0, t->bd_errors) && ok;
  return ok;
}

/** Step 0: the extra renewals, each checked by the bench. */
static bool rekeys(uint32_t sram) {
  bool ok = true;
  for (uint32_t r = 0; r < EG2_PARAM(rekey); ++r) {
    LOG_INFO("EG2 MEM scr %s rekey", name_of(sram));
    ok = eg2_chk_true("SCR_KEY_VALID after an extra renewal",
                      ctrl_and_wait(sram, SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT,
                                    SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)) &&
         ok;
  }
  return ok;
}

/** Sec or ret SRAM, in place. */
static bool in_place(uint32_t sram, bool gaps) {
  scr_tally_t t = {.n = words_for(sram)};
  uint32_t base = sram == kSec ? kSecBase : kRetBase;
  uint32_t size = sram == kSec ? kSecSize : kRetSize;
  uint32_t frame_bytes = 4 * (t.n + kBdWords);
  uint32_t addr = base + 4 * eg2_rand_range(0, (size - frame_bytes) / 4);
  volatile uint32_t *f = eg2_chk_ptr(addr);
  bool ok = true;
  if (sram == kRet) {
    // The OpenTitan test's wipe; the retention SRAM's other contents are no longer needed.
    ok = eg2_chk_true("INIT_DONE", ctrl_and_wait(sram, SRAM_CTRL_CTRL_INIT_BIT,
                                                 SRAM_CTRL_STATUS_INIT_DONE_BIT)) &&
         ok;
  }
  ok = rekeys(sram) && ok;
  ok = prepare(f, t.n) && ok;
  LOG_INFO("EG2 MEM scr %s renew 0x%x", name_of(sram), addr + 4 * t.n);
  ok = eg2_chk_true("SCR_KEY_VALID", ctrl_and_wait(sram, SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT,
                                                   SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT)) &&
       ok;
  // The bench writes the backdoor words in zero time once the key is valid.
  eg2_chk_delay(2000);
  bool bench_backdoor = sram == kRet;
  if (!bench_backdoor) {
    for (uint32_t i = 0; i < kBdWords; ++i) {
      f[t.n + i] = backdoor(sram, i);
    }
  }
  for (uint32_t i = 0; i < t.n + kBdWords; ++i) {
    eg2_chk_trap_t tr;
    uint32_t c = eg2_chk_access(&f[i], kEg2ChkLw, 0, &tr);
    tally_word(&t, sram, i, tr.value, c != 0);
  }
  LOG_INFO("EG2 MEM scr %s done", name_of(sram));
  return check_tally(sram, &t, bench_backdoor, gaps) && ok;
}

/** Declares rv_core_ibex's fatal_hw_err if the integrity errors raised it. */
static void declare_hw_err(void) {
  int a = eg2_status_alert_find("rv_core_ibex_fatal_hw_err");
  if (a < 0) {
    return;
  }
  uint32_t cause = eg2_dev_read32(kEg2DevAlertHandler,
                                  ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4 * (uint32_t)a);
  eg2_chk_step("rv_core_ibex_fatal_hw_err cause", cause & 1);
  if (cause & 1) {
    eg2_status_expect_alert((size_t)a);
  }
}

static bool boot1(bool gaps) {
  volatile uint32_t *ref = eg2_chk_ptr(kRefAddr);
  ref[kRefMagic] = 0;
  bool ok = in_place(kSec, gaps);

  // Main SRAM: the frame in a stack buffer, at a drawn offset.
  volatile uint32_t buf[kMainBufWords];
  uint32_t n = words_for(kMain);
  uint32_t off = eg2_rand_range(0, kMainBufWords - (n + kBdWords));
  volatile uint32_t *f = &buf[off];
  ok = prepare(f, n) && ok;
  for (uint32_t i = kRefFrame; i < kRefFrame + n + kBdWords + 2; ++i) {
    ref[i] = 0;
  }
  ref[kRefWords] = n;
  ref[kRefFails] = ok ? 0 : 1;
  ref[kRefMagic] = kRefMagicValue;
  eg2_chk_summary(2);
  LOG_INFO("EG2 MEM scr main renew 0x%x", eg2_chk_addr_of(f) + 4 * n);
  eg2_scr_main_rekey(eg2_dev(kEg2DevSramCtrlMainRegs), f, &ref[kRefFrame], n + kBdWords,
                     eg2_dev(kEg2DevRstmgr));
}

static bool boot2(bool gaps) {
  volatile uint32_t *ref = eg2_chk_ptr(kRefAddr);
  bool ok = eg2_chk_eq("main SRAM frame copy present", kRefMagicValue, ref[kRefMagic]);
  ok = eg2_chk_eq("boot 1 failed checks", 0, ref[kRefFails]) && ok;
  scr_tally_t t = {.n = ref[kRefWords]};
  ok = eg2_chk_true("main SRAM frame size", t.n >= 16 && t.n <= kMainMaxWords) && ok;
  if (t.n < 16 || t.n > kMainMaxWords) {
    t.n = 16;
  }
  for (uint32_t i = 0; i < t.n + kBdWords; ++i) {
    tally_word(&t, kMain, i, ref[kRefFrame + i], false);
  }
  // The assembly counts every error of the copy, the backdoor words' included: all count against
  // the pattern words here, and a backdoor word with an error shows as a wrong value.
  uint32_t irqs = ref[kRefFrame + t.n + kBdWords];
  uint32_t traps = ref[kRefFrame + t.n + kBdWords + 1];
  t.errors = irqs + traps;
  eg2_chk_step("main copy: internal interrupts", irqs);
  eg2_chk_step("main copy: traps", traps);
  ok = check_tally(kMain, &t, true, gaps) && ok;
  ok = in_place(kRet, gaps) && ok;
  declare_hw_err();
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("MEM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  const bool gaps = EG2_PARAM(model_gaps) != 0;
  // The test ROM moves RESET_INFO to the retention SRAM (creator.reset_reasons, offset 4).
  uint32_t reason = eg2_chk_rd32(kRetBase + 4);
  eg2_chk_step("reset reason", reason);
  if (reason & kResetPor) {
    return boot1(gaps);
  }
  bool ok = eg2_chk_eq("reset reason: software", kResetSw, reason & kResetSw);
  ok = boot2(gaps) && ok;
  return eg2_chk_summary(10) && ok;
}
