// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_scramble (EG2 verification spec REQ_RRAM_07, REQ_SEC_18): XEX
 * scrambling with the keys sideloaded from otp_ctrl. Both paths, the host
 * window and the controller, read the plaintext written; the macro holds
 * ciphertext (the vseq's check, below); a new key turns old scrambled data into
 * garbage, while unscrambled data is unaffected.
 *
 * Documents: RFC RRAM §4.4 p10 and §4.7 p18; rram_ctrl theory_of_operation.md
 * "RRAM Scrambling" (ciphertext = PRINCE(tweak ^ plaintext, data_key) ^ tweak,
 * tweak = GF_MULT(word_addr, addr_key)), "Address-XOR" (each bus word XORed
 * with its bus-word address before scrambling), programmers_guide.md
 * "Scrambling Consistency" (the attribute of a page decides; writing and
 * reading with the same attribute gives the data back).
 *
 * Phase 0 (boot_phase = 0): a region over n_words at the drawn location with
 * RD, WR, ECC and, with `scramble`, SCR; a controller write of the seed's
 * pattern (eg2_rram_pattern, salt kSalt); a controller read and a host read of
 * every word, each compared with the pattern. Then `EG2 RRAM scramble off=0x<o>
 * n=<n> scr=<s> salt=0x<salt>`: the vseq reads the macro rows by backdoor and
 * checks each against rram_ctrl_bkdr_util's XEX model of the plaintext (the
 *   pattern it recomputes from kEg2Seed), with the address and data keys taken
 * from the otp_ctrl -> rram_ctrl key interface; without `scramble`, the rows
 * hold plaintext with only the address XOR. With key_change, the software then
 * prints `EG2 RRAM scramble rekey` and waits: the vseq writes new NVM key seeds
 * into OTP's SECRET1 (otp_write_secret1_partition), sets boot_phase = 1 and
 * resets the chip. Phase 1: the same region, a host and a controller read of
 * every word: with `scramble` no RRAM word (four bus words) reads back as the
 * pattern (old data is garbage under the new key, with no ECC error: the ECC
 * covers the ciphertext); without it every word reads back as the pattern. Then
 * a write and read-back of a new pattern under the new key.
 *
 * Knobs:
 *   n_words     bus words written (a multiple of 4: whole RRAM words), 4-256
 *   loc_class   0 the first words of a page, 1 the last words of a page, 2
 * random (RRAM-word aligned), 3 drawn from the seed scramble    0 the region
 * unscrambled, 1 scrambled key_change  1: the second boot with new keys (chip
 * only: the model has no key interface) boot_phase  written by the vseq before
 * the reset (0 in the image) model_gaps  skips what the SystemC model does not
 * model:
 *
 * | Skipped with model_gaps = 1        | Why |
 * |------------------------------------|-------------------------------------------------------|
 * | (nothing in phase 0; key_change is | eg2sim's rram_ctrl does not scramble
 * (ip/eg2_mem.h):  | |   0 in the model)                  |   both paths read
 * plaintext there too, and there is   | |                                    |
 * no macro or key interface to compare               |
 *
 * Built with EG2_RRAM_SCRAMBLE_INJECT_FAIL the first value check expects bit 0
 * flipped: the run must fail with `RRAM FAIL`.
 *
 * EG2_NEEDS_BENCH: the bench makes this test's stimulus, so regr_vlt.sh leaves it out of the Verilator runs.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define SCRAMBLE_PARAMS(X) \
  X(n_words, 32, 4, 256)   \
  X(loc_class, 3, 0, 3)    \
  X(scramble, 1, 0, 1)     \
  X(key_change, 0, 0, 1)   \
  X(boot_phase, 0, 0, 1)   \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(SCRAMBLE_PARAMS)

enum {
  kSalt = 0x5c4a3b1e,
  kSalt2 = 0x0badcafe,
  kPolls = 200000,
  kRekeyWait = 4000000,
};

/**
 * The region's byte offset: drawn from the seed only, so the second boot (same
 * seed) finds the same place. eg2_rand() is the seed's sequence from the start
 * of the run in both boots.
 */
static uint32_t place(uint32_t n) {
  uint32_t loc = EG2_PARAM(loc_class);
  if (loc > 2) {
    loc = eg2_rand_range(0, 2);
  }
  uint32_t page =
      eg2_rand_range(kEg2RramScratchFirstPage, kEg2RramScratchLastPage - 1);
  uint32_t page_words = kEg2RramPageBytes / 4;
  uint32_t first;
  switch (loc) {
    case 0:
      first = 0;
      break;
    case 1:
      first = n <= page_words ? page_words - n : 0;
      break;
    default:
      first = 4 * eg2_rand_range(0, (page_words - 4) / 4);
      break;
  }
  return eg2_rram_addr(page, first);
}

enum { kChunk = 16 };

/** Writes the pattern `salt` over n words at `off`, generated as it is pushed.
 */
static bool write_pattern(uint32_t off, uint32_t n, uint32_t salt) {
  eg2_rram_result_t r;
  r.words = 0;
  eg2_rram_start(kEg2RramOpWrite, kEg2RramPartData, off, n);
  for (uint32_t i = 0; i < n; ++i) {
    if (!eg2_rram_push(eg2_rram_pattern(salt, i), kPolls)) {
      break;
    }
    ++r.words;
  }
  bool ok =
      eg2_chk_true("controller write done", eg2_rram_wait_done(&r, kPolls));
  ok = eg2_chk_eq("controller write: words pushed", n, r.words) && ok;
  return eg2_chk_eq("controller write error code", 0, r.err_code) && ok;
}

