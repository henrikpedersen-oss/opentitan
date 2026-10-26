// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_rram_mem_protection (EG2 verification spec REQ_RRAM_14, "RRAM controller in RISC-V
 * mode"): data regions and a default region with read, write, scramble and ECC attributes; the
 * lowest index wins; REGWEN locks; the rules apply to host reads too; emulated info windows are
 * always host-blocked and fail closed when they overlap.
 *
 * Expected behaviour, from rram_ctrl's documents (theory of operation "Memory Protection", "Data
 * partition", "Emulated Info Pages", "Access attributes"; programmer's guide "Configuring Memory
 * Protection", "Error Encountered by Software Initiated Controller Operations"; hjson
 * MP_REGION_CFG, EMUL_INFO_REGION, EMUL_INFO_SUBREGION):
 *   - a page's rule is the lowest-indexed enabled region holding it, DEFAULT_REGION if none;
 *   - a controller read or write the rule forbids ends with OP_STATUS.err, ERR_CODE.mp_err,
 *     ERR_ADDR the first denied address, the recov_err alert; a denied read returns 0xFFFFFFFF
 *     from the denied word on;
 *   - a host read of a page whose rule has no rd_en is answered with an error (host reads "are
 *     subject to the same memory protection"): a load access fault, mcause 5;
 *   - a locked region (REGION_CFG_REGWEN = 0) keeps its configuration;
 *   - an emulated info window, once enabled: host access always blocked; controller access only
 *     through an enabled subregion (window locked, subregion placement locked from the first),
 *     with the subregion's attributes; the rest of the window denied; two enabled windows that
 *     overlap disable both windows' subregions, so both are denied entirely. Subregion 0 is
 *     taken as window 0's first subregion (the hjson numbers 24 subregions for 2 windows of 12).
 * Info pages 0-4 (no life-cycle gate) are probed under INFO_PAGE_CFG the same way.
 *
 * The test image's pages get a region of their own with the default region's attributes first, so
 * DEFAULT_REGION can be drawn freely for the scratch pages. Locks (a region's REGWEN, the
 * emulated window) last until reset, so they come last and only on scratch pages.
 *
 * Knobs:
 *   n_regions     test regions over the 16-page scratch window (0-8, as free regions allow)
 *   n_probes      controller write, controller read and host read probes per phase
 *   dflt_attrs    DEFAULT_REGION's attributes (bit 0 rd, 1 wr, 2 scramble, 3 ECC); 16: drawn
 *   info          1: probe an info page (0-4) under a drawn INFO_PAGE_CFG
 *   lock          1: lock one test region and check it keeps its configuration
 *   emul          0: no emulated window, 1: one window with a subregion, 2: and an overlapping
 *                 second window, 3: drawn
 *   model_gaps    skips what the SystemC model does not model:
 *
 * | Skipped with model_gaps = 1                | Why (eg2_mem.h, Eg2RramCtrl)                     |
 * |--------------------------------------------|--------------------------------------------------|
 * | a host read denied by the rule (the fault) | "host-read memory protection" is not modelled    |
 * | the emulated info windows                  | "the emulated info windows (treated as absent)"  |
 *
 * Built with EG2_RRAM_MEM_PROTECTION_INJECT_FAIL the first value check expects bit 0 flipped: the
 * run must fail with `RRAM FAIL`.
 */

#include "sw/device/tests/eg2_rram/eg2_rram.h"

EG2_TEST_CONFIG();
EG2_CHK_INJECT_FAIL_DEFINE;

#define MEM_PROTECTION_PARAMS(X) \
  X(n_regions, 4, 0, 8)          \
  X(n_probes, 24, 1, 256)        \
  X(dflt_attrs, 16, 0, 16)       \
  X(info, 1, 0, 1)               \
  X(lock, 1, 0, 1)               \
  X(emul, 3, 0, 3)               \
  X(model_gaps, 0, 0, 1)
EG2_CHK_PARAMS(MEM_PROTECTION_PARAMS)

enum {
  kPolls = 200000,
  kWinPages = 16,
  kNoData = 0xffffffffu,  // what a denied controller read returns
};

static uint32_t win_first;  // the scratch window's first page
static bool ok_all = true;
static int recov_alert = -1;
static uint32_t mp_errors;

static void chk(bool c) { ok_all = c && ok_all; }

/** Per page of the window: the RRAM word last written there (byte address, or
 * 0) and its data. */
typedef struct page_data {
  uint32_t addr;
  uint32_t w[4];
} page_data_t;
// In sec SRAM: the CHERIoT image keeps its globals under 4 KiB (REQ_STE_12).
static page_data_t *pages;

/** The effective attributes of data page `p` from the regions and
 * DEFAULT_REGION as set now. */
static uint32_t rule(uint32_t p) {
  for (uint32_t i = 0; i < kEg2RramRegions; ++i) {
    uint32_t cfg = eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * i);
    if ((cfg & 0xf) != kEg2RramMubiTrue) {
      continue;
    }
    uint32_t rg = eg2_rram_reg(RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4 * i);
    uint32_t base = rg & RRAM_CTRL_MP_REGION_0_BASE_0_MASK;
    uint32_t size = (rg >> RRAM_CTRL_MP_REGION_0_SIZE_0_OFFSET) &
                    RRAM_CTRL_MP_REGION_0_SIZE_0_MASK;
    if (p >= base && p <= base + size) {
      return eg2_rram_cfg_attrs(cfg, false);
    }
  }
  return eg2_rram_cfg_attrs(eg2_rram_reg(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET),
                            true);
}

