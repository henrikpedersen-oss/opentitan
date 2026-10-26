// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_fetch_backpressure (EG2 spec "Revocation", "Corner cases"; REQ_CIB_06,
 * REQ_CMS_11).
 *
 * Instruction fetch competing with capability traffic. A kernel (eg2_fetch_backpressure.S) with a
 * CLC followed straight away by a taken branch, by a jump and by a CJALR (CHERIoT jumps wait for
 * outstanding memory accesses), a CSC then a CLC of the same address, and compressed and 32-bit
 * instructions mixed so that 32-bit ones straddle fetch words. Each of `n_runs` runs draws:
 *  - where the code is (`code_region`): RRAM (in place), main SRAM (the test's main arena) or sec
 *    SRAM (the work area), sharing sec SRAM with the CLC/CSC data in arena B and the objects in
 *    arena A; code in SRAM is fetched over the same SRAM ports as the data (sram_ctrl EXEC set);
 *  - the instruction cache on or off (`icache`; cpuctrlsts.icache_enable, Ibex CSR documentation);
 *  - which of the 32 objects (one bitmap word) are marked while the kernel runs.
 * Around it (`delays`, `contention`): the bench's response delays on the main, sec and meta SRAM
 * ports (so a fetch miss meets a held load/store response and the bitmap reads are slow), SBA reads
 * of sec SRAM, and the TBRE sweeping arena B and the code.
 *
 * Checks:
 *  - each CLC's tag as the load filter's expression gives (eg2_revoke.h): tag 0 exactly for a slot
 *    whose object was marked; the CSC/CLC pair returns what the third CLC loaded;
 *  - progress within a bound: the kernel's mcycle count at most `iters` x 4000 cycles;
 *  - the sweeps end without an error;
 *  - the scoreboard's instruction check on every retired instruction (bench, en_eg2_cosim): a
 *    dropped or repeated fetch is a wrong instruction.
 * Not checked (dv_todo.md): code in ROM (the test ROM's contents are fixed; no kernel can be placed
 * there); the fetch request count on the instruction port (Ibex's prefetch buffer fetches ahead
 * and discards on a jump, so no document states the count; the scoreboard checks what retires).
 *
 * `model_gaps` (eg2sim): no load filter and no bench: nothing is marked (every tag 1 is checked),
 * the bench steps are skipped.
 *
 * Built with EG2_FETCH_BACKPRESSURE_INJECT_FAIL, the first iteration's tag mask is expected with
 * bit 3 flipped: the run must fail with `MISMATCH tags of iteration #0`.
 */

#include "hw/top/sram_ctrl_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define FETCH_BACKPRESSURE_PARAMS(X) \
  X(n_runs, 0, 0, 32)                \
  X(iters, 0, 0, 128)                \
  X(code_region, 3, 0, 3)            \
  X(icache, 2, 0, 2)                 \
  X(delays, 2, 0, 2)                 \
  X(contention, 4, 0, 4)             \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(FETCH_BACKPRESSURE_PARAMS);

// eg2_fetch_backpressure.S.
extern void eg2_fb_kernel(void *slots, uint32_t *out, uint32_t iters, void *dst, uint32_t *mark,
                          uint32_t mask);
extern char eg2_fb_kernel_end[];
typedef void (*fb_kernel_t)(void *slots, uint32_t *out, uint32_t iters, void *dst,
                            uint32_t *mark, uint32_t mask);

enum {
  kObjects = 32,
  kObj = kEg2ArenaA + 0x100,  // 32 granules: one bitmap word
  kSlots = kEg2ArenaB,        // 3 per iteration
  kMaxIters = 128,
  kDst = kEg2ArenaB + 3 * 8 * kMaxIters,
  kOut = kEg2WorkBase,
  kSecCode = kEg2WorkBase + 0x1000,
  kRegionRram = 0,
  kRegionMain = 1,
  kRegionSec = 2,
  kMuBi4True = 0x6,
};

static eg2_tally_t tally;
static const char *const kRegionName[3] = {"RRAM", "main SRAM", "sec SRAM"};

static uint32_t rd(uint32_t a) { return eg2_ms_read32(a); }

/** The kernel at `base` (copied there unless it is RRAM). */
static fb_kernel_t place(uint32_t region, uint32_t base) {
  if (region == kRegionRram) {
    return eg2_fb_kernel;
  }
  uint32_t from = EG2_ADDR(eg2_fb_kernel), size = EG2_ADDR(eg2_fb_kernel_end) - from;
  for (uint32_t i = 0; i < size; i += 4) {
    eg2_ms_write32(base + i, eg2_ms_read32(from + i));
  }
  __asm__ volatile("fence.i" ::: "memory");
  void *entry;
  __asm__ volatile("auipcc %0, 0" : "=C"(entry));
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(entry) : "r"(base));
  return (fb_kernel_t)entry;
}

