// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rv_core_ibex_cheriot_addr_translation (EG2 spec "Core in CHERIoT mode"; REQ_CIB_04).
 *
 * rv_core_ibex's address translation cannot make capability bounds, tags and revocation lookups
 * disagree: either the data and revocation-bitmap addresses translate together, or translation
 * is unavailable in CHERIoT mode (REQ_CIB_04 is derived from the RTL, decisions.md open item 10;
 * rv_core_ibex.hjson IBUS/DBUS_ADDR_MATCHING, REMAP_ADDR, ADDR_EN).
 *
 * In CHERIoT mode the test programs translation slot 0 (`slot`: the instruction bus, the data bus,
 * both) for a window of `size` (the smallest, a middle and the largest the region allows) at a
 * virtual address in unmapped space, onto `region` (main SRAM's arena, sec SRAM, the RRAM area, or
 * retention SRAM, untagged). If the configuration reads back disabled, translation is unavailable
 * and the requirement holds. Otherwise:
 *  - data slot: `tagged_pct` of the physical window's granules hold a tagged capability, the rest
 *    integers. Each granule is reloaded through the virtual address and through the physical one:
 *    the same words and the same tag (tag 0 everywhere in retention SRAM); a CSC through the
 *    virtual address is seen tagged through the physical one. Then capabilities based in the
 *    virtual window, whose physical granules are marked in the revocation bitmap, are reloaded:
 *    revoked (tag 0), as capabilities based on the same physical granules through their physical
 *    addresses are.
 *  - instruction slot: a function in RRAM called through a capability at its virtual address
 *    returns its value.
 *
 * Expected to fail on the RTL of 2026-10-24 in the revocation part: the data bus is translated,
 * the core's bitmap lookup (`main_core_revbm_addr`) is not.
 *
 * The SystemC model does not translate (rv_core_ibex.cfg's translation registers are storage
 * there): with `model_gaps` only the configuration is checked.
 *
 * Built with EG2_ADDR_TRANSLATION_INJECT_FAIL, DBUS_ADDR_EN_0 is expected to read back 0x2: the
 * run must fail with `MISMATCH addr_en`.
 */

#include "hw/top/rv_core_ibex_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define ADDR_TRANSLATION_PARAMS(X) \
  X(slot, 0, 0, 3)                 \
  X(region, 0, 0, 4)               \
  X(size, 0, 0, 3)                 \
  X(tagged_pct, 101, 0, 101)       \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(ADDR_TRANSLATION_PARAMS);

enum {
  kVirtData = 0x20000000u,  // unmapped
  kVirtCode = 0x28000000u,  // unmapped
  kRetArea = 0x40601c00u,
};

static eg2_tally_t tally;

/** The function the instruction slot reaches through its virtual address. */
static OT_NOINLINE __attribute__((aligned(64))) uint32_t xlate_target(void) {
  return 0x7a5e1234u;
}

static void program_slot(bool ibus, uint32_t virt, uint32_t phys, uint32_t size) {
  uint32_t base = ibus ? RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET : RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET;
  uint32_t match = ibus ? RV_CORE_IBEX_IBUS_ADDR_MATCHING_0_REG_OFFSET
                        : RV_CORE_IBEX_DBUS_ADDR_MATCHING_0_REG_OFFSET;
  uint32_t remap = ibus ? RV_CORE_IBEX_IBUS_REMAP_ADDR_0_REG_OFFSET
                        : RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET;
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, match, virt | ((size - 1) >> 1));
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, remap, phys);
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, base, 1);
}

static bool slot_enabled(bool ibus) {
  uint32_t en = eg2_dev_read32(kEg2DevRvCoreIbexCfg, ibus ? RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET
                                                          : RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET);
  uint32_t exp_bits = 0;
#ifdef EG2_ADDR_TRANSLATION_INJECT_FAIL
  exp_bits = 2;
#endif
  eg2_check(&tally, "addr_en", ibus ? 1 : 0, en & 2, exp_bits);
  return (en & 1) != 0;
}

