// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_revbm_write_order (EG2 spec "Revocation", "Corner cases"; REQ_CMS_11).
 *
 * A bitmap write through the software window (cheriot_mem_sys's revbm port) and a CLC whose
 * bitmap read takes the core's own port (corerevbm) race in the memory subsystem. Each case, at
 * the bitmap's first word (granules from 0x1000_0000), its last (to 0x1002_FFF8) and `n_random`
 * words of sec SRAM's arena A, at bit selects 0, 31 and a random one:
 *  - word write: `sw` of the mark and a CLC of a capability based in that granule as the very next
 *    instruction, then `sw` of the clear and a CLC as the next one (eg2_revoke.S, one stub, so no
 *    other code runs while the mark is set): tag 0 after the mark, the tag back after the clear;
 *  - halfword and byte writes of the same mark: the revbm window takes full 32-bit words only
 *    (cheriot_mem_sys theory_of_operation.md "Meta SRAM Address Map": "Accesses must be full
 *    32-bit words"; "Access Checkers": anything else is answered with a TL-UL error), so the write
 *    is a store access fault (mcause 7), the bitmap word is unchanged (reads 0), and a CLC after it
 *    returns the tag. The spec row expects the CLC to see byte and half writes; the test follows the
 *    IP documentation (reported as a spec discrepancy);
 *  - an SBA write of the mark (bench step sba_wr32, the debug module's port reaches
 *    cheriot_mem_sys.revbm), then a core CLC: tag 0; an SBA write of 0, a CLC: tag 1. Not at the
 *    first word, whose granules hold the image's globals and the C code runs between the SBA write
 *    and the CLC.
 *
 * `model_gaps` (eg2sim): no load filter, the revbm window takes partial writes, no bench: the tags
 * after a mark, the partial writes' faults and the SBA cases are skipped.
 *
 * Built with EG2_REVBM_WRITE_ORDER_INJECT_FAIL, the first case's tag after the clear is expected 0:
 * the run must fail with `MISMATCH tag after the clear #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define REVBM_WRITE_ORDER_PARAMS(X) \
  X(n_random, 0, 0, 32)             \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(REVBM_WRITE_ORDER_PARAMS);

extern char eg2_rv_stub_mark_sw[], eg2_rv_stub_mark_sh[], eg2_rv_stub_mark_sb[];
extern uint32_t eg2_rv_clc_tag(void *slot);

enum {
  kLastWord = kEg2MsRevbmSize / 4 - 1,
  kSlot = kEg2ArenaB,
  kMcauseStoreFault = 7,
};

static eg2_tally_t tally;
static bool gaps, bench;
static uint32_t idx;

static void *bm_word_cap(uint32_t w) { return eg2_mem(kEg2MsMetaBase + 4 * w, 4); }

/** One case: the granule of bit `b` of word `w`. */
static void one_case(uint32_t w, uint32_t b, bool sba) {
  uint32_t g = kEg2MsHeapBase + (w * 32 + b) * 8;
  // The capability, stored before anything is marked.
  eg2_ms_csc(kSlot, eg2_mem(g + eg2_pick(8), 8));
  void *slot = eg2_mem(kSlot, 8);
  uint32_t i = idx++;
  ++tally.planned;

  // The word write and the CLC, then the clear and the CLC.
  bool trapped = eg2_probe_call(bm_word_cap(w), (void *)(uintptr_t)(1u << b), slot,
                                eg2_rv_stub_mark_sw);
  eg2_check(&tally, "word write trapped", i, trapped, 0);
  if (!gaps) {
    eg2_check(&tally, "tag after the mark", i, eg2_trap_rec.value2, 0);
  }
  uint32_t exp_clear = 1;
#ifdef EG2_REVBM_WRITE_ORDER_INJECT_FAIL
  if (i == 0) {
    exp_clear = 0;
  }
#endif
  eg2_check(&tally, "tag after the clear", i, eg2_trap_rec.value, exp_clear);
  eg2_check(&tally, "bitmap word after the clear", i, eg2_ms_revbm_read(w), 0);

  // Halfword and byte writes: refused.
  for (uint32_t width = 2; width >= 1; --width) {
    uint32_t byte = width == 2 ? (b / 16) * 2 : b / 8;
    uint32_t v = 1u << (b % (8 * width));
    void *a = eg2_mem(kEg2MsMetaBase + 4 * w + byte, width);
    trapped = eg2_probe_call(a, (void *)(uintptr_t)v, slot,
                             width == 2 ? eg2_rv_stub_mark_sh : eg2_rv_stub_mark_sb);
    if (gaps) {
      eg2_ms_revbm_write(w, 0);
    } else {
      eg2_check(&tally, width == 2 ? "halfword write trapped" : "byte write trapped", i, trapped,
                1);
      if (trapped) {
        eg2_check(&tally, "partial write mcause", i, eg2_trap_rec.mcause, kMcauseStoreFault);
      }
      eg2_check(&tally, "bitmap word after a partial write", i, eg2_ms_revbm_read(w), 0);
    }
    eg2_check(&tally, "tag after a partial write", i, eg2_rv_clc_tag(slot), 1);
  }

  // SBA.
  if (sba && bench) {
    uint32_t addr = kEg2MsMetaBase + 4 * w;
    if (eg2_ms_bench("sba_wr32", addr, 1u << b, true)) {
      eg2_check(&tally, "SBA write sberror", i, eg2_ms_bench_result(), 0);
    }
    uint32_t t = eg2_rv_clc_tag(slot);
    (void)eg2_ms_bench("sba_wr32", addr, 0, true);
    eg2_check(&tally, "tag after the SBA mark", i, t, 0);
    eg2_check(&tally, "tag after the SBA clear", i, eg2_rv_clc_tag(slot), 1);
  }
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  bench = !gaps;
  uint32_t n_random = EG2_PARAM(n_random) != 0 ? EG2_PARAM(n_random) : eg2_rand_range(4, 32);
  eg2_put_kv("EG2 revbm_write_order n_random", n_random);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_probe_begin();
  // The first and the last word, bit selects 0, 31 and a random one.
  uint32_t r = eg2_rand_range(1, 30);
  static const uint32_t kBits[3] = {0, 31, 0};
  for (uint32_t k = 0; k < 3; ++k) {
    uint32_t b = k == 2 ? r : kBits[k];
    one_case(0, b, false);
    one_case(kLastWord, b, true);
  }
  // Words of arena A.
  uint32_t first = (kEg2ArenaA - kEg2MsHeapBase) / 256;
  for (uint32_t k = 0; k < n_random; ++k) {
    one_case(first + eg2_pick(kEg2ArenaSize / 256), eg2_pick(32), true);
  }
  eg2_probe_end();
  eg2_tally_print("revbm_write_order", &tally);
  return eg2_tally_ok(&tally);
}
