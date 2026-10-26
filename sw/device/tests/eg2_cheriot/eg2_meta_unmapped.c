// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_meta_unmapped[_cheriot] (EG2 spec "Memory subsystem in RISC-V mode" and "Twins
 * of the RISC-V tests"; REQ_CMS_09).
 *
 * sram_ctrl_meta.ram is reachable only from the subsystem; no host can address the tag regions
 * (RFC CHERIoT in OpenTitan p2, p4 "In ePMP mode, this memory will not be accessible"; RFC CHERIoT
 * Memory Subsystem Fig. 1; xbar_main.hjson; cheriot_mem_sys theory_of_operation.md: the revbm
 * window is the software-visible view of the bitmap, "Accesses must be full 32-bit words, and the
 * window returns a TL-UL error while the system is in ePMP mode").
 *
 * Accesses over the meta SRAM's addresses [0x1100_0000, 0x1100_9800): `path` core load, core
 * store, SBA read, SBA write (a bench step: the bench reads the word first and writes back what
 * it read, and answers with sberror); `target` the first word, the last word, the tag region, the
 * bitmap region, anywhere; `width` word, half, byte. Directed first: every target class at every
 * path and width once; then `n_accesses` drawn.
 *
 * Expected:
 *  - RISC-V mode: every access is answered with an error: a load or store access fault with
 *    `mtval` the address, or sberror != 0.
 *  - CHERIoT mode, with a capability that covers the addresses: the tag regions (offset 0xC00 on,
 *    the last word included) error as above; the bitmap region (the first word included) answers
 *    full-word accesses (a store writes back the word it read) and errors on half and byte.
 *  - CHERIoT mode, the meta SRAM unchanged: the tags of capabilities stored beforehand in sec SRAM,
 *    and a pattern in the bitmap over granules no capability uses, read back as they were.
 *    (RISC-V mode cannot read the meta SRAM: the bench's backdoor compare is its checker.)
 *
 * The SystemC model answers the meta SRAM's addresses and the bitmap window in either mode
 * (dv_todo G2): with `model_gaps` the expected errors are not checked, the stores that must error
 * are not made (the model would write the meta SRAM), and SBA steps are skipped.
 *
 * Built with EG2_META_UNMAPPED_INJECT_FAIL, one more access is planned than made: the run must fail
 * with `EG2 meta_unmapped: <n>/<n+1> done`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define META_UNMAPPED_PARAMS(X) \
  X(n_accesses, 0, 0, 256)      \
  X(w_target_first, 15, 1, 100) \
  X(w_target_last, 15, 1, 100)  \
  X(w_target_tags, 25, 1, 100)  \
  X(w_target_bitmap, 25, 1, 100) \
  X(w_target_any, 20, 1, 100)   \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(META_UNMAPPED_PARAMS);

enum {
  kPathLoad = 0,
  kPathStore,
  kPathSbaRead,
  kPathSbaWrite,
  kPathCount,
  kTargetCount = 5,
  kPatternCaps = 16,
};

static eg2_tally_t tally;

static uint32_t target_addr(uint32_t t) {
  switch (t) {
    case 0:
      return kEg2MsMetaBase;
    case 1:
      return kEg2MsMetaBase + kEg2MsMetaSize - 4;
    case 2:
      return kEg2MsMetaBase + kEg2MsRevbmSize + 4 * eg2_pick((kEg2MsMetaSize - kEg2MsRevbmSize) / 4);
    case 3:
      return kEg2MsMetaBase + 4 * eg2_pick(kEg2MsRevbmSize / 4);
    default:
      return kEg2MsMetaBase + 4 * eg2_pick(kEg2MsMetaSize / 4);
  }
}

/** One access; checks its outcome. */
static void access(uint32_t path, uint32_t addr, uint32_t width, uint32_t idx) {
  ++tally.planned;
  bool gaps = EG2_PARAM(model_gaps) != 0;
  // A narrower access at any lane of the word.
  addr += width == 4 ? 0 : width * eg2_pick(4 / width);
#ifdef __CHERIOT__
  bool bitmap = addr < kEg2MsMetaBase + kEg2MsRevbmSize;
  bool ok = bitmap && width == 4;
#else
  bool ok = false;
#endif
  if (path == kPathSbaRead || path == kPathSbaWrite) {
    if (eg2_ms_bench(path == kPathSbaRead ? "sba_read" : "sba_rmw", addr, 8 * width, !gaps)) {
      if (!gaps) {
        eg2_check(&tally, "sberror != 0", idx, eg2_ms_bench_result() != 0 ? 1 : 0, ok ? 0 : 1);
      }
    }
    ++tally.done;
    return;
  }
  if (gaps && !ok && path == kPathStore) {
    // The model takes the store (dv_todo G2) and would change the meta SRAM the test checks.
    ++tally.done;
    return;
  }
  void *a = eg2_mem(addr, width);
  void *stub;
  uint32_t value = 0;
  if (path == kPathLoad) {
    stub = width == 4 ? eg2_stub_lw : width == 2 ? eg2_stub_lhu : eg2_stub_lbu;
  } else {
    stub = width == 4 ? eg2_stub_sw : width == 2 ? eg2_stub_sh : eg2_stub_sb;
    if (ok) {
      value = eg2_ms_read32(addr);  // the bitmap word written back unchanged
    }
  }
  bool trapped = eg2_probe_call(a, (void *)(uintptr_t)value, NULL, stub);
  if (ok || !gaps) {
    uint32_t exp_cause = ok ? 0
                         : path == kPathLoad ? (uint32_t)kEg2McauseLoadFault
                                             : (uint32_t)kEg2McauseStoreFault;
    if (eg2_check(&tally, path == kPathLoad ? "load mcause" : "store mcause", idx,
                  trapped ? eg2_trap_rec.mcause : 0, exp_cause) &&
        trapped) {
      eg2_check(&tally, "mtval", idx, eg2_trap_rec.mtval, addr);
    }
  }
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  uint32_t n = EG2_PARAM(n_accesses) != 0 ? EG2_PARAM(n_accesses) : eg2_rand_range(32, 128);
  eg2_family_t targets = {.n = kTargetCount, .plain_mask = 0, .boosted = kTargetCount};
  targets.w[0] = EG2_PARAM(w_target_first);
  targets.w[1] = EG2_PARAM(w_target_last);
  targets.w[2] = EG2_PARAM(w_target_tags);
  targets.w[3] = EG2_PARAM(w_target_bitmap);
  targets.w[4] = EG2_PARAM(w_target_any);
  eg2_family_print("target", &targets);

#ifdef __CHERIOT__
  // What must not change: tagged capabilities in sec SRAM, a pattern in the bitmap over arena A
  // (no capability is based there).
  eg2_ms_revbm_clear();
  for (uint32_t i = 0; i < kPatternCaps; ++i) {
    eg2_ms_csc(kEg2ArenaB + 8 * i, eg2_ms_object_cap(kEg2ArenaB + 0x100 + 8 * i, 8, false));
  }
  uint32_t w0, b0;
  (void)eg2_ms_revbm_bit(kEg2ArenaA, &w0, &b0);
  for (uint32_t i = 0; i < kEg2ArenaSize / 256; ++i) {
    eg2_ms_revbm_write(w0 + i, 0xa5a50000u ^ i);
  }
#endif

  eg2_probe_begin();
  uint32_t idx = 0;
  static const uint8_t kWidths[3] = {4, 2, 1};
  for (uint32_t t = 0; t < kTargetCount; ++t) {
    for (uint32_t p = 0; p < kPathCount; ++p) {
      for (uint32_t w = 0; w < 3; ++w) {
        access(p, target_addr(t), kWidths[w], idx++);
      }
    }
  }
  for (uint32_t i = 0; i < n; ++i) {
    access(eg2_pick(kPathCount), target_addr(eg2_family_pick(&targets)), kWidths[eg2_pick(3)],
           idx++);
  }
  eg2_probe_end();

#ifdef __CHERIOT__
  for (uint32_t i = 0; i < kPatternCaps; ++i) {
    eg2_check(&tally, "tag kept", i, eg2_ms_tag(kEg2ArenaB + 8 * i), 1);
  }
  for (uint32_t i = 0; i < kEg2ArenaSize / 256; ++i) {
    eg2_check(&tally, "bitmap kept", i, eg2_ms_revbm_read(w0 + i), 0xa5a50000u ^ i);
  }
  eg2_ms_revbm_clear();
#endif
#ifdef EG2_META_UNMAPPED_INJECT_FAIL
  ++tally.planned;
#endif
  eg2_tally_print("meta_unmapped", &tally);
  return eg2_tally_ok(&tally);
}
