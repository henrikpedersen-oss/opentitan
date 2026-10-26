// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

#include "hw/top/aon_timer_regs.h"     // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/rv_dm_regs.h"         // Generated.
#include "hw/top/rv_plic_regs.h"       // Generated.
#include "hw/top/rv_timer_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"

// The kernel context's layout is the assembly's (eg2_dm_kernels.S).
#define EG2_DM_PSZ sizeof(void *)
_Static_assert(offsetof(eg2_dm_kctx_t, dev) == 1 * EG2_DM_PSZ, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, slots) == 2 * EG2_DM_PSZ, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, msip) == 3 * EG2_DM_PSZ, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, ack) == 4 * EG2_DM_PSZ, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, ack_val) == 7 * EG2_DM_PSZ, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, max) == 7 * EG2_DM_PSZ + 12, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, arg) == 7 * EG2_DM_PSZ + 16, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, n) == 7 * EG2_DM_PSZ + 20, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, sum) == 7 * EG2_DM_PSZ + 24, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, traps) == 7 * EG2_DM_PSZ + 28, "kctx layout");
_Static_assert(offsetof(eg2_dm_kctx_t, irqs) == 7 * EG2_DM_PSZ + 32, "kctx layout");

// Written by the bench over SBA. In .bss: main SRAM in both modes.
static volatile uint32_t done_;
static volatile uint32_t cmd_;
static volatile uint32_t stop_;
static volatile uint32_t scratch_[kEg2DmScratchWords];
// CAP's two slots: at byte offsets 0 and 8 (indices 0 and 8 / sizeof(void *)).
static void *volatile slots_[16 / sizeof(void *)] __attribute__((aligned(8)));
enum { kSlot1 = 8 / sizeof(void *) };

// LOAD's word: read-only data, in RRAM with the image.
static const volatile uint32_t kLoadWord = 0x2468ace1u;

volatile uint32_t *eg2_dm_scratch(void) { return scratch_; }
volatile uint32_t *eg2_dm_stop(void) { return &stop_; }

static inline uint32_t csr_mie_swap(uint32_t v) {
  uint32_t old;
  __asm__ volatile("csrrw %0, mie, %1" : "=r"(old) : "r"(v));
  return old;
}

static volatile uint32_t *dev_reg(eg2_dev_id_t dev, uint32_t offset) {
  return eg2_dev(dev) + offset / sizeof(uint32_t);
}

static uint32_t div_ref(uint32_t n) {
  uint32_t sum = 0;
  for (uint32_t i = 0; i < n; ++i) {
    int32_t d = (int32_t)((i & 255u) + 3u);
    int32_t q = (int32_t)(sum + 1365u) / d;
    sum += (uint32_t)q;
  }
  return sum;
}

static const char *const kKernelNames[kEg2DmKernelCount] = {
    "none", "div", "load", "store", "cap", "wfi", "trap", "irq", "nmi", "park",
};