/** Notes an mp_err the test caused: the recov_err alert must follow. */
static void expect_recov(void) {
  ++mp_errors;
  if (recov_alert >= 0) {
    eg2_status_expect_alert((size_t)recov_alert);
    chk(eg2_chk_true("recov_err alert after mp_err",
                     eg2_status_wait_alert((size_t)recov_alert, 10000)));
    eg2_status_clear_expected();
  }
}

/** A controller write of one RRAM word at `a` (partition `part`), allowed or
 * not. */
static void probe_write(uint32_t part, uint32_t a, bool allowed,
                        page_data_t *pd) {
  uint32_t w[4];
  for (int i = 0; i < 4; ++i) {
    w[i] = eg2_rand();
  }
  eg2_rram_result_t r;
  chk(eg2_chk_true("probe write ends", eg2_rram_write(part, a, w, 4, &r)));
  if (allowed) {
    chk(eg2_chk_eq("allowed write OP_STATUS", 1, r.op_status));
    chk(eg2_chk_eq("allowed write ERR_CODE", 0, r.err_code));
    if (pd != NULL) {
      pd->addr = a;
      for (int i = 0; i < 4; ++i) {
        pd->w[i] = w[i];
      }
    }
  } else {
    chk(eg2_chk_eq("denied write OP_STATUS (done, err)", 3, r.op_status));
    chk(eg2_chk_eq("denied write ERR_CODE (mp_err)", 1u << 1, r.err_code));
    chk(eg2_chk_eq("denied write ERR_ADDR", a, r.err_addr));
    expect_recov();
  }
}

/** A controller read of one RRAM word at `a`. */
static void probe_read(uint32_t part, uint32_t a, bool allowed,
                       const page_data_t *pd) {
  uint32_t w[4];
  eg2_rram_result_t r;
  chk(eg2_chk_true("probe read ends", eg2_rram_read(part, a, w, 4, &r)));
  if (allowed) {
    chk(eg2_chk_eq("allowed read OP_STATUS", 1, r.op_status));
    chk(eg2_chk_eq("allowed read ERR_CODE", 0, r.err_code));
    if (pd != NULL && pd->addr == a) {
      for (int i = 0; i < 4; ++i) {
        chk(eg2_chk_eq("allowed read data", pd->w[i], w[i]));
      }
    }
  } else {
    chk(eg2_chk_eq("denied read OP_STATUS (done, err)", 3, r.op_status));
    chk(eg2_chk_eq("denied read ERR_CODE (mp_err)", 1u << 1, r.err_code));
    chk(eg2_chk_eq("denied read ERR_ADDR", a, r.err_addr));
    for (int i = 0; i < 4; ++i) {
      chk(eg2_chk_eq("denied read data (all ones)", kNoData, w[i]));
    }
    expect_recov();
  }
}

