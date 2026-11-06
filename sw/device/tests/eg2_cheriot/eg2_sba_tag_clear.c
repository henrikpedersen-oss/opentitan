// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_cheriot_sba_tag_clear (EG2 spec "Debug module in CHERIoT mode"; REQ_CDM_06).
 *
 * In a debug-enabled state an SBA write over a tagged granule leaves its tag unchanged: the debug
 * module's system bus access bypasses cheriot_mem_sys's tag filter (rv_dm theory of operation,
 * "CHERIoT Mode": "SBA bypasses the CHERIoT tag filter and does not clear tags on writes";
 * earlgrey-internal-tracker #1040, closed as documented). REQ_CMS_16 leaves SBA out for this
 * reason; REQ_CDM_05 keeps SBA away outside the debug-enabled states. The test name is the old one.
 *
 * `n_caps` capabilities in sec SRAM's arena B (`range_class`: one granule, a partial granule (one
 * word of a granule; the bench may write narrower), many granules, the whole arena), as many in
 * arena A outside the range. A bench step (`sba_fill <addr> <bytes>`) has the chip bench write
 * the range over JTAG and SBA, at a width it draws (8, 16 or 32 bits), with data of its own; it
 * answers with sberror (0 expected). Then every capability is reloaded: tag 1 everywhere, in a
 * granule the bench wrote any byte of as well; outside it, also the words as stored.
 *
 * Without a bench (the SystemC model) the step is skipped, and so are the written granules' checks
 * (`model_gaps`).
 *
 * Built with EG2_SBA_TAG_CLEAR_INJECT_FAIL, the first capability outside the range is expected
 * untagged: the run must fail with `MISMATCH outside tag`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define SBA_TAG_CLEAR_PARAMS(X) \
  X(n_caps, 0, 0, 64)           \
  X(range_class, 0, 0, 4)       \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(SBA_TAG_CLEAR_PARAMS);

static eg2_tally_t tally;

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool bench = EG2_PARAM(model_gaps) == 0;
  uint32_t n = EG2_PARAM(n_caps) != 0 ? EG2_PARAM(n_caps) : eg2_rand_range(1, 64);
  uint32_t cls = EG2_PARAM(range_class);
  if (cls == 0) {
    uint32_t d = eg2_rand_range(0, 99);
    cls = d < 30 ? 1 : d < 50 ? 2 : d < 80 ? 3 : 4;
  }
  // The capabilities: granules 0..n-1 of arena B, and the same number in arena A.
  for (uint32_t i = 0; i < n; ++i) {
    eg2_ms_csc(kEg2ArenaB + 8 * i, eg2_ms_object_cap(kEg2ArenaA + 8 * i, 8, false));
    eg2_ms_csc(kEg2ArenaA + 8 * i, eg2_ms_object_cap(kEg2ArenaB + 8 * i, 8, false));
  }
  // The written range [lo, hi) in bytes.
  uint32_t g = eg2_pick(n);
  uint32_t lo, hi;
  switch (cls) {
    case 1:
      lo = kEg2ArenaB + 8 * g;
      hi = lo + 8;
      break;
    case 2:
      lo = kEg2ArenaB + 8 * g + 4 * eg2_pick(2);
      hi = lo + 4;
      break;
    case 3:
      lo = kEg2ArenaB + 8 * g;
      hi = lo + 8 * eg2_rand_range(2, n - g > 2 ? n - g : 2);
      break;
    default:
      lo = kEg2ArenaB;
      hi = kEg2ArenaB + kEg2ArenaSize;
      break;
  }
  if (!eg2_ms_bench("sba_fill", lo, hi - lo, bench)) {
    return false;
  }
  if (bench) {
    eg2_check(&tally, "sberror", 0, eg2_ms_bench_result(), 0);
  }
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    uint32_t a = kEg2ArenaB + 8 * i;
    if (a + 8 > lo && a < hi) {
      if (bench) {
        eg2_check(&tally, "written tag (unchanged, REQ_CDM_06)", i, eg2_ms_tag(a), 1);
      }
    } else {
      eg2_check(&tally, "outside tag (arena B)", i, eg2_ms_tag(a), 1);
    }
    uint32_t exp = 1;
#ifdef EG2_SBA_TAG_CLEAR_INJECT_FAIL
    if (i == 0) {
      eg2_inject_armed("outside tag");
    }
    exp = i == 0 ? 0 : 1;
#endif
    void *c = eg2_ms_clc(kEg2ArenaA + 8 * i);
    eg2_check(&tally, "outside tag", i, eg2_ms_cap_tag(c), exp);
    eg2_check(&tally, "outside base", i, eg2_ms_cap_base(c), kEg2ArenaB + 8 * i);
    ++tally.done;
  }
  eg2_tally_print("sba_tag_clear", &tally);
  return eg2_tally_ok(&tally);
}