bool eg2_dm_run_kernel(const eg2_dm_cfg_t *cfg, uint32_t kind, uint32_t max) {
  bool ok = true;
  eg2_dm_kctx_t k = {0};
  k.stop = &stop_;
  k.max = max;
  uint32_t mie_set = 0;
  bool mie_on = false;
  void *slot0 = (void *)&scratch_[0];
  void *slot1 = (void *)&scratch_[1];

  switch (kind) {
    case kEg2DmLoad:
      k.dev = (volatile uint32_t *)&kLoadWord;
      break;
    case kEg2DmStore:
      k.dev = dev_reg(kEg2DevAonTimer, AON_TIMER_WKUP_THOLD_LO_REG_OFFSET);
      break;
    case kEg2DmCap:
      slots_[0] = slot0;
      slots_[kSlot1] = slot1;
      k.slots = slots_;
      break;
    case kEg2DmWfi: {
      k.dev = eg2_dev(kEg2DevRvTimer);
      k.arg = cfg->wfi_ticks;
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET,
                      (23u << RV_TIMER_CFG0_PRESCALE_OFFSET) | (1u << RV_TIMER_CFG0_STEP_OFFSET));
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, cfg->wfi_ticks);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
      eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
      mie_set = 1u << 7;  // MTIE: wakes wfi; mstatus.MIE stays off
      break;
    }
    case kEg2DmIrq:
      k.dev = dev_reg(kEg2DevRvPlic, RV_PLIC_MSIP0_REG_OFFSET);
      k.msip = k.dev;
      *k.msip = 0;
      mie_set = 1u << 3;  // MSIE
      mie_on = true;
      break;
    case kEg2DmNmi:
      k.dev = eg2_dev(kEg2DevAonTimer);
      k.arg = cfg->nmi_spin;
      k.ack[0] = dev_reg(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET);
      k.ack_val[0] = 0;
      k.ack[1] = dev_reg(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET);
      k.ack_val[1] = 1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT;
      k.ack[2] = dev_reg(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_STATE_REG_OFFSET);
      k.ack_val[2] = 1u << RV_CORE_IBEX_NMI_STATE_WDOG_BIT;
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, 0xffffffffu);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, cfg->nmi_bark);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_INTR_STATE_REG_OFFSET,
                      1u << AON_TIMER_INTR_STATE_WDOG_TIMER_BARK_BIT);
      eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_NMI_ENABLE_REG_OFFSET,
                      1u << RV_CORE_IBEX_NMI_ENABLE_WDOG_EN_BIT);
      break;
    default:
      break;
  }

  eg2_chk_irq_off();
  uint32_t mie_old = csr_mie_swap(mie_set);
  if (mie_on) {
    eg2_chk_irq_on();
  }
  uint32_t n = eg2_dm_kernel(kind, &k);
  eg2_chk_irq_off();
  (void)csr_mie_swap(mie_old);

  // Tear-down: nothing the kernel armed fires afterwards.
  if (kind == kEg2DmWfi) {
    eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
    eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
  } else if (kind == kEg2DmIrq) {
    *k.msip = 0;
  } else if (kind == kEg2DmNmi) {
    eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
  }

  const char *name = kind < kEg2DmKernelCount ? kKernelNames[kind] : "?";
  eg2_chk_step(name, n);
  ok = eg2_chk_eq("kernel iterations reported", n, k.n) && ok;
  ok = eg2_chk_true("kernel made progress", n >= 1) && ok;
  ok = eg2_chk_true("kernel within its limit", n <= max) && ok;
  ok = eg2_chk_eq("kernel exceptions", kind == kEg2DmTrap ? n : 0, k.traps) && ok;
  ok = eg2_chk_eq("kernel interrupts",
                  (kind == kEg2DmIrq || kind == kEg2DmNmi) ? n : 0, k.irqs) &&
       ok;
  switch (kind) {
    case kEg2DmDiv:
      ok = eg2_chk_eq("div checksum", div_ref(n), k.sum) && ok;
      break;
    case kEg2DmLoad:
      ok = eg2_chk_eq("load checksum", n * kLoadWord, k.sum) && ok;
      break;
    case kEg2DmStore:
      ok = eg2_chk_eq("store count", n, k.sum) && ok;
      ok = eg2_chk_eq("last value stored across the CDC", n - 1, *k.dev) && ok;
      break;
    case kEg2DmCap: {
      bool odd = (n & 1u) != 0;
      void *exp0 = odd ? slot1 : slot0;
      void *exp1 = odd ? slot0 : slot1;
      ok = eg2_chk_eq("cap count", n, k.sum) && ok;
      ok = eg2_chk_eq("slot 0 after the swaps", eg2_chk_addr_of(exp0),
                      eg2_chk_addr_of(slots_[0])) &&
           ok;
      ok = eg2_chk_eq("slot 1 after the swaps", eg2_chk_addr_of(exp1),
                      eg2_chk_addr_of(slots_[kSlot1])) &&
           ok;
#ifdef __CHERIOT__
      ok = eg2_chk_true("slot 0 tag kept", __builtin_cheri_tag_get(slots_[0])) && ok;
      ok = eg2_chk_true("slot 1 tag kept", __builtin_cheri_tag_get(slots_[kSlot1])) && ok;
#endif
      break;
    }
    default:
      ok = eg2_chk_eq("kernel count", n, k.sum) && ok;
      break;
  }
  return ok;
}

