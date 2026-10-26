// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_cheriot_tag_roundtrip (EG2 spec "Memory, tags and devices"; REQ_CMS_10, REQ_TL_06).
 *
 * A capability is two 32-bit accesses with its tag beside TL-UL. A CSC/CLC round trip keeps the tag
 * in main SRAM, sec SRAM and RRAM; a CLC from ROM, retention SRAM, debug memory or a peripheral
 * returns tag 0 (RFC CHERIoT in OpenTitan p3-4; cheriot_mem_sys theory_of_operation.md "Tag
 * Filter", "Write-to-Read-and-Compare Filter").
 *
 *  - Directed first: every region at its first, last and an interior granule (main SRAM's first
 *    and last granules hold the image's globals and the outermost stack frame: they are saved with
 *    CLC and put back with CSC; RRAM's are those of the test's RRAM area, since the image runs
 *    from the start of RRAM and the top pages are OTP).
 *  - Then `n_caps` slots, the region by `w_region_<r>`, the granule by `granule_pos`; a share
 *    `int_store_pct` overwritten by an integer store (byte, half or word, anywhere in the granule)
 *    before the reload.
 *
 * Expected, from the documents:
 *  - main and sec SRAM: the CLC returns what the CSC stored, tag 1; after an integer store, tag 0
 *    ("Any non-capability write clears the tag bit", TAG_CLEAR_ON_DATA_WRITE).
 *  - RRAM: the capability's words are programmed through rram_ctrl, then the CSC to the same
 *    address is verified, not written, and sets the tag (WTRC); the CLC returns it tagged. An
 *    integer store to RRAM is answered with a bus error (the NVM "cannot be written through the
 *    interconnect") and still clears the tag ("a failed data write still updates the tag").
 *  - retention SRAM: the CSC writes the data; the CLC returns it with tag 0.
 *  - ROM, debug memory, a device (rv_plic's PRIO_0/PRIO_1): the CLC returns the words a 32-bit
 *    load reads there, tag 0.
 *
 * The SystemC model has no debug memory (a load from rv_dm.mem is an error there), answers a CLC
 * from a register window (one 8-byte transaction there, two 32-bit ones on the chip) with an
 * error, and writes RRAM
 * through the host window: with `model_gaps` debug memory and the device are left out and the RRAM integer stores
 * are not checked.
 *
 * Not checked here (bench): the meta SRAM by backdoor, and that an untagged region makes no meta
 * request (REQ_CMS_10's other checker; eg2_tag_sideband_assert, REQ_TL_06).
 *
 * Built with EG2_TAG_ROUNDTRIP_INJECT_FAIL, the first main SRAM round trip expects tag 0: the run
 * must fail with `MISMATCH tag ...`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"
#include "sw/device/tests/eg2_cheriot/eg2_probe.h"

EG2_TEST_CONFIG();

// n_caps 0 draws 16-512. The vseq always writes it (16-512); the default is for runs without a
// bench: 16, the lowest draw (regr-vlt 2026-10-26: 460 drawn, TIMEOUT at 3600 s with no line after
// the weights; was 0).
#define TAG_ROUNDTRIP_PARAMS(X)  \
  X(n_caps, 16, 0, 512)          \
  X(w_region_main, 30, 1, 100)   \
  X(w_region_sec, 25, 1, 100)    \
  X(w_region_rram, 25, 1, 100)   \
  X(w_region_rom, 5, 1, 100)     \
  X(w_region_ret, 5, 1, 100)     \
  X(w_region_dbg, 5, 1, 100)     \
  X(w_region_dev, 5, 1, 100)     \
  X(granule_pos, 0, 0, 3)        \
  X(int_store_pct, 101, 0, 101)  \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(TAG_ROUNDTRIP_PARAMS);

typedef enum region {
  kMain = 0,
  kSec,
  kRram,
  kRom,
  kRet,
  kDbg,
  kDev,
  kRegionCount,
} region_t;

static const char *region_name(region_t r) {
  switch (r) {
    case kMain:
      return "main SRAM";
    case kSec:
      return "sec SRAM";
    case kRram:
      return "RRAM";
    case kRom:
      return "ROM";
    case kRet:
      return "retention SRAM";
    case kDbg:
      return "debug memory";
    default:
      return "rv_plic";
  }
}

enum {
  // Retention SRAM: the test's KiB minus the boot record at its top.
  kRetArea = 0x40601c00u,
  kRetAreaSize = 0x300u,
  kPlicBase = TOP_EARLGREY_RV_PLIC_BASE_ADDR,
  // RRAM slots: one capability per 16-byte RRAM word, so programming one never touches another.
  kRramSlots = kEg2MsRramAreaSize / 16,
};

static eg2_tally_t tally;
static uint32_t rram_next;  // the next free RRAM word of the area

/** The region's [base, size) for a slot: the part the test may use. */
static void region_span(region_t r, uint32_t *base, uint32_t *size) {
  switch (r) {
    case kMain:
      *base = kEg2MainBase;
      *size = kEg2MainSize;
      return;
    case kSec:
      *base = kEg2SecBase;
      *size = kEg2SecSize;
      return;
    case kRram:
      *base = kEg2MsRramArea;
      *size = kEg2MsRramAreaSize;
      return;
    case kRom:
      *base = kEg2RomBase;
      *size = kEg2RomSize;
      return;
    case kRet:
      *base = kRetArea;
      *size = kRetAreaSize;
      return;
    case kDbg:
      *base = kEg2MsDbgMemBase;
      *size = kEg2MsDbgMemSize;
      return;
    default:
      *base = kPlicBase;
      *size = 8;
      return;
  }
}

/** A random capability to store: a heap object, untagged words never (every slot stores a tag). */
static void *random_cap(void) {
  uint32_t base = kEg2MsHeapBase + 8 * eg2_rand_range(0, (kEg2MsHeapTop - kEg2MsHeapBase) / 8 - 64);
  uint32_t len = eg2_rand_range(1, 256);
  return eg2_ms_object_cap(base, len, false);
}

/** A free main SRAM granule for random slots: the main arena below the stack. */
static uint32_t main_random_granule(void) {
  uint32_t a = eg2_main_arena();
  return a + 8 * eg2_rand_range(0, kEg2MainArenaSize / 8 - 1);
}

/** Integer store into the granule at `g`; returns true if it trapped (mcause in the record). */
static bool int_store(uint32_t g) {
  uint32_t kind = eg2_pick(3);
  uint32_t off = kind == 2 ? 4 * eg2_pick(2) : kind == 1 ? 2 * eg2_pick(4) : eg2_pick(8);
  void *a = eg2_mem(g + off, kind == 2 ? 4 : kind == 1 ? 2 : 1);
  void *stub = kind == 2 ? eg2_stub_sw : kind == 1 ? eg2_stub_sh : eg2_stub_sb;
  return eg2_probe_call(a, (void *)(uintptr_t)eg2_rand(), NULL, stub);
}

/** CLC under the probe; returns true if it trapped. `*out` gets the loaded capability. */
static bool probe_clc(uint32_t addr, void **out) {
  bool t = eg2_probe_call(eg2_ms_auth(addr), NULL, NULL, eg2_stub_clc);
  *out = eg2_trap_rec.cap.p;
  return t;
}

/** CSC under the probe. */
static bool probe_csc(uint32_t addr, void *cap) {
  return eg2_probe_call(eg2_ms_auth(addr), cap, NULL, eg2_stub_csc);
}

/** Programs the capability's words into the RRAM word holding `g` (the other half zero). */
static bool rram_put(uint32_t g, void *cap) {
  uint32_t w[4] = {0, 0, 0, 0};
  uint32_t half = (g & 8) ? 2 : 0;
  w[half] = eg2_ms_cap_addr(cap);
  w[half + 1] = eg2_ms_cap_high(cap);
  uint32_t err;
  bool ok = eg2_ms_rram_program(g & ~15u, w, 4, &err);
  if (!ok) {
    eg2_ms_note("rram_ctrl program failed, ERR_CODE", err);
  }
  return ok;
}

static bool first_main = true;

/**
 * One round trip at granule `g` of region `r`, with an integer store first if `int_store_first`.
 * Every expectation from the documents (see the file comment).
 */
static void roundtrip(region_t r, uint32_t g, bool int_store_first, uint32_t idx) {
  ++tally.planned;
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool writable = r == kMain || r == kSec || r == kRet;
  bool tagged = r == kMain || r == kSec || r == kRram;
  // main SRAM's first and last granules hold live data: keep it.
  bool keep = r == kMain && (g == kEg2MainBase || g == kEg2MainBase + kEg2MainSize - 8);
  void *saved = keep ? eg2_ms_clc(g) : NULL;

  void *cap = random_cap();
  uint32_t exp_lo = eg2_ms_cap_addr(cap), exp_hi = eg2_ms_cap_high(cap);
  uint32_t exp_tag = tagged ? 1 : 0;
  if (writable) {
    eg2_check(&tally, "csc trapped", idx, probe_csc(g, cap) ? 1 : 0, 0);
  } else if (r == kRram) {
    if (rram_put(g, cap)) {
      // The words now match: the CSC is verified and sets the tag.
      eg2_check(&tally, "rram csc trapped", idx, probe_csc(g, cap) ? 1 : 0, 0);
    } else {
      eg2_check(&tally, "rram program", idx, 0, 1);
    }
  } else {
    // ROM, debug memory, a device: what is there.
    exp_lo = eg2_ms_read32(g);
    exp_hi = eg2_ms_read32(g + 4);
  }
  if (int_store_first && (writable || r == kRram)) {
    uint32_t lo = eg2_ms_read32(g), hi = eg2_ms_read32(g + 4);
    bool trapped = int_store(g);
    if (r == kRram) {
      if (!gaps) {
        // A bus error on the store, and the tag cleared all the same.
        eg2_check(&tally, "rram int store mcause", idx, trapped ? eg2_trap_rec.mcause : 0,
                  kEg2McauseStoreFault);
        eg2_check(&tally, "rram int store lo", idx, eg2_ms_read32(g), lo);
        eg2_check(&tally, "rram int store hi", idx, eg2_ms_read32(g + 4), hi);
      }
      exp_lo = eg2_ms_read32(g);
      exp_hi = eg2_ms_read32(g + 4);
    } else {
      eg2_check(&tally, "int store trapped", idx, trapped ? 1 : 0, 0);
      exp_lo = eg2_ms_read32(g);
      exp_hi = eg2_ms_read32(g + 4);
    }
    exp_tag = 0;
  }
#ifdef EG2_TAG_ROUNDTRIP_INJECT_FAIL
  if (r == kMain && first_main) {
    exp_tag = 0;
  }
#endif
  if (r == kMain) {
    first_main = false;
  }
  void *got = NULL;
  bool trapped = probe_clc(g, &got);
  if (!eg2_check(&tally, "clc trapped", idx, trapped ? eg2_trap_rec.mcause : 0, 0)) {
    if (tally.failed <= kEg2MaxReports) {
      eg2_puts("  in ");
      eg2_puts(region_name(r));
      eg2_put_kv(" at", g);
      eg2_puts("\r\n");
    }
  } else {
    uint32_t failed = tally.failed;
    eg2_check(&tally, "tag", idx, eg2_ms_cap_tag(got), exp_tag);
    eg2_check(&tally, "lo", idx, eg2_ms_cap_addr(got), exp_lo);
    eg2_check(&tally, "hi", idx, eg2_ms_cap_high(got), exp_hi);
    if (tally.failed != failed && failed < kEg2MaxReports) {
      eg2_puts("  in ");
      eg2_puts(region_name(r));
      eg2_put_kv(" at", g);
      eg2_puts("\r\n");
    }
  }
  if (keep) {
    eg2_ms_csc(g, saved);
  }
  ++tally.done;
}

/** The granule of a slot: first, last or interior of the region's usable span. */
static uint32_t pick_granule(region_t r, uint32_t pos) {
  uint32_t base, size;
  region_span(r, &base, &size);
  if (r == kRram) {
    // One capability per RRAM word: take the next free word, at its first or second half.
    uint32_t w = pos == 1 ? 0 : pos == 2 ? kRramSlots - 1 : 1 + (rram_next++ % (kRramSlots - 2));
    return base + 16 * w + (pos == 2 ? 8 : 8 * eg2_pick(2));
  }
  if (r == kMain && pos == 3) {
    return main_random_granule();
  }
  if (pos == 1) {
    return base;
  }
  if (pos == 2) {
    return base + size - 8;
  }
  uint32_t n = size / 8;
  return n <= 2 ? base : base + 8 * eg2_rand_range(1, n - 2);
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    eg2_puts("EG2 no main SRAM arena below the stack\r\n");
    return false;
  }
  if (!eg2_ms_rram_setup()) {
    eg2_puts("EG2 rram_ctrl initialisation did not complete\r\n");
    return false;
  }
  // The retention SRAM area: every word written before any is read.
  for (uint32_t a = kRetArea; a < kRetArea + kRetAreaSize; a += 4) {
    eg2_ms_write32(a, 0);
  }
  uint32_t pct = EG2_PARAM(int_store_pct) == 101 ? eg2_rand_range(0, 50) : EG2_PARAM(int_store_pct);
  uint32_t n = EG2_PARAM(n_caps) != 0 ? EG2_PARAM(n_caps) : eg2_rand_range(16, 512);
  eg2_put_kv("EG2 tag_roundtrip n_caps", n);
  eg2_put_kv(" int_store_pct", pct);
  eg2_puts("\r\n");

  eg2_family_t regions = {.n = kRegionCount, .plain_mask = 0, .boosted = kRegionCount};
  regions.w[kMain] = EG2_PARAM(w_region_main);
  regions.w[kSec] = EG2_PARAM(w_region_sec);
  regions.w[kRram] = EG2_PARAM(w_region_rram);
  regions.w[kRom] = EG2_PARAM(w_region_rom);
  regions.w[kRet] = EG2_PARAM(w_region_ret);
  regions.w[kDbg] = EG2_PARAM(w_region_dbg);
  regions.w[kDev] = EG2_PARAM(w_region_dev);
  if (EG2_PARAM(model_gaps)) {
    regions.w[kDbg] = 0;
    regions.w[kDev] = 0;
  }
  eg2_family_print("region", &regions);

  eg2_probe_begin();
  // Directed: every region at its first, last and an interior granule, with and without an
  // integer store in between for the writable and tagged ones.
  uint32_t idx = 0;
  for (uint32_t r = 0; r < kRegionCount; ++r) {
    for (uint32_t pos = 1; pos <= 3; ++pos) {
      region_t reg = (region_t)r;
      if ((reg == kDbg || reg == kDev) && EG2_PARAM(model_gaps)) {
        continue;  // the model: no debug memory; a register window refuses an 8-byte CLC
      }
      if (reg == kDev && pos != 1) {
        continue;
      }
      roundtrip(reg, pick_granule(reg, pos), false, idx++);
      if (reg == kSec || reg == kRram || reg == kRet || (reg == kMain && pos == 3)) {
        roundtrip(reg, pick_granule(reg, pos == 3 ? 3 : pos), true, idx++);
      }
    }
  }
  eg2_tally_print("directed", &tally);

  for (uint32_t i = 0; i < n; ++i) {
    region_t r = (region_t)eg2_family_pick(&regions);
    uint32_t pos = EG2_PARAM(granule_pos);
    if (pos == 0) {
      uint32_t d = eg2_rand_range(0, 99);
      pos = d < 25 ? 1 : d < 50 ? 2 : 3;
    }
    if (r == kDev) {
      pos = 1;
    }
    bool st = eg2_rand_range(0, 99) < pct;
    roundtrip(r, pick_granule(r, pos), st, idx++);
  }
  eg2_probe_end();
  eg2_tally_print("tag_roundtrip", &tally);
  return eg2_tally_ok(&tally);
}
