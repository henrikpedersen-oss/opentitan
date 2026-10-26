// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_cheriot_sb (EG2 verification spec REQ_PBF_22, "Post-boot features in CHERIoT
 * mode"): allocation, free with revocation and reuse after secure boot (or the test ROM), in
 * CHERIoT mode only (no capabilities in RISC-V mode).
 *
 * The spec's test boots the CHERIoT RTOS behind secure boot and runs a compartment call. This tree
 * has no RTOS port for the Earl Grey 2 chip (the RTOS suite runs on the RTOS test SoC), so the
 * test does what the RTOS allocator does, bare metal, from cheriot_mem_sys's programmer's guide
 * (eg2_pbf_heap.h), and reports the RTOS part as a gap:
 *   1. The engine idle: TBRE_REGWEN 1, TBRE_STATUS.busy 0, TBRE_EPOCH even; the bitmap clear.
 *   2. `num_allocs` objects of `size_class`, each capability stored in its holder: all tagged.
 *   3. Half of them freed in `free_order`: their granules' revocation bits set.
 *   4. The load filter: a CLC of a freed object's holder returns it untagged, a live one tagged
 *      (CHERIoT ISA, the load barrier; RFC CHERIoT Ibex, TRVK).
 *   5. A sweep over the holders, before the reuse (`sweep_when` 0) or during it (1: a long sweep,
 *      and while TBRE_STATUS.busy reads 1 the core stores new capabilities into further holders:
 *      "a capability the core writes while the engine resolves it keeps the tag the core wrote").
 *      The sweep ends with no error and the epoch advanced by two ("Tracking Completed Sweeps").
 *   6. The bits cleared ("release the memory"): the freed objects' holders now load untagged, as
 *      the sweep left them in memory (the filter no longer hides them); live ones and those written
 *      during the sweep load tagged.
 *   7. The freed space handed out again: new capabilities to it, stored in the freed holders, load
 *      tagged.
 *
 * Knobs: size_class (0: 8-64 bytes, 1: 65-512, 2: 513-4096, 3: drawn per object), num_allocs,
 * free_order (0 ascending, 1 descending, 2 from the seed), sweep_when, model_gaps.
 * Built with EG2_PBF_INJECT_FAIL the first value check expects a wrong value: `PBF FAIL`.
 */

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_heap.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

EG2_TEST_CONFIG();

#define CHERIOT_SB_PARAMS(X) \
  X(size_class, 3, 0, 3)     \
  X(num_allocs, 8, 2, 16)    \
  X(free_order, 2, 0, 2)     \
  X(sweep_when, 1, 0, 1)     \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(CHERIOT_SB_PARAMS)

enum {
  kMaxAllocs = 16,
  kTailCaps = 8192,  // the long sweep's extra capabilities (sweep_when 1)
  kPolls = 1000000,
};

