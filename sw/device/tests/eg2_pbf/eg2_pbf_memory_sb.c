// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_memory_sb (EG2 verification spec REQ_PBF_19, "Post-boot features in CHERIoT
 * mode"): the SRAM and RRAM runtime operations after secure boot (or the test ROM), in RISC-V mode
 * and, in the `_cheriot` build, through capabilities in CHERIoT mode.
 *
 * Operations, each selected by a bit of `ops` (the spec's knob "operation"):
 *   bit 0  SRAM INIT on the secure SRAM (sram_ctrl_sec): a range written with a pattern, then
 *          CTRL.INIT: STATUS.INIT_DONE, and the range reads back without error and with none of
 *          its old values ("LFSR Initialization Feature": the memory is overwritten with
 *          pseudorandom data); then the range written and read back.
 *   bit 1  SRAM key renewal on the secure SRAM: SCR_KEY_ROTATED cleared (write MuBi4True: it reads
 *          MuBi4False), CTRL.RENEW_SCR_KEY with INIT in one write (the theory of operation: the
 *          initialisation waits for the new key), STATUS.SCR_KEY_VALID, SCR_KEY_ROTATED MuBi4True;
 *          a range written and read back under the new key.
 *   bit 2  RRAM program: a data region of the test's own over the OTTF non-volatile scratch area
 *          (scrambling as `scramble`, ECC on); the range first read through the host window (so
 *          the read buffer holds the old data), programmed through the controller, then read
 *          through the host window and through the controller: both give the new words (REQ_RRAM:
 *          the read buffer returns the new data after a controller write).
 *   bit 3  RRAM rewrite ("erase" in the spec's knob): the RRAM controller has no erase; its
 *          operations are Read, Write and Rewrite (programmer's guide). A Rewrite of each RRAM
 *          word of the range leaves its contents unchanged, read through both paths.
 * `range_class` sets the range: 0 one word (one RRAM word, 4 bus words), 1 up to 16 words,
 * 2 up to 256 words, 3 drawn per operation. Where the range starts is drawn from the seed.
 *
 * Expected values are the patterns the test wrote, from the seed, compared on a path other than
 * the one that wrote them (host window against controller, and the documented effect of INIT);
 * the chip bench's scoreboard checks every load against its model.
 *
 * Knobs: ops, range_class, scramble, model_gaps (1: eg2sim; see the gaps below).
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top/sram_ctrl_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define MEMORY_SB_PARAMS(X) \
  X(ops, 15, 1, 15)         \
  X(range_class, 3, 0, 3)   \
  X(scramble, 1, 0, 1)      \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(MEMORY_SB_PARAMS)

enum {
  kMaxWords = 256,
  kSecWords = TOP_EARLGREY_SRAM_CTRL_SEC_RAM_SIZE_BYTES / 4,
};

// The buffers live on the stack (eg2_test_main): CHERIoT images keep their globals small.
static uint32_t *buf;
static uint32_t *got;

/** The range's length in words, a multiple of `unit`. */
static uint32_t range_words(uint32_t unit) {
  uint32_t c = EG2_PARAM(range_class);
  if (c == 3) {
    c = eg2_rand_range(0, 2);
  }
  uint32_t n = c == 0 ? 1 : c == 1 ? eg2_rand_range(2, 16) : eg2_rand_range(17, kMaxWords);
  n = (n + unit - 1) / unit * unit;
  return n > kMaxWords ? kMaxWords : n;
}

static void fill(uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) {
    buf[i] = eg2_rand();
  }
}

/** Writes `buf` to the secure SRAM at word `at` and reads it back into `got`. */
static bool sec_write_read(const char *what, uint32_t at, uint32_t n) {
  fill(n);
  volatile uint32_t *p = eg2_pbf_word(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 4 * at);
  for (uint32_t i = 0; i < n; ++i) {
    p[i] = buf[i];
  }
  for (uint32_t i = 0; i < n; ++i) {
    got[i] = p[i];
  }
  return eg2_pbf_check_words(what, buf, got, n);
}

static bool sram_init(void) {
  uint32_t n = range_words(1), at = eg2_rand_range(0, kSecWords - n);
  eg2_pbf_note("SRAM INIT: secure SRAM words", n);
  // The secure SRAM holds nothing of the test; written first, so every word read has integrity.
  bool ok = sec_write_read("secure SRAM before INIT", at, n);
  ok = eg2_pbf_step("secure SRAM INIT done", eg2_pbf_sram_op(kEg2DevSramCtrlSecRegs, false, true)) &&
       ok;
  volatile uint32_t *p = eg2_pbf_word(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR + 4 * at);
  uint32_t same = 0;
  for (uint32_t i = 0; i < n; ++i) {
    same += p[i] == buf[i];
  }
  ok = eg2_pbf_check("words keeping their value through INIT", 0, same) && ok;
  ok = sec_write_read("secure SRAM after INIT", at, n) && ok;
  eg2_pbf_activity("sram INIT", 1);
  return ok;
}

static bool sram_key_renewal(void) {
  uint32_t n = range_words(1), at = eg2_rand_range(0, kSecWords - n);
  eg2_pbf_note("SRAM key renewal: secure SRAM words", n);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET, kMultiBitBool4True);
  bool ok = eg2_pbf_check("SCR_KEY_ROTATED after the clear", kMultiBitBool4False,
                          eg2_dev_read32(kEg2DevSramCtrlSecRegs,
                                         SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) & 0xf);
  ok = eg2_pbf_step("secure SRAM key renewed and initialised",
                    eg2_pbf_sram_op(kEg2DevSramCtrlSecRegs, true, true)) &&
       ok;
  ok = eg2_pbf_check("SCR_KEY_ROTATED after the renewal", kMultiBitBool4True,
                     eg2_dev_read32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_SCR_KEY_ROTATED_REG_OFFSET) &
                         0xf) &&
       ok;
  ok = sec_write_read("secure SRAM under the new key", at, n) && ok;
  eg2_pbf_activity("sram key renewal", 1);
  return ok;
}

