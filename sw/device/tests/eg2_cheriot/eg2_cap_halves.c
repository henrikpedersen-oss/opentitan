// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_cheriot_cap_halves (EG2 spec "Revocation", "Corner cases"; REQ_CMS_10, REQ_CDM_09): the
 * stimulus of the chip assertion (tb/eg2_cap_halves_chk.sv, on with +eg2_cap_halves=1) that a
 * capability's two halves arrive in order and back to back at cheriot_mem_sys's core port and at
 * its TBRE port, the condition in ibex_trvk's and cheriot_trvk_tlul's headers that the formal
 * proofs assume.
 *
 * Each of `n_runs` runs: 64-256 slots in sec SRAM's arena B, each a distinct tagged capability (to
 * one of 32 objects in arena A, at a distinct address). A copy kernel (eg2_cap_halves.S: CLC of each
 * slot, CSC to a copy in the work area, back to back, 2-4 rounds) runs from main SRAM, sec SRAM (the
 * same SRAM as the slots) or RRAM (`code_region`), while:
 *  - the bench rewrites random words of the odd slots over SBA with the data they hold (sba_hit:
 *    read, then write the same word), so their granules are hit while the data stays;
 *  - the TBRE sweeps the slots (nothing is marked: it reads every capability and clears none);
 *  - the bench's response delays hold the main, sec and meta SRAM ports (`delays`).
 * Checks:
 *  - every copy holds both words of its slot's capability: no CLC returned a capability whose
 *    words come from two different capabilities. The copies of even slots are tagged. The odd
 *    slots' tags are not checked: an SBA write keeps the tag by cheriot_mem_sys "System Bus
 *    Access" and clears it by REQ_CMS_16 (F23), so either is logged, not judged;
 *  - the sweeps end without an error, the epoch +2 each;
 *  - the bench's assertion counts (cap_halves_count): capability loads checked on the core port
 *    (at least the copies made) and on the TBRE port (at least the slots swept), and no failure.
 *    The assertion itself reports each failure as `EG2_CAP_HALVES` (uvm_error).
 * Not checked (dv_todo.md): the TBRE's decode against the reference model (the spec row's "never
 * decodes a base from a mix of old and new words"): with no core write to a slot during the sweep
 * no mix can arise here; the core-write race is chip_sw_cheriot_tbre_core_write_snoop's.
 *
 * `model_gaps` (eg2sim): no bench: the SBA hits, the delays and the counts are skipped.
 *
 * Built with EG2_CAP_HALVES_INJECT_FAIL, the first copy's address is expected off by 8: the run
 * must fail with `MISMATCH copy address #0`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"
#include "hw/top/sram_ctrl_regs.h"

EG2_TEST_CONFIG();

#define CAP_HALVES_PARAMS(X) \
  X(n_runs, 0, 0, 32)        \
  X(code_region, 3, 0, 3)    \
  X(delays, 2, 0, 2)         \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(CAP_HALVES_PARAMS);

// eg2_cap_halves.S.
extern void eg2_ch_copy(void *slots, void *copies, uint32_t n, uint32_t rounds);
extern char eg2_ch_copy_end[];
typedef void (*ch_copy_t)(void *slots, void *copies, uint32_t n, uint32_t rounds);

enum {
  kObjects = 32,
  kObj = kEg2ArenaA,
  kSlots = kEg2ArenaB,
  kMaxSlots = 256,
  kCopies = kEg2WorkBase,
  kHi = kCopies + 8 * kMaxSlots,
  kAddr = kHi + 4 * kMaxSlots,
  kSecCode = kEg2WorkBase + 0x2000,
  kRegionRram = 0,
  kRegionMain = 1,
  kRegionSec = 2,
  kMuBi4True = 0x6,
};

static eg2_tally_t tally;
static const char *const kRegionName[3] = {"RRAM", "main SRAM", "sec SRAM"};

static ch_copy_t place(uint32_t region, uint32_t base) {
  if (region == kRegionRram) {
    return eg2_ch_copy;
  }
  uint32_t from = EG2_ADDR(eg2_ch_copy), size = EG2_ADDR(eg2_ch_copy_end) - from;
  for (uint32_t i = 0; i < size; i += 4) {
    eg2_ms_write32(base + i, eg2_ms_read32(from + i));
  }
  __asm__ volatile("fence.i" ::: "memory");
  void *entry;
  __asm__ volatile("auipcc %0, 0" : "=C"(entry));
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(entry) : "r"(base));
  return (ch_copy_t)entry;
}

static uint32_t count(uint32_t which, bool bench, uint32_t *fails) {
  *fails = 0;
  if (!eg2_ms_bench("cap_halves_count", which, 0, bench) || !bench) {
    return 0;
  }
  uint32_t r = eg2_ms_bench_result();
  *fails = r >> 20;
  return r & 0xfffff;
}

