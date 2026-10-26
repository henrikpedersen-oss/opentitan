// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_mem_sys_e2e[_cheriot] (EG2 spec "Memory subsystem in RISC-V mode", "Twins of
 * the RISC-V tests"; REQ_CMS_19, and REQ_CMS_16 in the twin).
 *
 * Firmware runs end to end in both modes, with the block tests' scenarios ported to the chip: tag
 * store and load, sub-word tag clear, sweep, access check (RFC CHERIoT Memory Subsystem
 * "Verification Strategy"; cheriot_mem_sys theory_of_operation.md).
 *
 * `n_scenarios` scenarios drawn by `w_scenario_<s>`, each with its own self-check:
 *  - store_load: CHERIoT: CSC and CLC in main SRAM, sec SRAM and RRAM (programmed, then a verified
 *    CSC), tag and words kept. RISC-V: words stored and loaded through cheriot_mem_sys (which
 *    passes them unchanged, "Legacy Mode Transparency") at each width.
 *  - subword_clear: CHERIoT: a capability stored, then a byte, half or word store at each offset
 *    of its granule: tag 0, the store's bytes in place. RISC-V: the same stores, the bytes in place.
 *  - sweep: CHERIoT: a slot based on a marked granule and one not, swept: the first untagged in
 *    memory (seen after the bitmap is cleared), the second tagged, the epoch up by 2. RISC-V: the
 *    engine refuses a start outside CHERIoT mode (TBRE_STATUS.start_err, epoch and busy
 *    unchanged).
 *  - access_check: CHERIoT: the bitmap window answers reads of every width (a sub-word load is a
 *    full-word Get on the bus, F34) and full-word writes, and errors on half-word and byte
 *    writes and past its end. RISC-V: the window errors on every access ("returns
 *    a TL-UL error while the system is in ePMP mode").
 *  - rekey (CHERIoT twin, `n_rekey_caps` 0-64): capabilities in sec SRAM, then sram_ctrl_sec INIT
 *    after a new key: every one reloads untagged (REQ_CMS_16; expected to fail on the RTL of
 *    2026-10-24, see chip_sw_cheriot_sram_rekey_tag; rtl_tickets F50).
 *
 * The SystemC model has no revoking engine, answers the bitmap window at any width and in RISC-V
 * mode, and keeps tags through INIT (dv_todo G2, eg2_mpact_cpu.h): with `model_gaps` those checks
 * are skipped.
 *
 * Built with EG2_MEM_SYS_E2E_INJECT_FAIL, the first store_load expects a wrong word back: the run
 * must fail with `MISMATCH store_load`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

#define MEM_SYS_E2E_PARAMS(X)         \
  X(n_scenarios, 0, 0, 32)            \
  X(w_scenario_store_load, 1, 0, 100) \
  X(w_scenario_subword_clear, 1, 0, 100) \
  X(w_scenario_sweep, 1, 0, 100)      \
  X(w_scenario_access_check, 1, 0, 100) \
  X(w_scenario_rekey, 1, 0, 100)      \
  X(n_rekey_caps, 65, 0, 65)          \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(MEM_SYS_E2E_PARAMS);

enum {
  kScStoreLoad = 0,
  kScSubwordClear,
  kScSweep,
  kScAccessCheck,
  kScRekey,
  kScCount,
};

static eg2_tally_t tally;
#ifdef EG2_MEM_SYS_E2E_INJECT_FAIL
static bool injected;
#endif

static bool gaps(void) { return EG2_PARAM(model_gaps) != 0; }

static uint32_t bad_value(uint32_t v) {
#ifdef EG2_MEM_SYS_E2E_INJECT_FAIL
  if (!injected) {
    injected = true;
    return v ^ 1;
  }
#endif
  return v;
}

static void store_load(uint32_t idx) {
#ifdef __CHERIOT__
  static const uint32_t kWhere[3] = {0, 1, 2};
  for (uint32_t k = 0; k < 3; ++k) {
    uint32_t a;
    void *c = eg2_ms_object_cap(kEg2ArenaA + 8 * eg2_pick(64), 8, false);
    if (kWhere[k] == 0) {
      a = eg2_main_arena() + 8 * eg2_pick(kEg2MainArenaSize / 8);
    } else if (kWhere[k] == 1) {
      a = kEg2ArenaB + 8 * eg2_pick(kEg2ArenaSize / 8);
    } else {
      a = kEg2MsRramArea + 16 * eg2_pick(kEg2MsRramAreaSize / 16);
      uint32_t w[4] = {eg2_ms_cap_addr(c), eg2_ms_cap_high(c), 0, 0};
      uint32_t err;
      if (!eg2_ms_rram_program(a, w, 4, &err)) {
        eg2_check(&tally, "store_load rram program", idx, err, 0);
        continue;
      }
    }
    eg2_ms_csc(a, c);
    void *got = eg2_ms_clc(a);
    eg2_check(&tally, "store_load tag", idx, eg2_ms_cap_tag(got), 1);
    eg2_check(&tally, "store_load", idx, eg2_ms_cap_addr(got), bad_value(eg2_ms_cap_addr(c)));
    eg2_check(&tally, "store_load hi", idx, eg2_ms_cap_high(got), eg2_ms_cap_high(c));
  }
#else
  for (uint32_t k = 0; k < 2; ++k) {
    uint32_t a = k == 0 ? eg2_main_arena() + 4 * eg2_pick(kEg2MainArenaSize / 4)
                        : kEg2ArenaB + 4 * eg2_pick(kEg2ArenaSize / 4);
    uint32_t v = eg2_rand();
    eg2_ms_write32(a, v);
    eg2_check(&tally, "store_load", idx, eg2_ms_read32(a), bad_value(v));
    volatile uint8_t *b = eg2_mem(a, 4);
    b[1] = 0x5a;
    eg2_check(&tally, "store_load byte", idx, eg2_ms_read32(a), (v & ~0xff00u) | 0x5a00u);
  }
#endif
}

static void subword_clear(uint32_t idx) {
  uint32_t g = kEg2ArenaB + 8 * eg2_pick(kEg2ArenaSize / 8);
  for (uint32_t width = 1; width <= 4; width *= 2) {
    for (uint32_t off = 0; off < 8; off += width) {
#ifdef __CHERIOT__
      eg2_ms_csc(g, eg2_ms_object_cap(kEg2ArenaA, 8, false));
#else
      eg2_ms_write32(g, 0x01234567u);
      eg2_ms_write32(g + 4, 0x89abcdefu);
#endif
      uint32_t lo = eg2_ms_read32(g), hi = eg2_ms_read32(g + 4);
      uint32_t v = eg2_rand();
      if (width == 1) {
        *(volatile uint8_t *)eg2_mem(g + off, 1) = (uint8_t)v;
      } else if (width == 2) {
        *(volatile uint16_t *)eg2_mem(g + off, 2) = (uint16_t)v;
      } else {
        *(volatile uint32_t *)eg2_mem(g + off, 4) = v;
      }
      // The expected words: the stored bytes in place.
      uint32_t mask = (width == 4 ? 0xffffffffu : ((1u << (8 * width)) - 1)) << (8 * (off & 3));
      uint32_t vv = v << (8 * (off & 3));
      if (off < 4) {
        lo = (lo & ~mask) | (vv & mask);
      } else {
        hi = (hi & ~mask) | (vv & mask);
      }
      eg2_check(&tally, "subword lo", idx, eg2_ms_read32(g), lo);
      eg2_check(&tally, "subword hi", idx, eg2_ms_read32(g + 4), hi);
#ifdef __CHERIOT__
      eg2_check(&tally, "subword tag", idx, eg2_ms_tag(g), 0);
#endif
    }
  }
}

static void sweep(uint32_t idx) {
#ifdef __CHERIOT__
  uint32_t s = kEg2ArenaB + 16 * eg2_pick(kEg2ArenaSize / 16 - 1);
  uint32_t obj = kEg2ArenaA + 8 * eg2_pick(kEg2ArenaSize / 8 - 1);
  eg2_ms_csc(s, eg2_ms_object_cap(obj, 8, false));
  eg2_ms_csc(s + 8, eg2_ms_object_cap(obj + 8, 8, false));
  eg2_ms_revbm_mark(obj, true);
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(s, 2, &sw);
  eg2_ms_tbre_wait(&sw, 100000);
  eg2_ms_revbm_mark(obj, false);
  eg2_ms_tbre_clear();
  eg2_check(&tally, "sweep started", idx, sw.started ? 1 : 0, 1);
  eg2_check(&tally, "sweep epoch", idx, sw.epoch_after, sw.epoch_before + 2);
  if (!gaps()) {
    eg2_check(&tally, "sweep revoked tag", idx, eg2_ms_tag(s), 0);
  }
  eg2_check(&tally, "sweep kept tag", idx, eg2_ms_tag(s + 8), 1);
#else
  eg2_ms_tbre_clear();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kEg2ArenaB, 4, &sw);
  eg2_check(&tally, "sweep start_err", idx,
            (sw.status >> CHERIOT_MEM_SYS_TBRE_STATUS_START_ERR_BIT) & 1, 1);
  eg2_check(&tally, "sweep busy", idx, sw.status & 1, 0);
  eg2_check(&tally, "sweep epoch", idx, eg2_ms_tbre_epoch(), sw.epoch_before);
  eg2_ms_tbre_clear();
#endif
}