static void one_run(uint32_t run, bool gaps, uint32_t main_arena) {
  ++tally.planned;
  bool bench = !gaps;
  uint32_t iters = EG2_PARAM(iters) != 0 ? EG2_PARAM(iters) : eg2_rand_range(16, kMaxIters);
  uint32_t region = EG2_PARAM(code_region) != 3 ? EG2_PARAM(code_region) : eg2_pick(3);
  uint32_t icache = EG2_PARAM(icache) != 2 ? EG2_PARAM(icache) : eg2_pick(2);
  bool delays = EG2_PARAM(delays) == 2 ? eg2_pick(4) != 0 : EG2_PARAM(delays) != 0;
  uint32_t contention = EG2_PARAM(contention) == 4 ? eg2_rand_range(0, 3) : EG2_PARAM(contention);
  uint32_t mask = gaps ? 0 : eg2_rand();
  uint32_t base = region == kRegionMain ? main_arena : kSecCode;

  // Slots: three per iteration, each a tagged capability to one of the 32 objects.
  for (uint32_t s = 0; s < 3 * iters; ++s) {
    uint32_t o = eg2_pick(kObjects);
    eg2_ms_csc(kSlots + 8 * s, eg2_ms_object_cap(kObj + 8 * o, 8, false));
    eg2_ms_write32(kOut + 4 * kMaxIters + 4 * s, o);
  }
  uint32_t ctl;
  __asm__ volatile("csrr %0, 0x7c0" : "=r"(ctl));
  ctl = (ctl & ~1u) | icache;
  __asm__ volatile("csrw 0x7c0, %0" : : "r"(ctl));
  fb_kernel_t k = place(region, base);
  uint32_t w, b;
  (void)eg2_ms_revbm_bit(kObj, &w, &b);

  if (delays) {
    (void)eg2_rv_delays(true, bench);
  }
  if (contention & 2) {
    (void)eg2_rv_sba_noise(true, bench);
  }
  uint32_t epoch0 = eg2_ms_tbre_epoch(), sweeps = 0;
  if (contention & 1) {
    // Arena B (the slots) and, for code in sec SRAM, the code. While the kernel's marks are set
    // the sweep may untag marked slots in memory: their CLCs return tag 0 either way, and the
    // next run stores the slots again.
    eg2_ms_sweep_t sw;
    uint32_t lo = region == kRegionSec ? kSecCode : kSlots;
    eg2_ms_tbre_start(lo, (kEg2ArenaB + kEg2ArenaSize - lo) / 8, &sw);
    sweeps += sw.started ? 1 : 0;
  }
  uint32_t t0, t1;
  __asm__ volatile("csrr %0, mcycle" : "=r"(t0));
  k(eg2_mem(kSlots, 24 * iters), eg2_mem(kOut, 4 * iters), iters, eg2_mem(kDst, 8),
    eg2_mem(kEg2MsMetaBase + 4 * w, 4), mask);
  __asm__ volatile("csrr %0, mcycle" : "=r"(t1));
  if (contention & 2) {
    (void)eg2_rv_sba_noise(false, bench);
  }
  if (delays) {
    (void)eg2_rv_delays(false, bench);
  }
  if (contention & 1) {
    eg2_ms_sweep_t sw = {0};
    eg2_ms_tbre_wait(&sw, 10000000);
    eg2_check(&tally, "sweep ended", run, sw.done, 1);
    eg2_check(&tally, "TBRE_STATUS", run, sw.status, 0);
    eg2_check(&tally, "epoch", run, eg2_ms_tbre_epoch(), epoch0 + 2 * sweeps);
    eg2_ms_tbre_clear();
  }
  eg2_check(&tally, "bitmap word after the kernel", run, eg2_ms_revbm_read(w), 0);

  for (uint32_t i = 0; i < iters; ++i) {
    uint32_t exp = 0;
    for (uint32_t j = 0; j < 3; ++j) {
      uint32_t o = rd(kOut + 4 * kMaxIters + 4 * (3 * i + j));
      if (!((mask >> o) & 1)) {
        exp |= 1u << j;
      }
    }
    exp |= ((exp >> 2) & 1) << 3;
#ifdef EG2_FETCH_BACKPRESSURE_INJECT_FAIL
    if (run == 0 && i == 0) {
      exp ^= 8;
    }
#endif
    eg2_check(&tally, "tags of iteration", run * kMaxIters + i, rd(kOut + 4 * i), exp);
  }
  eg2_check(&tally, "progress within the bound", run, (t1 - t0) <= iters * 4000 ? 1u : 0u, 1u);
  eg2_put_kv("EG2 fetch_backpressure run", run);
  eg2_puts(" ");
  eg2_puts(kRegionName[region]);
  eg2_put_kv(" icache", icache);
  eg2_put_kv(" iters", iters);
  eg2_put_kv(" delays", delays);
  eg2_put_kv(" contention", contention);
  eg2_put_kv(" mask", mask);
  eg2_put_kv(" cycles", t1 - t0);
  eg2_puts("\r\n");
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t main_arena = eg2_main_arena();
  uint32_t n_runs = EG2_PARAM(n_runs) != 0 ? EG2_PARAM(n_runs) : eg2_rand_range(4, 12);
  eg2_put_kv("EG2 fetch_backpressure runs", n_runs);
  eg2_puts("\r\n");
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  eg2_dev_write32(kEg2DevSramCtrlMainRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMuBi4True);
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMuBi4True);
  for (uint32_t i = 0; i < kObjects; ++i) {
    eg2_ms_write32(kObj + 8 * i, 0xfe000000u + i);
    eg2_ms_write32(kObj + 8 * i + 4, 0xfe100000u + i);
  }
  for (uint32_t r = 0; r < n_runs; ++r) {
    one_run(r, gaps, main_arena);
  }
  eg2_tally_print("fetch_backpressure", &tally);
  return eg2_tally_ok(&tally);
}