static uint32_t obj_size(void) {
  uint32_t c = EG2_PARAM(size_class);
  if (c == 3) {
    c = eg2_rand_range(0, 2);
  }
  return c == 0 ? eg2_rand_range(8, 64) : c == 1 ? eg2_rand_range(65, 512) : eg2_rand_range(513, 4096);
}

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  eg2_pbf_begin("cheriot", EG2_PBF_INJECT);
  eg2_pbf_boot_stage();
  eg2_pbf_gap("CHERIoT RTOS behind secure boot, compartment call",
              "no RTOS port for the Earl Grey 2 chip in this tree; the allocator's free, revocation "
              "and reuse run bare metal");
  bool gaps = EG2_PARAM(model_gaps) != 0;
  uint32_t n = EG2_PARAM(num_allocs);

  // 1. Idle engine, clear bitmap.
  ok = eg2_pbf_check("TBRE_STATUS at the start", 0, ms_rd(kMsTbreStatus)) && ok;
  ok = eg2_pbf_check("TBRE_REGWEN at the start", 1, ms_rd(kMsTbreRegwen) & 1) && ok;
  uint32_t epoch0 = ms_rd(kMsTbreEpoch);
  ok = eg2_pbf_check("TBRE_EPOCH even at the start", 0, epoch0 & 1) && ok;
  ok = eg2_pbf_check("revocation bitmap words set at the start", 0, heap_revbm_nonzero()) && ok;

  // 2. Allocate.
  void *objs[kMaxAllocs];
  uint32_t next = kHeapObjects;
  for (uint32_t i = 0; i < n; ++i) {
    objs[i] = heap_alloc(&next, obj_size());
    if (objs[i] == NULL) {
      return eg2_pbf_step("allocation inside the heap", false);
    }
    *(volatile uint32_t *)objs[i] = 0xa110c000 | i;
    heap_holder_store(i, objs[i]);
  }
  uint32_t tagged = 0;
  for (uint32_t i = 0; i < n; ++i) {
    tagged += heap_holder_tag(i);
  }
  ok = eg2_pbf_check("holders tagged after the allocation", n, tagged) && ok;

  // 3. Free half, in free_order.
  bool freed[kMaxAllocs] = {false};
  uint32_t nfree = n / 2;
  for (uint32_t k = 0; k < nfree; ++k) {
    uint32_t i;
    switch (EG2_PARAM(free_order)) {
      case 0:
        i = k;
        break;
      case 1:
        i = n - 1 - k;
        break;
      default:
        do {
          i = eg2_rand_range(0, n - 1);
        } while (freed[i]);
    }
    freed[i] = true;
    heap_mark(objs[i], true);
  }
  eg2_pbf_activity("objects freed", nfree);

  // 4. The load filter.
  uint32_t filter_wrong = 0;
  for (uint32_t i = 0; i < n; ++i) {
    filter_wrong += heap_holder_tag(i) != (freed[i] ? 0u : 1u);
  }
  if (gaps) {
    eg2_pbf_model_gap("load filter: a revoked capability loads untagged");
  } else {
    ok = eg2_pbf_check("holders loading with the wrong tag (load filter)", 0, filter_wrong) && ok;
  }

  // 5. The sweep, before or during the reuse.
  bool during = EG2_PARAM(sweep_when) == 1;
  uint32_t ncaps = during ? kHeapMaxHolders + kTailCaps : n;
  void *fresh[kMaxAllocs];
  ok = eg2_pbf_step("sweep started", tbre_start(kHeapHolders, ncaps)) && ok;
  uint32_t busy_seen = 0;
  if (during) {
    // New objects, never freed, into holders n..2n-1 while the engine sweeps them.
    for (uint32_t i = 0; i < n; ++i) {
      busy_seen += (ms_rd(kMsTbreStatus) & kMsStatusBusy) != 0;
      fresh[i] = heap_alloc(&next, obj_size());
      if (fresh[i] == NULL) {
        return eg2_pbf_step("allocation inside the heap", false);
      }
      heap_holder_store(n + i, fresh[i]);
    }
    busy_seen += (ms_rd(kMsTbreStatus) & kMsStatusBusy) != 0;
    eg2_pbf_activity("core capability stores while the sweep was busy", busy_seen);
  }
  uint32_t st = tbre_wait(kPolls);
  ok = eg2_pbf_check("TBRE_STATUS after the sweep", 0, st) && ok;
  ok = eg2_pbf_check("TBRE_EPOCH after the sweep", epoch0 + 2, ms_rd(kMsTbreEpoch)) && ok;
  eg2_pbf_activity("tbre sweeps", 1);

  // 6. Release, then the tags the sweep left in memory.
  for (uint32_t i = 0; i < n; ++i) {
    if (freed[i]) {
      heap_mark(objs[i], false);
    }
  }
  ok = eg2_pbf_check("revocation bitmap words set after the release", 0, heap_revbm_nonzero()) &&
       ok;
  uint32_t stale = 0, live_lost = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t t = heap_holder_tag(i);
    stale += freed[i] && t;
    live_lost += !freed[i] && !t;
  }
  if (gaps) {
    eg2_pbf_model_gap("the sweep clears the tags of revoked capabilities");
  } else {
    ok = eg2_pbf_check("freed objects' holders still tagged after the sweep", 0, stale) && ok;
  }
  ok = eg2_pbf_check("live objects' holders untagged after the sweep", 0, live_lost) && ok;
  if (during) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; ++i) {
      kept += heap_holder_tag(n + i);
    }
    ok = eg2_pbf_check("holders written during the sweep still tagged", n, kept) && ok;
    if (busy_seen == 0) {
      eg2_pbf_gap("stores during the sweep", "the sweep had ended before the first store");
    }
  }

  // 7. Reuse the freed space.
  uint32_t reused = 0;
  for (uint32_t i = 0; i < n; ++i) {
    if (freed[i]) {
      void *again = __builtin_cheri_bounds_set_exact(
          __builtin_cheri_address_set(heap_window(), (uint32_t)__builtin_cheri_base_get(objs[i])),
          (uint32_t)__builtin_cheri_length_get(objs[i]));
      heap_holder_store(i, again);
      reused += heap_holder_tag(i);
    }
  }
  ok = eg2_pbf_check("reused objects' holders tagged", nfree, reused) && ok;
  eg2_pbf_activity("objects reused", reused);
  return ok;
}