static void access_check(uint32_t idx) {
  uint32_t w = eg2_pick(kEg2MsRevbmSize / 4);
  uint32_t a = kEg2MsMetaBase + 4 * w;
  static const uint8_t kWidths[3] = {4, 2, 1};
  for (uint32_t k = 0; k < 3; ++k) {
    uint32_t width = kWidths[k];
    for (uint32_t st = 0; st < 2; ++st) {
#ifdef __CHERIOT__
      // A sub-word load reaches the window as a full-word Get (the core's tlul_adapter_host
      // enables every lane of a read, as TL-UL requires), so only sub-word stores
      // (PutPartialData) are refused (F34).
      bool ok = width == 4 || !st;
      uint32_t value = eg2_ms_revbm_read(w);
#else
      bool ok = false;
      uint32_t value = 0;
#endif
      void *stub = st ? (width == 4 ? eg2_stub_sw : width == 2 ? eg2_stub_sh : eg2_stub_sb)
                      : (width == 4 ? eg2_stub_lw : width == 2 ? eg2_stub_lhu : eg2_stub_lbu);
      bool trapped = eg2_probe_call(eg2_mem(a, width), (void *)(uintptr_t)value, NULL, stub);
      if (ok || !gaps()) {
        eg2_check(&tally, "access_check mcause", idx, trapped ? eg2_trap_rec.mcause : 0,
                  ok ? 0 : st ? (uint32_t)kEg2McauseStoreFault : (uint32_t)kEg2McauseLoadFault);
      }
    }
  }
  // Past the window's end: the tag region, never reachable.
  bool trapped = eg2_probe_call(eg2_mem(kEg2MsMetaBase + kEg2MsRevbmSize, 4), NULL, NULL,
                                eg2_stub_lw);
  if (!gaps()) {
    eg2_check(&tally, "access_check past the window", idx,
              trapped ? eg2_trap_rec.mcause : 0, kEg2McauseLoadFault);
  }
}