static bool run_command(const eg2_dm_cfg_t *cfg, uint32_t kind, uint32_t arg, uint32_t max) {
  if (kind >= kEg2DmAction) {
    if (cfg->action == NULL) {
      return eg2_chk_true("action known", false);
    }
    return cfg->action(kind, arg);
  }
  return eg2_dm_run_kernel(cfg, kind, max);
}

bool eg2_dm_serve(const eg2_dm_cfg_t *cfg) {
  bool ok = true;
  if (cfg->late_debug_sw) {
    eg2_dev_write32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET, kMultiBitBool32True);
    ok = eg2_chk_eq("late debug enable", kMultiBitBool32True,
                    eg2_dev_read32(kEg2DevRvDmRegs, RV_DM_LATE_DEBUG_ENABLE_REG_OFFSET)) &&
         ok;
  }
  // The reset reason of this boot: rstmgr RESET_INFO as the test ROM saved it in retention SRAM
  // (retention_sram_t.creator.reset_reasons, offset 4) before clearing the register. The bench
  // checks it after an ndmreset or an escalation reset.
  uint32_t reset_info = eg2_chk_rd32(TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 4);
  LOG_INFO("EG2 DEBUG boot 0x%08x", reset_info);
  done_ = 0;
  cmd_ = 0;
  stop_ = 0;
  LOG_INFO("EG2 DEBUG ready 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x", eg2_chk_addr_of(&done_),
           eg2_chk_addr_of(&scratch_[0]), eg2_chk_addr_of(&cmd_), eg2_chk_addr_of(&stop_),
           eg2_dm_kernel_target(0),
           (uint32_t)(TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR +
                      RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET));

  if (cfg->model_gaps) {
    for (uint32_t kind = 1; kind < kEg2DmKernelCount; ++kind) {
      if ((cfg->model_kinds >> kind) & 1u) {
        ok = eg2_dm_run_kernel(cfg, kind, cfg->model_iters) && ok;
      } else {
        eg2_chk_skip(kKernelNames[kind]);
      }
    }
    for (uint32_t a = 0; a < 16; ++a) {
      if ((cfg->model_actions >> a) & 1u) {
        ok = run_command(cfg, kEg2DmAction + a, 0, cfg->model_iters) && ok;
      }
    }
    eg2_chk_skip("the bench's JTAG and SBA accesses, commands and the done word (no debug module)");
    return ok;
  }

  uint32_t last_seq = 0;
  uint32_t commands = 0;
  uint32_t polls = 0;
  const uint32_t magic = kEg2DebugMagic + cfg->test_id;
  while (done_ == 0 && polls < cfg->idle_polls) {
    uint32_t c = cmd_;
    uint32_t seq = c >> 16;
    if (seq == 0 || seq == last_seq) {
      ++polls;
      continue;
    }
    polls = 0;
    last_seq = seq;
    uint32_t kind = c & 0xffu;
    uint32_t arg = (c >> 8) & 0xffu;
    stop_ = 0;
    LOG_INFO("EG2 DEBUG start 0x%08x 0x%08x 0x%08x", seq, kind,
             kind < kEg2DmKernelCount ? eg2_dm_kernel_target(kind) : 0);
    ok = run_command(cfg, kind, arg, cfg->max_iters) && ok;
    ++commands;
    LOG_INFO("EG2 DEBUG end 0x%08x 0x%08x", seq, ok ? 1u : 0u);
  }
  eg2_chk_step("commands", commands);
  ok = eg2_chk_eq("done word written over SBA", magic, done_) && ok;
  return ok;
}