static void data_slot(uint32_t region, uint32_t size, uint32_t pct, bool gaps) {
  uint32_t phys;
  switch (region) {
    case 1:
      phys = eg2_main_arena();
      break;
    case 2:
      phys = kEg2SecBase;
      break;
    case 3:
      phys = kEg2MsRramArea;
      break;
    default:
      phys = kRetArea;
      break;
  }
  uint32_t virt = kVirtData;
  bool tagged_region = region != 4;
  // The words: tagged capabilities in pct of the granules (RRAM: programmed, then CSC).
  uint32_t n = size / 8 > 64 ? 64 : size / 8;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t g = phys + 8 * i;
    bool cap = eg2_rand_range(0, 99) < pct;
    void *c = eg2_ms_object_cap(kEg2ArenaA + 8 * i, 8, false);
    if (region == 3) {
      uint32_t w[4] = {eg2_ms_cap_addr(c), eg2_ms_cap_high(c), 0, 0};
      uint32_t err;
      if ((g & 8) == 0 && !eg2_ms_rram_program(g, w, 4, &err)) {
        eg2_check(&tally, "rram program", i, err, 0);
      }
      if ((g & 8) == 0 && cap) {
        eg2_ms_csc(g, c);
      }
    } else if (cap) {
      eg2_ms_csc(g, c);
    } else {
      eg2_ms_write32(g, eg2_rand());
      eg2_ms_write32(g + 4, eg2_rand());
    }
  }
  program_slot(false, virt, phys, size);
  if (!slot_enabled(false)) {
    eg2_puts("EG2 data translation refused: REQ_CIB_04 holds\r\n");
    return;
  }
  if (gaps) {
    eg2_puts("EG2 model: no translation, accesses not checked\r\n");
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0);
    return;
  }
  for (uint32_t i = 0; i < n; ++i) {
    ++tally.planned;
    void *p = eg2_ms_clc(phys + 8 * i);
    void *v = eg2_ms_clc(virt + 8 * i);
    eg2_check(&tally, "tag virt = phys", i, eg2_ms_cap_tag(v), eg2_ms_cap_tag(p));
    eg2_check(&tally, "lo virt = phys", i, eg2_ms_cap_addr(v), eg2_ms_cap_addr(p));
    eg2_check(&tally, "hi virt = phys", i, eg2_ms_cap_high(v), eg2_ms_cap_high(p));
    if (!tagged_region) {
      eg2_check(&tally, "untagged region", i, eg2_ms_cap_tag(v), 0);
    }
    ++tally.done;
  }
  if (region != 3) {
    // A CSC through the virtual address, seen through the physical one.
    void *c = eg2_ms_object_cap(kEg2ArenaA, 8, false);
    eg2_ms_csc(virt, c);
    eg2_check(&tally, "csc virt, tag phys", 0, eg2_ms_tag(phys), tagged_region ? 1 : 0);
  }
  // Revocation through the window: capabilities based in the virtual window, their physical
  // granules marked. Stored in sec SRAM's arena B.
  if (tagged_region && region != 4 && phys >= kEg2MsHeapBase && phys < kEg2MsHeapTop) {
    eg2_ms_revbm_clear();
    uint32_t k = n > 16 ? 16 : n;
    for (uint32_t i = 0; i < k; ++i) {
      eg2_ms_csc(kEg2ArenaB + 16 * i, eg2_ms_object_cap(virt + 8 * i, 8, false));
      eg2_ms_csc(kEg2ArenaB + 16 * i + 8, eg2_ms_object_cap(phys + 8 * i, 8, false));
      eg2_ms_revbm_mark(phys + 8 * i, true);
    }
    for (uint32_t i = 0; i < k; ++i) {
      ++tally.planned;
      uint32_t tv = eg2_ms_tag(kEg2ArenaB + 16 * i);
      uint32_t tp = eg2_ms_tag(kEg2ArenaB + 16 * i + 8);
      eg2_check(&tally, "revoked through phys", i, tp, 0);
      eg2_check(&tally, "revoked through virt", i, tv, 0);
      ++tally.done;
    }
    eg2_ms_revbm_clear();
  }
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DBUS_ADDR_EN_0_REG_OFFSET, 0);
}

static void code_slot(uint32_t size, bool gaps) {
  uint32_t fn = EG2_ADDR(xlate_target);
  uint32_t phys = fn & ~(size - 1);
  program_slot(true, kVirtCode, phys, size);
  if (!slot_enabled(true)) {
    eg2_puts("EG2 instruction translation refused: REQ_CIB_04 holds\r\n");
    return;
  }
  if (!gaps) {
    ++tally.planned;
    uint32_t (*f)(void) = xlate_target;
    void *c = (void *)f;
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(kVirtCode + (fn - phys)));
    f = (uint32_t(*)(void))c;
    eg2_check(&tally, "call through the virtual address", 0, f(), 0x7a5e1234u);
    ++tally.done;
  }
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_IBUS_ADDR_EN_0_REG_OFFSET, 0);
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init() || !eg2_ms_rram_setup()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t slot = EG2_PARAM(slot) != 0 ? EG2_PARAM(slot) : eg2_rand_range(1, 3);
  uint32_t region = EG2_PARAM(region) != 0 ? EG2_PARAM(region) : eg2_rand_range(1, 4);
  uint32_t size_cls = EG2_PARAM(size) != 0 ? EG2_PARAM(size) : eg2_rand_range(1, 3);
  uint32_t pct = EG2_PARAM(tagged_pct) == 101 ? eg2_rand_range(0, 100) : EG2_PARAM(tagged_pct);
  // The largest window the region allows: the main SRAM arena 2 KiB, sec SRAM 64 KiB, the RRAM
  // area 32 KiB, retention SRAM's test area 512 bytes.
  uint32_t largest = region == 1 ? 2048 : region == 2 ? 0x10000 : region == 3 ? 0x8000 : 512;
  uint32_t size = size_cls == 1 ? 256 : size_cls == 2 ? (largest > 4096 ? 4096 : largest / 2)
                                                      : largest;
  eg2_put_kv("EG2 addr_translation slot", slot);
  eg2_put_kv(" region", region);
  eg2_put_kv(" size", size);
  eg2_put_kv(" tagged_pct", pct);
  eg2_puts("\r\n");
  if (slot & 2) {
    data_slot(region, size, pct, gaps);
  }
  if (slot & 1) {
    code_slot(size_cls == 1 ? 256 : size_cls == 2 ? 4096 : 0x10000, gaps);
  }
  eg2_tally_print("addr_translation", &tally);
  return eg2_tally_ok(&tally);
}
