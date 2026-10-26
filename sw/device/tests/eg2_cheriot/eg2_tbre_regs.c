// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tbre_regs (EG2 spec "Revocation", "Corner cases"; REQ_CMS_12). Ports
 * cms_tbre_csr, cms_tbre_epoch, cms_tbre_intr and bus_master_tbre_invalidate_only.
 *
 * The revocation engine's registers as cheriot_mem_sys.hjson and its programmer's guide describe
 * them. One source, two images:
 *
 * RISC-V mode (`chip_sw_cheriot_tbre_regs`): a start outside CHERIoT mode is ignored: start_err
 * set, busy 0, TBRE_REGWEN 1, the epoch unchanged; start_err is rw1c.
 *
 * CHERIoT mode (`chip_sw_cheriot_tbre_regs_cheriot`):
 *  1. Ignored starts: a base below main SRAM, at 0x1003_0000 (above sec SRAM), at 0 and in MMIO,
 *     in the meta SRAM, at the RRAM's top, and NUM_CAPS 0 with a legal base: start_err, busy 0,
 *     REGWEN 1, the epoch unchanged, nothing swept; start_err stays until written 1.
 *  2. BASE_ADDR written misaligned: bits 2:0 read 0; NUM_CAPS reads what was written.
 *  3. A base in RRAM is a legal start: the sweep ends without error, epoch +2.
 *  4. While a long sweep runs (main and sec SRAM, from the window's base): REGWEN 0, the epoch odd,
 *     busy 1; writes to BASE_ADDR, NUM_CAPS and START (a second start) are ignored: the registers
 *     keep their values, no start_err, and the epoch advances by 2 only; busy falls at the end.
 *  5. Exactly NUM_CAPS capabilities from BASE_ADDR: 64 slots in arena B, each based in a marked
 *     object of arena A, a sweep of a random run of them: the swept ones untagged, those before and
 *     after tagged, every data word unchanged; the bench counts the TBRE's reads inside the range
 *     (two per capability) and outside it (none).
 *  6. Sweeps that cross from main into sec SRAM (from 256 bytes below 0x1002_0000), and a sweep past
 *     the top of main and sec SRAM (from 8 * k bytes below 0x1003_0000, NUM_CAPS k + extra): it ends
 *     at the top without error, the top k slots revoked, no read at or above the top (the bench).
 *  7. (`rram_top`) A sweep past the RRAM's top. The programmer's guide: the topmost RRAM pages (the
 *     emulated OTP, from 0x301F_F600) cannot be read, "so sweeping them results in an error":
 *     sweep_err set, the epoch not advanced, no read at or above 0x3020_0000. The spec row expects
 *     no error here; the test follows the IP documentation (reported as a spec discrepancy).
 *
 * `model_gaps` (eg2sim): its TBRE does not revoke and there is no bench: the revoked slots' tags,
 * the bench's read counts and phase 7 are skipped.
 *
 * Built with EG2_TBRE_REGS_INJECT_FAIL, the first ignored start is expected without start_err: the
 * run must fail with `MISMATCH start_err #0`.
 */

#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/rram_ctrl_regs.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_revoke.h"

EG2_TEST_CONFIG();

#define TBRE_REGS_PARAMS(X) \
  X(rram_top, 0, 0, 1)      \
  X(past_top_extra, 0, 0, 64) \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TBRE_REGS_PARAMS);

enum {
  kStartErr = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_START_ERR_BIT,
  kSweepErr = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_SWEEP_ERR_BIT,
  kBusy = 1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT,
  kSlotsN = 64,
  kSlots = kEg2ArenaB + kEg2ArenaSize - 8 * kSlotsN,  // the top 64 granules of sec SRAM
  kObjects = kEg2ArenaA,
  kExpLo = kEg2WorkBase,
  kExpHi = kExpLo + 4 * kSlotsN,
};

static eg2_tally_t tally;
static bool gaps;

static uint32_t reg_rd(uint32_t off) { return eg2_dev_read32(kEg2DevCheriotMemSysRegs, off); }
static void reg_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevCheriotMemSysRegs, off, v); }
static uint32_t regwen(void) { return reg_rd(CHERIOT_MEM_SYS_TBRE_REGWEN_REG_OFFSET); }

/** A start that must be ignored. */
static void ignored_start(uint32_t idx, uint32_t base, uint32_t n) {
  uint32_t epoch = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(base, n, &sw);
  uint32_t exp = kStartErr;
#ifdef EG2_TBRE_REGS_INJECT_FAIL
  if (idx == 0) {
    exp = 0;
  }
#endif
  eg2_check(&tally, "start_err", idx, sw.status & kStartErr, exp);
  eg2_check(&tally, "busy after an ignored start", idx, sw.status & kBusy, 0);
  eg2_check(&tally, "REGWEN after an ignored start", idx, regwen(), 1);
  eg2_check(&tally, "epoch after an ignored start", idx, eg2_ms_tbre_epoch(), epoch);
  // Sticky until written 1 (rw1c): a write of 0 leaves it.
  reg_wr(CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET, 0);
  eg2_check(&tally, "start_err kept", idx, eg2_ms_tbre_status() & kStartErr, kStartErr);
  reg_wr(CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET, kStartErr);
  eg2_check(&tally, "start_err cleared", idx, eg2_ms_tbre_status() & kStartErr, 0);
}