static void one_run(uint32_t run, bool bench, uint32_t main_arena) {
  ++tally.planned;
  uint32_t n = eg2_rand_range(64, kMaxSlots);
  uint32_t rounds = eg2_rand_range(2, 4);
  uint32_t region = EG2_PARAM(code_region) != 3 ? EG2_PARAM(code_region) : eg2_pick(3);
  bool delays = EG2_PARAM(delays) == 2 ? eg2_pick(4) != 0 : EG2_PARAM(delays) != 0;
  for (uint32_t i = 0; i < n; ++i) {
    // Distinct capabilities: object i % 32, address in it by i / 32.
    uint32_t o = kObj + 8 * (i % kObjects);
    void *c = eg2_ms_object_cap(o, 8, false);
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(c) : "r"(o + (i / kObjects) % 8));
    eg2_ms_csc(kSlots + 8 * i, c);
    eg2_cap_t v = eg2_hw_read(eg2_ms_clc(kSlots + 8 * i));
    eg2_ms_write32(kHi + 4 * i, v.hi);
    eg2_ms_write32(kAddr + 4 * i, v.addr);
    eg2_ms_write32(kCopies + 8 * i, 0);
    eg2_ms_write32(kCopies + 8 * i + 4, 0);
  }
  ch_copy_t k = place(region, region == kRegionMain ? main_arena : kSecCode);
  uint32_t fails0, tfails0;
  uint32_t core0 = count(0, bench, &fails0), tbre0 = count(1, bench, &tfails0);

  if (delays) {
    (void)eg2_rv_delays(true, bench);
  }
  (void)eg2_ms_bench("sba_hit_begin", kSlots, kSlots + 8 * n, bench);
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kSlots, n, &sw);
  k(eg2_mem(kSlots, 8 * n), eg2_mem(kCopies, 8 * n), n, rounds);
  eg2_ms_sweep_t done = {0};
  eg2_ms_tbre_wait(&done, 10000000);
  (void)eg2_ms_bench("sba_hit_end", 0, 0, bench);
  uint32_t hits = bench ? eg2_ms_bench_result() : 0;
  if (delays) {
    (void)eg2_rv_delays(false, bench);
  }
  eg2_check(&tally, "sweep started", run, sw.started, 1);
  eg2_check(&tally, "sweep ended", run, done.done, 1);
  eg2_check(&tally, "TBRE_STATUS", run, done.status, 0);
  eg2_check(&tally, "epoch", run, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();

  uint32_t odd_untagged = 0;
  for (uint32_t i = 0; i < n; ++i) {
    eg2_cap_t c = eg2_hw_read(eg2_ms_clc(kCopies + 8 * i));
    uint32_t exp_addr = eg2_ms_read32(kAddr + 4 * i);
#ifdef EG2_CAP_HALVES_INJECT_FAIL
    if (run == 0 && i == 0) {
      exp_addr += 8;
    }
#endif
    if (i % 2 == 0) {
      eg2_check(&tally, "copy tag", run * kMaxSlots + i, c.tag ? 1u : 0u, 1u);
    } else if (!c.tag) {
      ++odd_untagged;
    }
    eg2_check(&tally, "copy address", run * kMaxSlots + i, c.addr, exp_addr);
    eg2_check(&tally, "copy high word", run * kMaxSlots + i, c.hi, eg2_ms_read32(kHi + 4 * i));
  }
  if (bench) {
    uint32_t fails, tfails;
    uint32_t core = count(0, bench, &fails) - core0, tbre = count(1, bench, &tfails) - tbre0;
    eg2_check(&tally, "core port: capability loads checked", run, core >= n * rounds ? 1u : 0u,
              1u);
    eg2_check(&tally, "core port: assertion failures", run, fails - fails0, 0);
    eg2_check(&tally, "TBRE port: capability loads checked", run, tbre >= n ? 1u : 0u, 1u);
    eg2_check(&tally, "TBRE port: assertion failures", run, tfails - tfails0, 0);
    eg2_put_kv("EG2 cap_halves checked: core", core);
    eg2_put_kv(" tbre", tbre);
    eg2_puts("\r\n");
  }
  eg2_put_kv("EG2 cap_halves run", run);
  eg2_puts(" ");
  eg2_puts(kRegionName[region]);
  eg2_put_kv(" slots", n);
  eg2_put_kv(" rounds", rounds);
  eg2_put_kv(" delays", delays);
  eg2_put_kv(" SBA hits", hits);
  eg2_put_kv(" odd slots copied untagged", odd_untagged);
  eg2_puts("\r\n");
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n_runs = EG2_PARAM(n_runs) != 0 ? EG2_PARAM(n_runs) : eg2_rand_range(4, 12);
  eg2_put_kv("EG2 cap_halves runs", n_runs);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMuBi4True);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMuBi4True);
  uint32_t main_arena = eg2_main_arena();
  for (uint32_t r = 0; r < n_runs; ++r) {
    one_run(r, !gaps, main_arena);
  }
  eg2_tally_print("cap_halves", &tally);
  return eg2_tally_ok(&tally);
}
