// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_progress (EG2 spec "Revocation", "Corner cases"; REQ_CIB_06, REQ_CMS_12;
 * ports the RTOS test `revocation_sweep_progress`).
 *
 * The core keeps running while the TBRE sweeps. A loop (eg2_tbre_progress.S) of word
 * read-modify-writes over a heap buffer of `n_words` words in sec SRAM's arena A, one pass at a
 * time, timed with mcycle. For the code in RRAM and in sec SRAM (copied there; sram_ctrl EXEC set):
 *  1. idle: `idle_passes` passes with no sweep; the average cycles per pass;
 *  2. through a sweep: a sweep of the whole of main and sec SRAM is started, and passes run until
 *     TBRE_STATUS.busy clears (at most 100000 passes).
 * Checks:
 *  - the sweep completes while the core runs: at least one pass ran while it was busy, it ended,
 *    TBRE_STATUS clean and the epoch +2 (nothing is marked);
 *  - the passes during the sweep average no more than 4 times the idle average;
 *  - every word of the buffer ends at its start value plus the passes made.
 *
 * No model gap: the SystemC model runs every check (its TBRE sweeps over many passes).
 *
 * Built with EG2_TBRE_PROGRESS_INJECT_FAIL, the first word is expected one higher: the run must
 * fail with `MISMATCH buffer word #0`.
 */

#include "hw/top/sram_ctrl_regs.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define TBRE_PROGRESS_PARAMS(X) \
  X(n_words, 0, 0, 256)         \
  X(idle_passes, 0, 0, 64)
EG2_TEST_PARAMS(TBRE_PROGRESS_PARAMS);

// eg2_tbre_progress.S.
extern uint32_t eg2_tp_rmw(uint32_t *buf, uint32_t n, uint32_t rounds);
extern char eg2_tp_rmw_end[];
typedef uint32_t (*tp_rmw_t)(uint32_t *buf, uint32_t n, uint32_t rounds);

enum {
  kBuf = kEg2ArenaA,
  kSecCode = kEg2WorkBase + 0x1000,
  kMaxPasses = 100000,
  kBusy = 1u,
  kMuBi4True = 0x6,
};

static eg2_tally_t tally;

static tp_rmw_t place(bool sram) {
  if (!sram) {
    return eg2_tp_rmw;
  }
  uint32_t from = EG2_ADDR(eg2_tp_rmw), size = EG2_ADDR(eg2_tp_rmw_end) - from;
  for (uint32_t i = 0; i < size; i += 4) {
    eg2_ms_write32(kSecCode + i, eg2_ms_read32(from + i));
  }
  __asm__ volatile("fence.i" ::: "memory");
  void *entry;
  __asm__ volatile("auipcc %0, 0" : "=C"(entry));
  __asm__ volatile("csetaddr %0, %0, %1" : "+C"(entry) : "r"(kSecCode));
  return (tp_rmw_t)entry;
}

static void one_region(uint32_t idx, bool sram, uint32_t n, uint32_t idle_passes) {
  ++tally.planned;
  tp_rmw_t rmw = place(sram);
  uint32_t *buf = eg2_mem(kBuf, 4 * n);
  for (uint32_t i = 0; i < n; ++i) {
    eg2_ms_write32(kBuf + 4 * i, 0x1000u * i);
  }
  uint32_t idle = 0;
  for (uint32_t p = 0; p < idle_passes; ++p) {
    idle += rmw(buf, n, 1);
  }
  uint32_t idle_avg = idle / idle_passes;

  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(kEg2MsHeapBase, (kEg2MsHeapTop - kEg2MsHeapBase) / 8, &sw);
  uint32_t busy_passes = 0, busy_cycles = 0, passes = 0;
  while (passes < kMaxPasses && (eg2_ms_tbre_status() & kBusy)) {
    busy_cycles += rmw(buf, n, 1);
    ++busy_passes;
    ++passes;
  }
  eg2_ms_sweep_t done = {0};
  eg2_ms_tbre_wait(&done, 10);
  eg2_check(&tally, "sweep started", idx, sw.started, 1);
  eg2_check(&tally, "sweep ended while the core ran", idx, done.done, 1);
  eg2_check(&tally, "TBRE_STATUS", idx, done.status, 0);
  eg2_check(&tally, "epoch", idx, eg2_ms_tbre_epoch(), epoch0 + 2);
  eg2_ms_tbre_clear();
  uint32_t busy_avg = busy_passes != 0 ? busy_cycles / busy_passes : 0;
  eg2_check(&tally, "passes while the sweep ran", idx, busy_passes != 0 ? 1u : 0u, 1u);
  eg2_check(&tally, "average pass during the sweep within 4x idle", idx,
            busy_avg <= 4 * idle_avg ? 1u : 0u, 1u);
  uint32_t total = idle_passes + passes;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t exp = 0x1000u * i + total;
#ifdef EG2_TBRE_PROGRESS_INJECT_FAIL
    if (idx == 0 && i == 0) {
      exp += 1;
    }
#endif
    eg2_check(&tally, "buffer word", idx * 256 + i, eg2_ms_read32(kBuf + 4 * i), exp);
  }
  eg2_put_kv("EG2 tbre_progress code in", sram);
  eg2_puts(sram ? " (sec SRAM)" : " (RRAM)");
  eg2_put_kv(" words", n);
  eg2_put_kv(" idle cycles/pass", idle_avg);
  eg2_put_kv(" sweep cycles/pass", busy_avg);
  eg2_put_kv(" passes during the sweep", busy_passes);
  eg2_puts("\r\n");
  ++tally.done;
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  uint32_t n = EG2_PARAM(n_words) != 0 ? EG2_PARAM(n_words) : eg2_rand_range(16, 256);
  uint32_t idle_passes =
      EG2_PARAM(idle_passes) != 0 ? EG2_PARAM(idle_passes) : eg2_rand_range(4, 16);
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  eg2_dev_write32(kEg2DevSramCtrlSecRegs, SRAM_CTRL_EXEC_REG_OFFSET, kMuBi4True);
  bool sram_first = eg2_pick(2) != 0;
  one_region(0, sram_first, n, idle_passes);
  one_region(1, !sram_first, n, idle_passes);
  eg2_tally_print("tbre_progress", &tally);
  return eg2_tally_ok(&tally);
}
