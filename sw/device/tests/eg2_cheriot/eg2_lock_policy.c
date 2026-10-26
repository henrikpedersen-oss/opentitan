// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_lock_policy and, built with EG2_PERIPH_LOCK, chip_sw_cheriot_periph_lock (EG2
 * spec "Mode switch and handover"; REQ_SOC_08, REQ_SOC_03).
 *
 * Only the boot stage that owns the mode decision can lock it: once later software runs, the
 * switch is locked; registers the ePMP guarded in ePMP mode (ast, otp_ctrl, lc_ctrl,
 * sram_ctrl_*.regs, cheriot_mem_sys.regs, rv_core_ibex.cfg) are locked before the switch to
 * CHERIoT mode (RFC CHERIoT in OpenTitan p2 "end of the boot stage"; RFC CHERIoT Memory Subsystem
 * "Peripheral Device Considerations"). The test runs in CHERIoT mode, after the boot code (here the
 * test ROM and the framework's start-up code) has locked it.
 *
 * `n_attempts` attempts, `when` (right after the lock; after a sleep: a WFI the timer ends; after
 * a software reset of the chip, on the boot that follows), each `attempt`:
 *  - lock_policy: a CHERIOT_ENA write (MuBi4False or another encoding), a CHERIOT_LOCK write (any
 *    of the 16 encodings), the same over SBA (a bench step, `sba_wr32`). Check: the mode is
 *    unchanged (misa E set, I clear, after every attempt), no alert (the switch's error state
 *    raises fatal_hw_err; the status sweep fails on it).
 *  - periph_lock: every software lock (REGWEN) of those blocks (eg2_regmem's tables, from the
 *    hjson) reads locked; a write to a register it guards, from the core or over SBA, leaves the
 *    register unchanged. A lock that reads open is reported and the registers it guards are not
 *    written (a write would reconfigure the chip). cheriot_mem_sys.regs has no software lock
 *    (TBRE_REGWEN is the engine's busy flag), so REQ_SOC_08 cannot hold for it as written: it is
 *    reported, not counted as a failure (a spec question).
 *
 * Expected to fail (periph_lock) on the boot flow of 2026-10-24: neither the test ROM nor the EG2
 * start-up code locks those registers before the switch. `lock_first` (stimulus only, 0 in the
 * regression) has the test close every software lock itself first, standing in for a compliant
 * boot stage, so that the write attempts are checked while the finding is open.
 *
 * Without a bench (`model_gaps`) the SBA attempts are skipped.
 *
 * Built with the INJECT_FAIL define, the mode after the first attempt is expected to be RISC-V:
 * the run must fail with `MISMATCH misa`.
 */

#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/rv_timer_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem_tables.h"

EG2_TEST_CONFIG();

#define LOCK_POLICY_PARAMS(X) \
  X(attempt, 0, 0, 3)         \
  X(n_attempts, 0, 0, 32)     \
  X(when, 0, 0, 3)            \
  X(lock_first, 0, 0, 1)      \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(LOCK_POLICY_PARAMS);

#if defined(EG2_LOCK_POLICY_INJECT_FAIL) || defined(EG2_PERIPH_LOCK_INJECT_FAIL)
#define INJECT 1
#else
#define INJECT 0
#endif

enum {
  kMisaE = 1u << 4,
  kMisaI = 1u << 8,
  kWhenNow = 1,
  kWhenSleep = 2,
  kWhenReset = 3,
};

static eg2_tally_t tally;
static bool injected;

static void check_mode(uint32_t idx) {
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
  uint32_t exp = kMisaE;
  if (INJECT && !injected) {
    injected = true;
    exp = kMisaI;
  }
  eg2_check(&tally, "misa", idx, misa & (kMisaE | kMisaI), exp);
}

/** A WFI the timer ends (mstatus.MIE clear: no trap). */
static void sleep_once(void) {
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CFG0_REG_OFFSET, 1u << RV_TIMER_CFG0_STEP_OFFSET);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_LOWER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_TIMER_V_UPPER0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_UPPER0_0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET, 500);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 1);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 1);
  __asm__ volatile("csrs mie, %0" : : "r"(0x80u));
  __asm__ volatile("wfi");
  __asm__ volatile("csrc mie, %0" : : "r"(0x80u));
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_ENABLE0_REG_OFFSET, 0);
  eg2_dev_write32(kEg2DevRvTimer, RV_TIMER_INTR_STATE0_REG_OFFSET, 1);
}