/** A host load at data-partition address `a`. */
static void probe_host(uint32_t a, bool allowed, const page_data_t *pd) {
  eg2_chk_trap_t t;
  uint32_t cause = eg2_chk_access(eg2_rram_host(a), kEg2ChkLw, 0, &t);
  if (allowed) {
    chk(eg2_chk_eq("allowed host read mcause", 0, cause));
    if (pd != NULL && pd->addr == a) {
      chk(eg2_chk_eq("allowed host read data", pd->w[0], t.value));
    }
  } else if (EG2_PARAM(model_gaps)) {
    eg2_chk_skip("a host read denied by the region rules");
  } else {
    chk(eg2_chk_eq("denied host read mcause (load access fault)",
                   kEg2ChkCauseLoadFault, cause));
    chk(eg2_chk_eq("denied host read mtval", kEg2RramHostBase + a, t.mtval));
  }
}

static uint32_t cur_rule(uint32_t p);
static bool host_blocked(uint32_t p);

/** One probe of a drawn kind on a drawn page of the window, under the rules as
 * they are now. */
static void probe_window(const char *phase) {
  uint32_t i = eg2_rand_range(0, kWinPages - 1);
  uint32_t p = win_first + i;
  uint32_t attrs = cur_rule(p);
  page_data_t *pd = &pages[i];
  uint32_t a =
      pd->addr != 0 && eg2_rand_range(0, 1)
          ? pd->addr
          : p * kEg2RramPageBytes + kEg2RramWordBytes * eg2_rand_range(0, 31);
  eg2_chk_step(phase, p);
  eg2_chk_step("rule attrs", attrs);
  switch (eg2_rand_range(0, 2)) {
    case 0:
      probe_write(kEg2RramPartData, a, (attrs & kEg2RramAttrWr) != 0, pd);
      break;
    case 1:
      probe_read(kEg2RramPartData, a, (attrs & kEg2RramAttrRd) != 0, pd);
      break;
    default:
      probe_host(a, (attrs & kEg2RramAttrRd) != 0 && !host_blocked(p), pd);
      break;
  }
}

// ---- The emulated info windows ----
static uint32_t emul_first;  // window 0's first page
static uint32_t emul_top;    // subregion 0's TOP (pages from emul_first)
static uint32_t emul_attrs;  // subregion 0's attributes
static bool emul_overlap;
static bool emul_active;

static uint32_t emul_rule(uint32_t p) {
  if (p < emul_first || p > emul_first + 3) {
    return rule(p);
  }
  if (!emul_overlap && p <= emul_first + emul_top) {
    return emul_attrs;
  }
  return 0;  // the window's deny entry
}

static bool host_blocked(uint32_t p) {
  return emul_active && p >= emul_first && p <= emul_first + 3;
}

static uint32_t cur_rule(uint32_t p) {
  return emul_active ? emul_rule(p) : rule(p);
}