#ifdef __CHERIOT__

/** A sweep that must complete without error, advancing the epoch by 2. */
static void good_sweep(const char *what, uint32_t idx, uint32_t base, uint32_t n) {
  uint32_t epoch = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw;
  eg2_ms_tbre_start(base, n, &sw);
  eg2_ms_tbre_wait(&sw, 10000000);
  eg2_check(&tally, what, idx, sw.started && sw.done ? sw.status : 0xffffffffu, 0);
  eg2_check(&tally, "epoch +2", idx, sw.epoch_after, epoch + 2);
  eg2_check(&tally, "REGWEN after the sweep", idx, regwen(), 1);
}

/** Slots: each a capability based in a marked object (the objects marked once). */
static void slots_init(void) {
  eg2_ms_revbm_clear();
  for (uint32_t s = 0; s < kSlotsN; ++s) {
    void *c = eg2_ms_object_cap(kObjects + 8 * s + eg2_pick(8), 8, eg2_pick(4) == 0);
    eg2_cap_t m = eg2_hw_read(c);
    eg2_ms_csc(kSlots + 8 * s, c);
    eg2_ms_write32(kExpLo + 4 * s, m.addr);
    eg2_ms_write32(kExpHi + 4 * s, m.hi);
  }
  for (uint32_t s = 0; s < kSlotsN; ++s) {
    eg2_ms_revbm_mark(kObjects + 8 * s, true);
  }
}

/** The slots after a sweep of [first, first + n): swept ones untagged, the others tagged. */
static void slots_check(uint32_t idx, uint32_t first, uint32_t n) {
  eg2_ms_revbm_clear();
  for (uint32_t s = 0; s < kSlotsN; ++s) {
    uint32_t a = kSlots + 8 * s;
    bool swept = s >= first && s < first + n;
    if (!(gaps && swept)) {
      eg2_check(&tally, swept ? "swept slot's tag" : "unswept slot's tag", idx * 100 + s,
                eg2_ms_tag(a), swept ? 0u : 1u);
    }
    eg2_check(&tally, "data word", idx * 100 + s, eg2_ms_read32(a), eg2_ms_read32(kExpLo + 4 * s));
    eg2_check(&tally, "data word", idx * 100 + s, eg2_ms_read32(a + 4),
              eg2_ms_read32(kExpHi + 4 * s));
  }
}

static void watch_check(uint32_t idx, uint32_t exp_inside, bool bench) {
  uint32_t in, out;
  (void)eg2_rv_tbre_watch_end(&in, &out, bench);
  if (bench) {
    eg2_check(&tally, "TBRE reads in the range", idx, in, exp_inside);
    eg2_check(&tally, "TBRE reads outside the range", idx, out, 0);
  }
}

#endif  // __CHERIOT__

