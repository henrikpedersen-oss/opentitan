// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_meta_zeroed_sb (EG2 verification spec REQ_PBF_14, REQ_CMS_15, "Post-boot
 * features in CHERIoT mode"): booted behind secure boot (or the test ROM), the meta SRAM is all
 * zero before the test's first capability store: every revocation bitmap bit, every capability tag
 * of the main SRAM the test has not used, and the tags of the NVM pages the run samples.
 *
 * CHERIoT build:
 *   1. The revocation bitmap through the `revbm` window, every word, in `read_order`: all zero
 *      (cheriot_mem_sys theory of operation "Meta SRAM Address Map"; nothing has freed memory).
 *   2. The tag of every 8-byte granule of the main SRAM between the globals and the stack, by CLC
 *      and CGetTag, in `read_order`: all zero. The globals and the stack are left out: the start-up
 *      code's own capability stores (`__cap_relocs`, the stack) precede the test.
 *   3. The tags of `nvm_pages` NVM pages (the image's first page and pages drawn from the seed):
 *      all zero (no boot stage stores a capability into the NVM).
 *   4. The test's first CSC, to granule `first_granule` of that main SRAM area: its tag reads 1 and
 *      the tags of the granules either side still read 0 (CHERIoT ISA: CSC of a tagged capability
 *      with SD and MC stores the tag; one tag per granule, REQ_CMS).
 * RISC-V build (ePMP mode): the tags are not readable, and the `revbm` window answers every access
 *   with a TL-UL error ("the window returns a TL-UL error while the system is in ePMP mode"): each
 *   bitmap word read is a load access fault (mcause 5).
 *
 * Knobs: read_order (0 ascending, 1 descending, 2 a permutation from the seed), first_granule,
 * nvm_pages, model_gaps.
 *
 * Who zeroes the meta SRAM: today the framework's CHERIoT start-up code (`sram_ctrl_meta`
 * CTRL.INIT, SecZeroInit), because neither the ROM nor ROM_EXT does (doc/eg2_test_framework.md
 * step 1). The test checks the outcome REQ_CMS_15 requires, whoever produced it.
 *
 * Built with EG2_PBF_INJECT_FAIL, the first check expects a wrong value: `PBF FAIL`.
 */

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define META_PARAMS(X)          \
  X(read_order, 2, 0, 2)        \
  X(first_granule, 0, 0, 12287) \
  X(nvm_pages, 4, 0, 64)        \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(META_PARAMS)

enum {
  kRevbmWords = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_SIZE_BYTES / 4,
  kMainGranules = kEg2PbfMainFreeSize / 8,
  kNvmPageGranules = kEg2PbfRramPage / 8,
};

/** Index `i` of `n` in the run's order: ascending, descending or a seeded permutation. */
static uint32_t order(uint32_t i, uint32_t n, uint32_t start, uint32_t stride) {
  switch (EG2_PARAM(read_order)) {
    case 0:
      return i;
    case 1:
      return n - 1 - i;
    default:
      return (i * stride + start) % n;  // n <= 12800: no 32-bit overflow
  }
}

static uint32_t gcd(uint32_t a, uint32_t b) {
  while (b != 0) {
    uint32_t t = a % b;
    a = b;
    b = t;
  }
  return a;
}

/** A stride coprime to n, from the seed, so `order` visits every index once. */
static uint32_t stride_for(uint32_t n) {
  uint32_t s;
  do {
    s = eg2_rand_range(1, n - 1);
  } while (gcd(s, n) != 1);
  return s;
}

#ifdef __CHERIOT__

/** The tag of the capability-sized granule at `addr`, through `window` (a root-derived cap). */
static uint32_t tag_at(void *window, uint32_t addr) {
  void *volatile *slot = (void *volatile *)__builtin_cheri_address_set(window, addr);
  void *c = *slot;
  return __builtin_cheri_tag_get(c) ? 1 : 0;
}

