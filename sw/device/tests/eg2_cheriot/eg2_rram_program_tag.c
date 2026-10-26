// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_rram_program_tag (EG2 spec "Memory subsystem in CHERIoT mode"; REQ_CMS_16).
 *
 * No write by a host other than the core's CSC leaves a tag set: an RRAM controller write over a
 * tagged granule leaves tag 0, or is unreachable in CHERIoT mode (RFC CHERIoT Memory Subsystem
 * "Lockstepped CHERIoT Subsystem": "any write by a non-CHERIoT host ... must clear").
 *
 * `n_caps` capabilities are made tagged in the test's RRAM area (programmed through rram_ctrl,
 * then a verified CSC), one per 16-byte RRAM word. Then rram_ctrl programs new data over
 * `range_class` of them: one granule (the RRAM word holding one capability: 16 bytes are the
 * controller's smallest write, so a part of a granule cannot be written alone), many granules (a
 * run of words), or the whole area. Expected, for each programmed capability: either the
 * controller refused the write (OP_STATUS.err) and the words are unchanged, or the write took and
 * the CLC returns tag 0; every capability outside the range keeps its tag and words.
 *
 * Expected to fail on the RTL of 2026-10-24: no RTL path clears a tag on an RRAM controller write
 * (dv_todo, mutation library: "Not expressible as a mutation: REQ_CMS_16").
 *
 * The SystemC model's controller does not touch the meta SRAM: with `model_gaps` the programmed
 * capabilities' tags are not checked (their words are).
 *
 * Built with EG2_RRAM_PROGRAM_TAG_INJECT_FAIL, the first capability is expected untagged after its
 * verified CSC: the run must fail with `MISMATCH tagged before #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define RRAM_PROGRAM_TAG_PARAMS(X) \
  X(n_caps, 0, 0, 64)              \
  X(range_class, 0, 0, 4)          \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(RRAM_PROGRAM_TAG_PARAMS);

enum {
  kWords = 64,  // capability slots (RRAM words) the test uses
  kExpLo = kEg2WorkBase,
  kExpHi = kEg2WorkBase + 4 * kWords,
};

static eg2_tally_t tally;

static uint32_t slot(uint32_t i) { return kEg2MsRramArea + 16 * i; }

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init() || !eg2_ms_rram_setup()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n = EG2_PARAM(n_caps) != 0 ? EG2_PARAM(n_caps) : eg2_rand_range(1, 64);
  uint32_t cls = EG2_PARAM(range_class);
  if (cls == 0) {
    // one granule 30, a partial granule 20 (one granule: see above), many 30, the whole area 20
    uint32_t d = eg2_rand_range(0, 99);
    cls = d < 50 ? 1 : d < 80 ? 3 : 4;
  }
  if (cls == 2) {
    cls = 1;
  }
  // The programmed range: words [lo, hi).
  uint32_t lo, hi;
  if (cls == 1) {
    lo = eg2_pick(n);
    hi = lo + 1;
  } else if (cls == 3) {
    lo = eg2_pick(n);
    hi = lo + eg2_rand_range(2, 16);
    hi = hi > n ? n : hi;
  } else {
    lo = 0;
    hi = n;
  }
  eg2_put_kv("EG2 rram_program_tag n_caps", n);
  eg2_put_kv(" range", lo);
  eg2_put_kv(" to", hi);
  eg2_puts("\r\n");

  for (uint32_t i = 0; i < n; ++i) {
    void *c = eg2_ms_object_cap(kEg2MsHeapBase + 8 * eg2_rand_range(0, 4000), 32, false);
    uint32_t w[4] = {eg2_ms_cap_addr(c), eg2_ms_cap_high(c), 0, 0};
    uint32_t err;
    if (!eg2_ms_rram_program(slot(i), w, 4, &err)) {
      eg2_ms_note("rram_ctrl program failed, ERR_CODE", err);
      return false;
    }
    eg2_ms_csc(slot(i), c);
    uint32_t before = 1;
#ifdef EG2_RRAM_PROGRAM_TAG_INJECT_FAIL
    before = i == 0 ? 0 : 1;
#endif
    eg2_check(&tally, "tagged before", i, eg2_ms_tag(slot(i)), before);
    eg2_ms_write32(kExpLo + 4 * i, w[0]);
    eg2_ms_write32(kExpHi + 4 * i, w[1]);
  }

  // The controller's write: new words over [lo, hi).
  bool refused = false;
  for (uint32_t i = lo; i < hi; ++i) {
    uint32_t w[4] = {eg2_rand(), eg2_rand(), eg2_rand(), eg2_rand()};
    uint32_t err;
    if (!eg2_ms_rram_program(slot(i), w, 4, &err)) {
      refused = true;
    } else {
      eg2_ms_write32(kExpLo + 4 * i, w[0]);
      eg2_ms_write32(kExpHi + 4 * i, w[1]);
    }
  }
  eg2_puts(refused ? "EG2 the controller refused the write\r\n"
                   : "EG2 the controller wrote the range\r\n");

  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    bool written = i >= lo && i < hi && !refused;
    eg2_check(&tally, "lo", i, eg2_ms_read32(slot(i)), eg2_ms_read32(kExpLo + 4 * i));
    eg2_check(&tally, "hi", i, eg2_ms_read32(slot(i) + 4), eg2_ms_read32(kExpHi + 4 * i));
    // The model keeps the tag of a programmed word, and MPACT-CHERIoT then faults on some CLCs of
    // the random words left tagged: with model_gaps a programmed slot is not reloaded.
    if (written) {
      if (!gaps) {
        eg2_check(&tally, "programmed tag", i, eg2_ms_tag(slot(i)), 0);
      }
    } else {
      eg2_check(&tally, "kept tag", i, eg2_ms_tag(slot(i)), 1);
    }
    ++tally.done;
  }
  eg2_tally_print("rram_program_tag", &tally);
  return eg2_tally_ok(&tally);
}