bool eg2_test_main(void) {
  eg2_chk_area("RRAM");
  if (!eg2_chk_params_begin()) {
    return false;
  }
  eg2_chk_irq_off();
  if (!eg2_chk_true("controller initialised", eg2_rram_wait_init(kPolls))) {
    return false;
  }
  recov_alert = eg2_status_alert_find("rram_ctrl_recov_err");
  pages = (page_data_t *)eg2_chk_ptr(TOP_EARLGREY_SRAM_CTRL_SEC_RAM_BASE_ADDR);
  for (uint32_t i = 0; i < kWinPages; ++i) {
    pages[i].addr = 0;
    for (int k = 0; k < 4; ++k) {
      pages[i].w[k] = 0;
    }
  }
  win_first = eg2_rand_range(kEg2RramScratchFirstPage,
                             kEg2RramScratchLastPage - kWinPages + 1);
  eg2_chk_step("window first page", win_first);

  // The image's pages: a region with the default region's attributes, so the
  // default can change.
  uint32_t dflt_boot = eg2_rram_reg(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET);
  uint32_t image_region = eg2_rram_free_region();
  if (!eg2_chk_true("a free region for the image",
                    image_region < kEg2RramRegions)) {
    return false;
  }
  eg2_rram_region_set(image_region, 0, kEg2RramScratchFirstPage,
                      eg2_rram_cfg_attrs(dflt_boot, true));

  // The test regions: free ones only, each over a drawn part of the window.
  uint32_t mine[kEg2RramRegions];
  uint32_t n_mine = 0;
  for (uint32_t i = 0; i < kEg2RramRegions && n_mine < EG2_PARAM(n_regions);
       ++i) {
    uint32_t cfg = eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * i);
    uint32_t regwen =
        eg2_rram_reg(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * i);
    if (i == image_region || (cfg & 0xf) == kEg2RramMubiTrue ||
        (regwen & 1) == 0) {
      continue;
    }
    uint32_t base = win_first + eg2_rand_range(0, kWinPages - 1);
    uint32_t size = eg2_rand_range(1, 8);
    if (base + size > win_first + kWinPages) {
      size = win_first + kWinPages - base;
    }
    uint32_t attrs = eg2_rand_range(0, 15);
    eg2_rram_region_set(i, base, size, attrs);
    eg2_chk_step("region", i);
    eg2_chk_step("region base page", base);
    eg2_chk_step("region pages", size);
    eg2_chk_step("region attrs", attrs);
    mine[n_mine++] = i;
  }
  uint32_t dattrs = EG2_PARAM(dflt_attrs) == 16 ? eg2_rand_range(0, 15)
                                                : EG2_PARAM(dflt_attrs);
  eg2_rram_reg_wr(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET,
                  eg2_rram_default_cfg(dattrs));
  eg2_chk_step("default attrs", dattrs);

  for (uint32_t i = 0; i < EG2_PARAM(n_probes); ++i) {
    probe_window("probe page");
  }

  if (EG2_PARAM(info)) {
    uint32_t page = eg2_rand_range(0, 4);
    uint32_t attrs = eg2_rand_range(0, 15);
    eg2_rram_info_set(page, attrs);
    eg2_chk_step("info page", page);
    eg2_chk_step("info attrs", attrs);
    page_data_t pd = {0};
    uint32_t a =
        page * kEg2RramPageBytes + kEg2RramWordBytes * eg2_rand_range(0, 31);
    probe_write(kEg2RramPartInfo, a, (attrs & kEg2RramAttrWr) != 0, &pd);
    probe_read(kEg2RramPartInfo, a, (attrs & kEg2RramAttrRd) != 0, &pd);
    eg2_rram_reg_wr(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * page,
                    RRAM_CTRL_INFO_PAGE_CFG_0_REG_RESVAL);
  }

  uint32_t locked = kEg2RramRegions;
  if (EG2_PARAM(lock) && n_mine > 0) {
    locked = mine[eg2_rand_range(0, n_mine - 1)];
    uint32_t cfg =
        eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * locked);
    uint32_t rg = eg2_rram_reg(RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4 * locked);
    eg2_rram_reg_wr(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * locked, 0);
    eg2_chk_step("locked region", locked);
    eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * locked,
                    eg2_rram_region_cfg(~eg2_rram_cfg_attrs(cfg, false) & 0xf));
    eg2_rram_reg_wr(RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4 * locked, rg ^ 1);
    eg2_rram_reg_wr(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * locked, 1);
    chk(eg2_chk_eq(
        "locked MP_REGION_CFG unchanged", cfg,
        eg2_rram_reg(RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4 * locked)));
    chk(eg2_chk_eq(
        "locked MP_REGION unchanged", rg,
        eg2_rram_reg(RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4 * locked)));
    chk(eg2_chk_eq(
        "REGION_CFG_REGWEN stays locked", 0,
        eg2_rram_reg(RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4 * locked) &
            1));
    probe_window("probe page after the lock");
  }

  uint32_t emul = EG2_PARAM(emul) == 3 ? eg2_rand_range(0, 2) : EG2_PARAM(emul);
  if (emul != 0 && EG2_PARAM(model_gaps)) {
    eg2_chk_skip("the emulated info windows");
  } else if (emul != 0) {
    emul_first = win_first + eg2_rand_range(0, kWinPages - 4);
    emul_top = eg2_rand_range(0, 2);
    emul_attrs = eg2_rand_range(0, 15);
    emul_overlap = emul == 2;
    eg2_chk_step("emulated window first page", emul_first);
    eg2_chk_step("subregion 0 TOP", emul_top);
    eg2_chk_step("subregion 0 attrs", emul_attrs);
    // Subregion 0 (the window's first): its placement, then its attributes,
    // then the locks.
    eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_SUBREGION_0_REG_OFFSET, emul_top);
    eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_SUBREGION_CFG_0_REG_OFFSET,
                    eg2_rram_region_cfg(emul_attrs) & ~0xfu);
    eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_REGION_0_REG_OFFSET,
                    emul_first |
                        3u << RRAM_CTRL_EMUL_INFO_REGION_0_SIZE_0_OFFSET |
                        (uint32_t)kEg2RramMubiTrue
                            << RRAM_CTRL_EMUL_INFO_REGION_0_EN_0_OFFSET);
    eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_REGWEN_0_REG_OFFSET, 0);
    eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_SUBREGION_REGWEN_0_REG_OFFSET, 0);
    if (emul_overlap) {
      eg2_chk_step("overlapping window 1 first page", emul_first + 1);
      eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_REGION_1_REG_OFFSET,
                      (emul_first + 1) |
                          1u << RRAM_CTRL_EMUL_INFO_REGION_0_SIZE_0_OFFSET |
                          (uint32_t)kEg2RramMubiTrue
                              << RRAM_CTRL_EMUL_INFO_REGION_0_EN_0_OFFSET);
      eg2_rram_reg_wr(RRAM_CTRL_EMUL_INFO_REGWEN_1_REG_OFFSET, 0);
    }
    emul_active = true;
    // Data written under the earlier rules may have another scramble setting:
    // forget it.
    for (uint32_t k = 0; k < 4; ++k) {
      pages[emul_first - win_first + k].addr = 0;
    }
    // The window's pages: each probed by the controller (write, read) and the
    // host.
    for (uint32_t k = 0; k < 4; ++k) {
      uint32_t p = emul_first + k;
      uint32_t attrs = emul_rule(p);
      uint32_t a =
          p * kEg2RramPageBytes + kEg2RramWordBytes * eg2_rand_range(0, 31);
      page_data_t pd = {0};
      eg2_chk_step("emulated window page", p);
      eg2_chk_step("rule attrs", attrs);
      probe_write(kEg2RramPartData, a, (attrs & kEg2RramAttrWr) != 0, &pd);
      probe_read(kEg2RramPartData, a, (attrs & kEg2RramAttrRd) != 0, &pd);
      probe_host(a, false, &pd);  // always blocked for the host
    }
    for (uint32_t i = 0; i < EG2_PARAM(n_probes) / 4; ++i) {
      probe_window("probe page with the window");
    }
  }

  // Restore: the default region first, then the test's regions (not the locked
  // one).
  eg2_rram_reg_wr(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET, dflt_boot);
  for (uint32_t i = 0; i < n_mine; ++i) {
    if (mine[i] != locked) {
      eg2_rram_region_off(mine[i]);
    }
  }
  eg2_rram_region_off(image_region);
  eg2_chk_step("mp_err seen", mp_errors);
  return eg2_chk_summary(EG2_PARAM(n_probes)) && ok_all;
}