static bool cheriot_body(void) {
  bool ok = true;
  // 1. The revocation bitmap.
  uint32_t start = eg2_rand_range(0, kRevbmWords - 1), stride = stride_for(kRevbmWords);
  uint32_t nonzero = 0;
  for (uint32_t i = 0; i < kRevbmWords; ++i) {
    uint32_t w = order(i, kRevbmWords, start, stride);
    nonzero += eg2_rm_rd32(TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR + 4 * w) != 0;
  }
  ok = eg2_pbf_check("revocation bitmap words not zero", 0, nonzero) && ok;
  eg2_pbf_activity("revocation bitmap words read", kRevbmWords);

  // 2. The main SRAM's tags.
  void *main = (void *)eg2_rm_ptr(TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR);
  start = eg2_rand_range(0, kMainGranules - 1);
  stride = stride_for(kMainGranules);
  uint32_t tagged = 0, first_tagged = 0xffffffff;
  for (uint32_t i = 0; i < kMainGranules; ++i) {
    uint32_t g = order(i, kMainGranules, start, stride);
    uint32_t addr = kEg2PbfMainFree + 8 * g;
    if (tag_at(main, addr)) {
      ++tagged;
      if (first_tagged == 0xffffffff) {
        first_tagged = addr;
      }
    }
  }
  if (tagged != 0) {
    eg2_pbf_note("first tagged main SRAM granule", first_tagged);
  }
  ok = eg2_pbf_check("main SRAM granules tagged before the first CSC", 0, tagged) && ok;
  eg2_pbf_activity("main SRAM tags read", kMainGranules);

  // 3. NVM tags: the image's first page and pages from the seed.
  void *nvm = (void *)eg2_rm_ptr(TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR);
  uint32_t nvm_tagged = 0, nvm_read = 0;
  // The page holding this test's code: the address of a function of the image, page aligned.
  uint32_t image_base =
      (uint32_t)__builtin_cheri_address_get((void *)&cheriot_body) & ~(kEg2PbfRramPage - 1u);
  for (uint32_t p = 0; p <= EG2_PARAM(nvm_pages); ++p) {
    // Page 0 of the image (the manifest's page), else a page of the OTTF non-volatile scratch
    // area: both readable whatever memory protection the boot stages left on the rest.
    uint32_t base = p == 0 ? image_base
                           : eg2_pbf_nv_scratch() +
                                 kEg2PbfRramPage *
                                     eg2_rand_range(0, eg2_pbf_nv_scratch_size() / kEg2PbfRramPage - 1);
    for (uint32_t g = 0; g < kNvmPageGranules; ++g) {
      nvm_tagged += tag_at(nvm, base + 8 * g);
      ++nvm_read;
    }
  }
  ok = eg2_pbf_check("NVM granules tagged", 0, nvm_tagged) && ok;
  eg2_pbf_activity("NVM tags read", nvm_read);
  eg2_pbf_gap("every NVM tag",
              "the run reads the tags of the image's first page and `nvm_pages` drawn pages; all "
              "262144 NVM granules take longer than a chip simulation's time limit");

  // 4. The first CSC.
  uint32_t g = EG2_PARAM(first_granule) % kMainGranules;
  uint32_t addr = kEg2PbfMainFree + 8 * g;
  void *obj = __builtin_cheri_bounds_set(__builtin_cheri_address_set(main, addr), 8);
  void *volatile *slot = (void *volatile *)__builtin_cheri_address_set(main, addr);
  *slot = obj;
  eg2_pbf_note("first CSC to", addr);
  ok = eg2_pbf_check("tag of the stored granule", 1, tag_at(main, addr)) && ok;
  if (g > 0) {
    ok = eg2_pbf_check("tag of the granule below", 0, tag_at(main, addr - 8)) && ok;
  }
  if (g + 1 < kMainGranules) {
    ok = eg2_pbf_check("tag of the granule above", 0, tag_at(main, addr + 8)) && ok;
  }
  // Leave memory as found: a plain store clears the tag (REQ_CMS, tag cleared on data write).
  *(volatile uint32_t *)__builtin_cheri_address_set(main, addr) = 0;
  ok = eg2_pbf_check("tag after a data store", 0, tag_at(main, addr)) && ok;
  return ok;
}

#else  // RISC-V mode

static bool riscv_body(void) {
  eg2_rm_irq_off();
  uint32_t start = eg2_rand_range(0, kRevbmWords - 1), stride = stride_for(kRevbmWords);
  uint32_t faults = 0, other = 0;
  for (uint32_t i = 0; i < kRevbmWords; ++i) {
    uint32_t w = order(i, kRevbmWords, start, stride);
    eg2_rm_out_t out;
    uint32_t cause = eg2_rm_probe(eg2_rm_ptr(TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR + 4 * w),
                                  kEg2RmLw, 0, &out);
    if (cause == kEg2RmCauseLoadFault) {
      ++faults;
    } else {
      ++other;
    }
  }
  bool ok;
  if (EG2_PARAM(model_gaps)) {
    eg2_pbf_model_gap("revbm window error in ePMP mode");
    ok = eg2_pbf_check("revocation bitmap words read", kRevbmWords, faults + other);
  } else {
    ok = eg2_pbf_check("revbm words answered with a load access fault in ePMP mode", kRevbmWords,
                       faults);
  }
  eg2_pbf_activity("revocation bitmap words probed", kRevbmWords);
  eg2_pbf_gap("capability tags in RISC-V mode",
              "ePMP mode has no capability loads; the tags are checked by the CHERIoT build");
  return ok;
}

#endif  // __CHERIOT__

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("meta_zeroed", EG2_PBF_INJECT);
  eg2_pbf_boot_stage();
#ifdef __CHERIOT__
  ok = cheriot_body() && ok;
#else
  ok = riscv_body() && ok;
#endif
  return ok;
}