#ifndef EG2_PERIPH_LOCK

static void attempt_mode(uint32_t kind, uint32_t idx, bool bench) {
  uint32_t cfg = TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR;
  uint32_t v = eg2_rand_range(0, 15);
  bool ena = (kind & 1) == 0;
  uint32_t off = ena ? RV_CORE_IBEX_CHERIOT_ENA_REG_OFFSET : RV_CORE_IBEX_CHERIOT_LOCK_REG_OFFSET;
  if (ena && v == 0x6) {
    v = 0x9;  // a request of the other mode: MuBi4False
  }
  if (kind >= 2) {
    if (!eg2_ms_bench("sba_wr32", cfg + off, v, bench)) {
      eg2_check(&tally, "bench step", idx, 0, 1);
    } else if (bench) {
      eg2_check(&tally, "sberror", idx, eg2_ms_bench_result(), 0);
    }
  } else {
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, off, v);
  }
  check_mode(idx);
}

#else  // EG2_PERIPH_LOCK

/** The blocks REQ_SOC_08 names. */
static bool named_block(uint32_t base) {
  switch (base) {
    case TOP_EARLGREY_AST_BASE_ADDR:
    case TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR:
    case TOP_EARLGREY_LC_CTRL_REGS_BASE_ADDR:
    case TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR:
    case TOP_EARLGREY_SRAM_CTRL_SEC_REGS_BASE_ADDR:
    case TOP_EARLGREY_SRAM_CTRL_RET_REGS_BASE_ADDR:
    case TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR:
    case TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR:
    case TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR:
      return true;
    default:
      return false;
  }
}

static uint32_t rd(uint32_t addr) { return eg2_ms_read32(addr); }

/** A software lock: a REGWEN software clears (rw0c), as reggen generates them. */
static bool sw_lock(const eg2_rm_reg_t *wen) {
  return kEg2RmMasks[wen->masks].rw0c != 0 || (wen->flags & kEg2RmLockable);
}

/** lock_first: every software lock of the blocks closed by the test, as a compliant boot stage
 * would. */
static void lock_all(void) {
  for (uint32_t bi = 0; bi < EG2_RM_BLOCK_COUNT; ++bi) {
    const eg2_rm_block_t *b = &kEg2RmBlocks[bi];
    if (!named_block(b->base)) {
      continue;
    }
    for (uint32_t i = 0; i < b->count; ++i) {
      const eg2_rm_reg_t *r = &kEg2RmRegs[b->first + i];
      if (r->regwen != 0xffff && sw_lock(&kEg2RmRegs[r->regwen])) {
        eg2_ms_write32(b->base + kEg2RmRegs[r->regwen].offset, 0);
      }
    }
  }
}