bool eg2_test_main(void) {
  gaps = EG2_PARAM(model_gaps) != 0;
  eg2_ms_tbre_clear();
  eg2_check(&tally, "REGWEN idle", 0, regwen(), 1);
  eg2_check(&tally, "busy idle", 0, eg2_ms_tbre_status() & kBusy, 0);
  ++tally.planned;
#ifndef __CHERIOT__
  // RISC-V mode: every start is ignored, even a legal one.
  ignored_start(0, kEg2SecBase, 4);
  ++tally.done;
#else
  bool bench = !gaps;
  if (!eg2_sec_sram_init() || !eg2_ms_rram_setup()) {
    eg2_puts("EG2 sec SRAM or rram_ctrl initialisation failed\r\n");
    return false;
  }
  tally.planned += 6;
  // 1. Ignored starts.
  static const uint32_t kBad[] = {0x0ffffff8u, 0x10030000u, 0x00000000u, 0x40000000u,
                                  0x11000000u, 0x30200000u};
  for (uint32_t i = 0; i < ARRAYSIZE(kBad); ++i) {
    ignored_start(i, kBad[i], 4);
  }
  ignored_start(ARRAYSIZE(kBad), kEg2SecBase, 0);
  ++tally.done;

  // 2. BASE_ADDR bits 2:0, NUM_CAPS as written.
  reg_wr(CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET, kSlots + 5);
  eg2_check(&tally, "BASE_ADDR[2:0]", 0, reg_rd(CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET), kSlots);
  reg_wr(CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET, 0x12345);
  eg2_check(&tally, "NUM_CAPS", 0, reg_rd(CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET), 0x12345);
  ++tally.done;

  // 3. A base in RRAM.
  good_sweep("RRAM sweep", 0, kEg2MsRramArea, 8);
  ++tally.done;

  // 4. Locked writes while a long sweep runs.
  {
    uint32_t epoch = eg2_ms_tbre_epoch();
    uint32_t n = (kEg2MsHeapTop - kEg2MsHeapBase) / 8;
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(kEg2MsHeapBase, n, &sw);
    eg2_check(&tally, "long sweep started", 0, sw.started ? 1u : 0u, 1u);
    eg2_check(&tally, "REGWEN while running", 0, regwen(), 0);
    eg2_check(&tally, "epoch odd while running", 0, eg2_ms_tbre_epoch(), epoch + 1);
    eg2_check(&tally, "busy while running", 0, eg2_ms_tbre_status() & kBusy, kBusy);
    reg_wr(CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET, kSlots);
    reg_wr(CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET, 1);
    reg_wr(CHERIOT_MEM_SYS_TBRE_START_REG_OFFSET, 1);
    eg2_check(&tally, "still running after the locked writes", 0, regwen(), 0);
    eg2_check(&tally, "BASE_ADDR locked", 0, reg_rd(CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET),
              kEg2MsHeapBase);
    eg2_check(&tally, "NUM_CAPS locked", 0, reg_rd(CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET), n);
    eg2_ms_tbre_wait(&sw, 10000000);
    eg2_check(&tally, "long sweep", 0, sw.done ? sw.status : 0xffffffffu, 0);
    eg2_check(&tally, "epoch after a second start", 0, sw.epoch_after, epoch + 2);
    eg2_check(&tally, "REGWEN after the sweep", 0, regwen(), 1);
    ++tally.done;
  }

  // 5. Exactly NUM_CAPS from BASE_ADDR.
  {
    slots_init();
    uint32_t first = eg2_rand_range(1, kSlotsN / 2), n = eg2_rand_range(1, kSlotsN / 2 - 1);
    (void)eg2_rv_tbre_watch_begin(kSlots + 8 * first, kSlots + 8 * (first + n), bench);
    good_sweep("range sweep", 1, kSlots + 8 * first, n);
    watch_check(1, 2 * n, bench);
    slots_check(1, first, n);
    ++tally.done;
  }

  // 6. Across main into sec SRAM, and past the top of the window.
  {
    // 32 capabilities of main SRAM's top (the stack's own, none of them marked) and 32 of sec's.
    (void)eg2_rv_tbre_watch_begin(kEg2SecBase - 256, kEg2SecBase + 256, bench);
    good_sweep("main to sec sweep", 2, kEg2SecBase - 256, 64);
    watch_check(2, 128, bench);
    slots_init();
    uint32_t k = eg2_rand_range(1, kSlotsN);
    uint32_t extra = EG2_PARAM(past_top_extra) != 0 ? EG2_PARAM(past_top_extra)
                                                    : eg2_rand_range(1, 64);
    (void)eg2_rv_tbre_watch_begin(kEg2MsHeapTop - 8 * k, kEg2MsHeapTop, bench);
    good_sweep("sweep past the top", 3, kEg2MsHeapTop - 8 * k, k + extra);
    watch_check(3, 2 * k, bench);
    slots_check(3, kSlotsN - k, k);
    ++tally.done;
  }

  // 7. Past the RRAM's top, into the unreadable OTP pages.
  if (EG2_PARAM(rram_top) && !gaps) {
    uint32_t epoch = eg2_ms_tbre_epoch();
    uint32_t base = kEg2MsNvmTop - 8 * 4;
    (void)eg2_rv_tbre_watch_begin(base, kEg2MsNvmTop, bench);
    eg2_ms_sweep_t sw;
    eg2_ms_tbre_start(base, 4 + 16, &sw);
    eg2_ms_tbre_wait(&sw, 10000000);
    watch_check(4, 8, bench);
    eg2_check(&tally, "RRAM top sweep done", 0, sw.done ? 1u : 0u, 1u);
    eg2_check(&tally, "RRAM top sweep_err", 0, sw.status & (kSweepErr | kStartErr), kSweepErr);
    eg2_check(&tally, "RRAM top epoch not advanced", 0, sw.epoch_after, epoch);
    eg2_ms_tbre_clear();
    // rram_ctrl may log the refused host reads; ERR_CODE is rw1c.
    uint32_t err = eg2_dev_read32(kEg2DevRramCtrlCore, RRAM_CTRL_ERR_CODE_REG_OFFSET);
    eg2_ms_note("rram_ctrl ERR_CODE after the OTP-page reads", err);
    eg2_dev_write32(kEg2DevRramCtrlCore, RRAM_CTRL_ERR_CODE_REG_OFFSET, err);
  }
  ++tally.done;
  eg2_ms_revbm_clear();
#endif
  eg2_ms_tbre_clear();
  eg2_tally_print("tbre_regs", &tally);
  return eg2_tally_ok(&tally);
}
