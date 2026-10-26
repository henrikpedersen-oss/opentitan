// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_rand_revocation (EG2 spec "Revocation"; REQ_CMS_11, REQ_CMS_12, REQ_CMS_15,
 * REQ_PBF_22).
 *
 * An allocator's view of temporal safety: `n_objects` objects placed in the revocable heap
 * (`w_heap_<h>`: main SRAM between the image's globals and its stack, sec SRAM's arenas), sizes by
 * `w_size_<s>` (one granule, small 9-511 bytes, large 512-8,192, a whole arena), a capability to
 * each stored in sec SRAM's work area. `free_pct` of them are freed: every granule of a freed
 * object is marked in the revocation bitmap (the CHERIoT allocator's quarantine). The slots are
 * then reloaded `loads_per_obj` times each in a random order, with `sweep_mode`: no sweep, a sweep
 * of the slots before the loads, a sweep running during them, or 2-4 sweeps one after another.
 *
 * Check (the test's model of what was freed; cheriot_mem_sys theory_of_operation.md):
 *  - every CLC of a freed object's capability returns tag 0 (the load filter, or the sweep), every
 *    other tag 1;
 *  - after the sweeps, with the bitmap cleared again, every swept slot of a freed object is
 *    untagged in memory and every other slot tagged;
 *  - TBRE_EPOCH advances by 2 per sweep; TBRE_STATUS shows no error;
 *  - liveness: loads made against loads planned.
 *
 * The SystemC model has neither the load filter nor a revoking engine (eg2_mpact_cpu.h): with
 * `model_gaps` the freed objects' tags are not checked.
 *
 * Built with EG2_RAND_REVOCATION_INJECT_FAIL, one more load is planned than made: the run must
 * fail with `EG2 rand_revocation: <n>/<n+1> done`.
 */

#include "sw/device/lib/testing/eg2/eg2_test.h"
#include "sw/device/tests/eg2_cheriot/eg2_cheriot_rand.h"
#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

EG2_TEST_CONFIG();

#define RAND_REVOCATION_PARAMS(X)   \
  X(n_objects, 0, 0, 256)           \
  X(w_size_granule, 20, 1, 100)     \
  X(w_size_small, 40, 1, 100)       \
  X(w_size_large, 30, 1, 100)       \
  X(w_size_region, 10, 1, 100)      \
  X(w_heap_main, 70, 1, 100)        \
  X(w_heap_sec, 30, 1, 100)         \
  X(free_pct, 0, 0, 90)             \
  X(sweep_mode, 4, 0, 4)            \
  X(loads_per_obj, 0, 0, 8)         \
  X(model_gaps, 0, 0, 1)
EG2_TEST_PARAMS(RAND_REVOCATION_PARAMS);

enum {
  kMaxObjects = 256,
  // sec SRAM work area: the slots, then per object base, length, freed.
  kSlots = kEg2WorkBase,
  kObjBase = kSlots + 8 * kMaxObjects,
  kObjLen = kObjBase + 4 * kMaxObjects,
  kObjFreed = kObjLen + 4 * kMaxObjects,
  kObjRevoked = kObjFreed + 4 * kMaxObjects,  // its base granule is marked
  kSweepNone = 0,
  kSweepBefore = 1,
  kSweepDuring = 2,
  kSweepRepeated = 3,
};

static eg2_tally_t tally;
static bool gaps;

typedef struct heap {
  uint32_t next, top;
} heap_t;

/** An object of `len` bytes from the heap (8-byte aligned, a random gap first); 0 if full. */
static uint32_t place(heap_t *h, uint32_t len) {
  uint32_t a = (h->next + 8 * eg2_rand_range(0, 4) + 7) & ~7u;
  if (a + len > h->top) {
    return 0;
  }
  h->next = a + len;
  return a;
}

static void check_load(uint32_t i, uint32_t idx) {
  bool freed = eg2_ms_read32(kObjRevoked + 4 * i) != 0;
  uint32_t t = eg2_ms_tag(kSlots + 8 * i);
  if (!(gaps && freed)) {
    eg2_check(&tally, freed ? "freed object's tag" : "live object's tag", idx, t, freed ? 0 : 1);
  }
}

bool eg2_test_main(void) {
  if (!eg2_sec_sram_init()) {
    return false;
  }
  gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n = EG2_PARAM(n_objects) != 0 ? EG2_PARAM(n_objects) : eg2_rand_range(8, 256);
  uint32_t free_pct = EG2_PARAM(free_pct) != 0 ? EG2_PARAM(free_pct) : eg2_rand_range(10, 90);
  uint32_t mode = EG2_PARAM(sweep_mode);
  if (mode == 4) {
    uint32_t d = eg2_rand_range(0, 99);
    mode = d < 10 ? kSweepNone : d < 40 ? kSweepBefore : d < 80 ? kSweepDuring : kSweepRepeated;
  }
  uint32_t lpo = EG2_PARAM(loads_per_obj) != 0 ? EG2_PARAM(loads_per_obj) : eg2_rand_range(1, 8);
  eg2_family_t sizes = {.n = 4, .plain_mask = 0, .boosted = 4};
  sizes.w[0] = EG2_PARAM(w_size_granule);
  sizes.w[1] = EG2_PARAM(w_size_small);
  sizes.w[2] = EG2_PARAM(w_size_large);
  sizes.w[3] = EG2_PARAM(w_size_region);
  eg2_family_t heaps = {.n = 2, .plain_mask = 0, .boosted = 2};
  heaps.w[0] = EG2_PARAM(w_heap_main);
  heaps.w[1] = EG2_PARAM(w_heap_sec);
  eg2_put_kv("EG2 rand_revocation n_objects", n);
  eg2_put_kv(" free_pct", free_pct);
  eg2_put_kv(" sweep_mode", mode);
  eg2_put_kv(" loads_per_obj", lpo);
  eg2_puts("\r\n");

  // The heaps: main SRAM between the globals and the stack's last 4 KiB (the arena), and sec
  // SRAM's arenas A and B (the work area holds the slots).
  heap_t main_heap = {.next = (eg2_sym_bss_end() + 64) & ~63u,
                      .top = eg2_sym_stack_start() - 0x1000};
  heap_t sec_a = {.next = kEg2ArenaA, .top = kEg2ArenaA + kEg2ArenaSize};
  heap_t sec_b = {.next = kEg2ArenaB, .top = kEg2ArenaB + kEg2ArenaSize};
  eg2_ms_revbm_clear();
  uint32_t made = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t s = eg2_family_pick(&sizes);
    uint32_t len = s == 0 ? eg2_rand_range(1, 8)
                   : s == 1 ? eg2_rand_range(9, 511)
                   : s == 2 ? eg2_rand_range(512, 8192)
                            : kEg2ArenaSize;
    heap_t *h = eg2_family_pick(&heaps) == 0 ? &main_heap : (eg2_pick(2) ? &sec_a : &sec_b);
    uint32_t base = place(h, len);
    if (base == 0) {
      base = place(&main_heap, len);
    }
    if (base == 0) {
      continue;  // the heaps are full: fewer objects
    }
    eg2_ms_csc(kSlots + 8 * made, eg2_ms_object_cap(base, len, false));
    // The base and length the hardware gave (CSetBounds may round a large object's bounds).
    void *c = eg2_ms_clc(kSlots + 8 * made);
    uint32_t b = eg2_ms_cap_base(c);
    uint32_t top;
    __asm__ volatile("cgettop %0, %1" : "=r"(top) : "C"(c));
    eg2_ms_write32(kObjBase + 4 * made, b);
    eg2_ms_write32(kObjLen + 4 * made, top - b);
    eg2_ms_write32(kObjFreed + 4 * made, 0);
    ++made;
  }
  n = made;

  // Free: mark every granule of each freed object.
  for (uint32_t i = 0; i < n; ++i) {
    if (eg2_rand_range(0, 99) < free_pct) {
      eg2_ms_write32(kObjFreed + 4 * i, 1);
      uint32_t b = eg2_ms_read32(kObjBase + 4 * i), len = eg2_ms_read32(kObjLen + 4 * i);
      for (uint32_t g = b & ~7u; g < b + len; g += 8) {
        eg2_ms_revbm_mark(g, true);
      }
    }
  }

  // Which capabilities are revoked: those whose base granule a freed object covers (a large
  // object's bounds are rounded, so a neighbour's base can fall into it).
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t g = eg2_ms_read32(kObjBase + 4 * i) & ~7u;
    uint32_t r = 0;
    for (uint32_t j = 0; j < n && !r; ++j) {
      uint32_t b = eg2_ms_read32(kObjBase + 4 * j), len = eg2_ms_read32(kObjLen + 4 * j);
      r = eg2_ms_read32(kObjFreed + 4 * j) && g >= (b & ~7u) && g < b + len;
    }
    eg2_ms_write32(kObjRevoked + 4 * i, r);
  }

  uint32_t sweeps = mode == kSweepNone ? 0 : mode == kSweepRepeated ? eg2_rand_range(2, 4) : 1;
  uint32_t epoch0 = eg2_ms_tbre_epoch();
  eg2_ms_sweep_t sw = {0};
  if (mode == kSweepBefore || mode == kSweepRepeated) {
    for (uint32_t k = 0; k < sweeps; ++k) {
      eg2_ms_tbre_start(kSlots, n, &sw);
      eg2_ms_tbre_wait(&sw, 10000000);
      eg2_check(&tally, "sweep", k, sw.started && sw.done ? sw.status : 0xffffffffu, 0);
    }
  } else if (mode == kSweepDuring) {
    eg2_ms_tbre_start(kSlots, n, &sw);
    eg2_check(&tally, "sweep started", 0, sw.started ? 1 : 0, 1);
  }

  // The loads, in a random order.
  uint32_t planned = n * lpo, idx = 0;
  tally.planned += planned;
  for (uint32_t k = 0; k < planned; ++k) {
    check_load(eg2_pick(n), idx++);
    ++tally.done;
  }
  if (mode == kSweepDuring) {
    eg2_ms_tbre_wait(&sw, 10000000);
    eg2_check(&tally, "sweep", 0, sw.done ? sw.status : 0xffffffffu, 0);
  }
  eg2_check(&tally, "epoch", 0, eg2_ms_tbre_epoch(), epoch0 + 2 * sweeps);

  // After the sweeps, with the marks gone: the swept freed slots untagged in memory.
  eg2_ms_revbm_clear();
  eg2_ms_tbre_clear();
  for (uint32_t i = 0; i < n; ++i) {
    bool freed = eg2_ms_read32(kObjRevoked + 4 * i) != 0;
    uint32_t exp = freed && sweeps != 0 ? 0 : 1;
    if (!(gaps && exp == 0)) {
      eg2_check(&tally, "slot after the sweeps", i, eg2_ms_tag(kSlots + 8 * i), exp);
    }
  }
#ifdef EG2_RAND_REVOCATION_INJECT_FAIL
  ++tally.planned;
#endif
  eg2_tally_print("rand_revocation", &tally);
  return eg2_tally_ok(&tally);
}