/** The RRAM range: 16-byte aligned in the NV scratch area. */
static uint32_t rram_range(uint32_t n) {
  uint32_t size = eg2_pbf_nv_scratch_size();
  uint32_t slots = (size - 4 * n) / 16;
  return eg2_pbf_nv_scratch() + 16 * eg2_rand_range(0, slots - 1);
}

static bool host_read(uint32_t addr, uint32_t n) {
  volatile uint32_t *p = eg2_pbf_word(addr);
  for (uint32_t i = 0; i < n; ++i) {
    got[i] = p[i];
  }
  return true;
}

static bool rram_region_ok;

static bool rram_program(void) {
  uint32_t n = range_words(4), addr = rram_range(n);
  eg2_pbf_note("RRAM program: address", addr);
  eg2_pbf_note("RRAM program: words", n);
  host_read(addr, n);  // fills the read buffer with the old words
  fill(n);
  bool ok = eg2_pbf_step("RRAM program", eg2_pbf_rram_program(addr, buf, n));
  ok = eg2_pbf_check("RRAM ERR_CODE after the program", 0, eg2_pbf_rram_err_code()) && ok;
  host_read(addr, n);
  ok = eg2_pbf_check_words("RRAM host window after the program", buf, got, n) && ok;
  ok = eg2_pbf_step("RRAM controller read", eg2_pbf_rram_ctrl_read(addr, got, n)) && ok;
  ok = eg2_pbf_check_words("RRAM controller read after the program", buf, got, n) && ok;
  eg2_pbf_activity("rram program words", n);
  return ok;
}

static bool rram_rewrite(void) {
  uint32_t n = range_words(4), addr = rram_range(n);
  eg2_pbf_note("RRAM rewrite: address", addr);
  fill(n);
  bool ok = eg2_pbf_step("RRAM program before the rewrite", eg2_pbf_rram_program(addr, buf, n));
  uint32_t done = 0;
  for (uint32_t w = 0; w < n; w += 4) {
    done += eg2_pbf_rram_rewrite(addr + 4 * w);
  }
  ok = eg2_pbf_check("RRAM rewrites completed", n / 4, done) && ok;
  ok = eg2_pbf_check("RRAM ERR_CODE after the rewrites", 0, eg2_pbf_rram_err_code()) && ok;
  host_read(addr, n);
  ok = eg2_pbf_check_words("RRAM host window after the rewrite", buf, got, n) && ok;
  ok = eg2_pbf_step("RRAM controller read", eg2_pbf_rram_ctrl_read(addr, got, n)) && ok;
  ok = eg2_pbf_check_words("RRAM controller read after the rewrite", buf, got, n) && ok;
  eg2_pbf_activity("rram rewrites", n / 4);
  eg2_pbf_gap("RRAM erase", "the RRAM controller has no erase operation (programmer's guide: Read, "
                            "Write, Rewrite); the spec's erase runs as a Rewrite");
  return ok;
}

bool eg2_test_main(void) {
  uint32_t buf_words[kMaxWords], got_words[kMaxWords];
  buf = buf_words;
  got = got_words;
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("memory", EG2_PBF_INJECT);
  eg2_pbf_boot_stage();
  uint32_t ops = EG2_PARAM(ops);
  uint32_t regwen = eg2_dev_read32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_CTRL_REGWEN_REG_OFFSET) & 1;
  if ((ops & 3) && regwen == 0) {
    eg2_pbf_gap("secure SRAM INIT and key renewal",
                "sram_ctrl_sec CTRL_REGWEN was cleared by a boot stage");
    ops &= ~3u;
  }
  if (ops & 1) {
    ok = sram_init() && ok;
  }
  if (ops & 2) {
    ok = sram_key_renewal() && ok;
  }
  if (ops & 12) {
    rram_region_ok = eg2_pbf_rram_region(eg2_pbf_nv_scratch(),
                                         eg2_pbf_nv_scratch_size() / kEg2PbfRramPage,
                                         EG2_PARAM(scramble) != 0, true);
    ok = eg2_pbf_step("an RRAM data region over the scratch area", rram_region_ok) && ok;
  }
  if ((ops & 4) && rram_region_ok) {
    ok = rram_program() && ok;
  }
  if ((ops & 8) && rram_region_ok) {
    ok = rram_rewrite() && ok;
  }
  eg2_pbf_gap("backdoor compare of the RRAM rows",
              "the bench does not descramble RRAM rows; the words are compared on the host and the "
              "controller paths, and the scoreboard checks every load");
  return ok;
}