static void rekey(uint32_t idx) {
#ifdef __CHERIOT__
  uint32_t n = EG2_PARAM(n_rekey_caps) == 65 ? eg2_rand_range(0, 64) : EG2_PARAM(n_rekey_caps);
  for (uint32_t i = 0; i < n; ++i) {
    eg2_ms_csc(kEg2ArenaB + 8 * i, eg2_ms_object_cap(kEg2ArenaA + 8 * i, 8, false));
  }
  bool done = eg2_ms_sram_ctrl(kEg2DevSramCtrlSecRegs, true, true);
  eg2_check(&tally, "rekey INIT done", idx, done ? 1 : 0, 1);
  if (!gaps()) {
    for (uint32_t i = 0; i < n; ++i) {
      eg2_check(&tally, "rekey tag", idx, eg2_ms_tag(kEg2ArenaB + 8 * i), 0);
    }
  }
  // sec SRAM's contents are random after INIT: zero them again for the other scenarios.
  (void)eg2_sec_sram_init();
#else
  (void)idx;
#endif
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
#ifdef __CHERIOT__
  if (!eg2_ms_rram_setup()) {
    return false;
  }
  eg2_ms_revbm_clear();
#endif
  uint32_t n = EG2_PARAM(n_scenarios) != 0 ? EG2_PARAM(n_scenarios) : eg2_rand_range(4, 32);
  eg2_family_t sc = {.n = kScCount, .plain_mask = 0, .boosted = kScCount};
  sc.w[kScStoreLoad] = EG2_PARAM(w_scenario_store_load);
  sc.w[kScSubwordClear] = EG2_PARAM(w_scenario_subword_clear);
  sc.w[kScSweep] = EG2_PARAM(w_scenario_sweep);
  sc.w[kScAccessCheck] = EG2_PARAM(w_scenario_access_check);
  sc.w[kScRekey] = EG2_PARAM(w_scenario_rekey);
#ifndef __CHERIOT__
  sc.w[kScRekey] = 0;  // the twin's scenario
#endif
  eg2_family_print("scenario", &sc);
  eg2_probe_begin();
  // Each scenario once first, then the draws.
  for (uint32_t i = 0; i < kScCount + n; ++i) {
    uint32_t s = i < kScCount ? i : eg2_family_pick(&sc);
    ++tally.planned;
    switch (s) {
      case kScStoreLoad:
        store_load(i);
        break;
      case kScSubwordClear:
        subword_clear(i);
        break;
      case kScSweep:
        sweep(i);
        break;
      case kScAccessCheck:
        access_check(i);
        break;
      default:
        rekey(i);
        break;
    }
    ++tally.done;
  }
  eg2_probe_end();
  eg2_tally_print("mem_sys_e2e", &tally);
  return eg2_tally_ok(&tally);
}