/** Reads n words both ways and counts the words (and RRAM words) equal to the
 * pattern. */
static bool read_both(uint32_t off, uint32_t n, uint32_t salt,
                      uint32_t *host_eq, uint32_t *ctrl_eq,
                      uint32_t *rword_eq) {
  bool ok = true;
  *host_eq = *ctrl_eq = *rword_eq = 0;
  uint32_t buf[kChunk];
  for (uint32_t i = 0; i < n; i += 4) {
    if (i % kChunk == 0) {
      eg2_rram_result_t r;
      uint32_t m = n - i < kChunk ? n - i : kChunk;
      ok = eg2_chk_true(
               "controller read done",
               eg2_rram_read(kEg2RramPartData, off + 4 * i, buf, m, &r)) &&
           ok;
      ok = eg2_chk_eq("controller read error code", 0, r.err_code) && ok;
    }
    uint32_t same = 0;
    for (uint32_t j = i; j < i + 4; ++j) {
      uint32_t exp = eg2_rram_pattern(salt, j);
      if (*eg2_rram_host(off + 4 * j) == exp) {
        ++*host_eq;
      }
      if (buf[j % kChunk] == exp) {
        ++*ctrl_eq;
        ++same;
      }
    }
    if (same == 4) {
      ++*rword_eq;
    }
  }
  return ok;
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  bool ok = eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls));
  uint32_t n = EG2_PARAM(n_words) & ~3u;
  uint32_t off = place(n);
  uint32_t attrs = kEg2RramAttrRd | kEg2RramAttrWr | kEg2RramAttrEcc |
                   (EG2_PARAM(scramble) ? kEg2RramAttrScr : 0);
  uint32_t region = eg2_rram_free_region();
  if (!eg2_chk_true("a free data region", region < kEg2RramRegions)) {
    return false;
  }
  uint32_t first_page = off / kEg2RramPageBytes;
  uint32_t pages = (off + 4 * n - 1) / kEg2RramPageBytes - first_page + 1;
  eg2_rram_region_set(region, first_page, pages, attrs);
  eg2_chk_step("region offset", off);
  eg2_chk_step("words", n);
  eg2_chk_step("scramble", EG2_PARAM(scramble));
  eg2_chk_step("boot phase", EG2_PARAM(boot_phase));

  uint32_t host_eq, ctrl_eq, rword_eq;
  if (EG2_PARAM(boot_phase) == 0) {
    ok = write_pattern(off, n, kSalt) && ok;
    ok = read_both(off, n, kSalt, &host_eq, &ctrl_eq, &rword_eq) && ok;
    ok = eg2_chk_eq("controller reads the plaintext (words)", n, ctrl_eq) && ok;
    ok = eg2_chk_eq("host reads the plaintext (words)", n, host_eq) && ok;
    // The bench compares the macro's rows with its XEX model here, in zero
    // time.
    LOG_INFO("EG2 RRAM scramble off=0x%x n=%d scr=%d salt=0x%x", off, n,
             (int)EG2_PARAM(scramble), (uint32_t)kSalt);
    if (EG2_PARAM(key_change)) {
      EG2_CHK_SYNC("EG2 RRAM scramble rekey");
      eg2_chk_delay(kRekeyWait);
      // The bench resets the chip before this point.
      return eg2_chk_true("the bench reset the chip for the new key", false);
    }
    eg2_rram_region_off(region);
    return eg2_chk_summary(5) && ok;
  }

  // Phase 1: the keys changed.
  ok = read_both(off, n, kSalt, &host_eq, &ctrl_eq, &rword_eq) && ok;
  if (EG2_PARAM(scramble)) {
    ok = eg2_chk_eq("RRAM words still plaintext under the new key (controller)",
                    0, rword_eq) &&
         ok;
    ok =
        eg2_chk_true("host reads garbage under the new key", host_eq < n / 2) &&
        ok;
  } else {
    ok = eg2_chk_eq("unscrambled words unaffected by the key (controller)", n,
                    ctrl_eq) &&
         ok;
    ok = eg2_chk_eq("unscrambled words unaffected by the key (host)", n,
                    host_eq) &&
         ok;
  }
  ok = eg2_chk_eq("no correctable error interrupt", 0,
                  eg2_rram_reg(RRAM_CTRL_INTR_STATE_REG_OFFSET) &
                      ((1u << RRAM_CTRL_INTR_STATE_CORR1_ERR_BIT) |
                       (1u << RRAM_CTRL_INTR_STATE_CORR2_ERR_BIT))) &&
       ok;
  ok = eg2_chk_eq("no fault", 0,
                  eg2_rram_reg(RRAM_CTRL_FAULT_STATUS_REG_OFFSET)) &&
       ok;
  // The new key works: a new pattern round trip.
  ok = write_pattern(off, n, kSalt2) && ok;
  ok = read_both(off, n, kSalt2, &host_eq, &ctrl_eq, &rword_eq) && ok;
  ok = eg2_chk_eq("controller reads the new plaintext", n, ctrl_eq) && ok;
  ok = eg2_chk_eq("host reads the new plaintext", n, host_eq) && ok;
  LOG_INFO("EG2 RRAM scramble off=0x%x n=%d scr=%d salt=0x%x", off, n,
           (int)EG2_PARAM(scramble), (uint32_t)kSalt2);
  eg2_rram_region_off(region);
  return eg2_chk_summary(8) && ok;
}