static void attempt_periph(uint32_t kind, uint32_t idx, bool bench) {
  for (uint32_t bi = 0; bi < EG2_RM_BLOCK_COUNT; ++bi) {
    const eg2_rm_block_t *b = &kEg2RmBlocks[bi];
    if (!named_block(b->base)) {
      continue;
    }
    bool any_lock = false;
    for (uint32_t i = 0; i < b->count; ++i) {
      const eg2_rm_reg_t *r = &kEg2RmRegs[b->first + i];
      if (r->regwen == 0xffff || (r->flags & kEg2RmNoRead)) {
        continue;  // no lock, or a read with a side effect
      }
      const eg2_rm_reg_t *wen = &kEg2RmRegs[r->regwen];
      if (!sw_lock(wen)) {
        continue;  // a hardware-driven enable, not a software lock
      }
      any_lock = true;
      uint32_t w = rd(b->base + wen->offset);
      bool open = (wen->flags & kEg2RmWenMubi) ? (w & 0xf) == 0x6 : (w & 1) == 1;
      ++tally.planned;
      if (!eg2_check(&tally, "lock reads locked", idx, open ? 0 : 1, 1)) {
        eg2_put_kv("  register", b->base + r->offset);
        eg2_put_kv(" its lock", b->base + wen->offset);
        eg2_puts("\r\n");
        ++tally.done;
        continue;
      }
      // Locked: a write must change nothing.
      const eg2_rm_masks_t *m = &kEg2RmMasks[r->masks];
      uint32_t addr = b->base + r->offset;
      uint32_t before = rd(addr);
      uint32_t v = before ^ m->rw;
      if (r->flags & kEg2RmShadowed) {
        eg2_ms_write32(addr, v);
        eg2_ms_write32(addr, v);
      } else if (kind >= 2) {
        if (eg2_ms_bench("sba_wr32", addr, v, bench) && bench) {
          eg2_check(&tally, "sberror", idx, eg2_ms_bench_result(), 0);
        }
      } else {
        eg2_ms_write32(addr, v);
      }
      eg2_check(&tally, "locked register unchanged", idx, rd(addr), before);
      ++tally.done;
    }
    if (!any_lock) {
      eg2_put_kv("EG2 no software lock in the block at", b->base);
      eg2_puts(" (REQ_SOC_08 cannot hold as written: spec question)\r\n");
    }
  }
  check_mode(idx);
}

#endif  // EG2_PERIPH_LOCK

bool eg2_test_main(void) {
  bool bench = EG2_PARAM(model_gaps) == 0;
  eg2_ms_rec_t *rec = eg2_ms_rec_boot();
  uint32_t when = EG2_PARAM(when) != 0 ? EG2_PARAM(when) : eg2_rand_range(1, 3);
  if (when == kWhenReset && rec->boot == 1) {
    eg2_puts("EG2 lock_policy: a software reset first\r\n");
    eg2_ms_reset(kEg2MsResetKindSw, bench);
    return false;  // no reset came
  }
  if (when == kWhenReset) {
    eg2_check(&tally, "reset reason software", 0, eg2_ms_reset_reason() & kEg2MsResetSw,
              kEg2MsResetSw);
  }
#ifdef EG2_PERIPH_LOCK
  if (EG2_PARAM(lock_first)) {
    eg2_puts("EG2 periph_lock: the test locks the registers itself (lock_first)\r\n");
    lock_all();
  }
#endif
  uint32_t n = EG2_PARAM(n_attempts) != 0 ? EG2_PARAM(n_attempts) : eg2_rand_range(1, 32);
  eg2_put_kv("EG2 lock_policy when", when);
  eg2_put_kv(" n_attempts", n);
  eg2_puts("\r\n");
  for (uint32_t i = 0; i < n; ++i) {
    if (when == kWhenSleep) {
      sleep_once();
    }
    // attempt: 0 draws; 1 the core, 2 SBA; for lock_policy each draw also picks ENA or LOCK.
    uint32_t where = EG2_PARAM(attempt) != 0 ? EG2_PARAM(attempt) : eg2_rand_range(1, 2);
    if (where == 3) {
      where = 1;
    }
#ifdef EG2_PERIPH_LOCK
    attempt_periph(where == 2 ? 2 : 0, i, bench);
#else
    ++tally.planned;
    attempt_mode((where == 2 ? 2 : 0) | eg2_pick(2), i, bench);
    ++tally.done;
#endif
  }
  eg2_tally_print("lock_policy", &tally);
  return eg2_tally_ok(&tally);
}
