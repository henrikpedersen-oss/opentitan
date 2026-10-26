// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_HEAP_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_HEAP_H_

/**
 * @file
 * @brief A bare-metal allocator with temporal safety for the CHERIoT post-boot tests (REQ_PBF_22),
 * as the CHERIoT RTOS allocator does it, from cheriot_mem_sys's programmer's guide ("Revoking
 * Capabilities", "Tracking Completed Sweeps"):
 *   - an allocation is a capability from the memory root, bounded to the object (CSetBounds);
 *   - its holders are capability slots in main SRAM ("holder" i at kHeapHolders + 8 i);
 *   - a free sets the revocation bit of every 8-byte granule of the object in the `revbm` window;
 *   - a sweep of the revocation engine (TBRE) over the holders clears the tag of every capability
 *     whose base is revoked; once it has ended without an error the bits are cleared and the memory
 *     may be handed out again.
 * CHERIoT builds only. Every function is static inline: each test includes this header once.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf.h"
#include "sw/device/tests/eg2_pbf/eg2_pbf_mem.h"

#ifdef __CHERIOT__

enum {
  // cheriot_mem_sys registers (hw/ip/cheriot_mem_sys/doc/registers.md).
  kMsIntrState = 0x0,
  kMsTbreRegwen = 0x10,
  kMsTbreBase = 0x14,
  kMsTbreNum = 0x18,
  kMsTbreStart = 0x1c,
  kMsTbreStatus = 0x20,
  kMsTbreEpoch = 0x24,
  kMsStatusBusy = 1u << 0,
  kMsStatusStartErr = 1u << 8,
  kMsStatusSweepErr = 1u << 9,
  // The heap: holders first, objects above them.
  kHeapHolders = kEg2PbfMainFree,
  kHeapMaxHolders = 64,
  kHeapObjects = kEg2PbfMainFree + 8 * kHeapMaxHolders,
  kHeapTop = kEg2PbfMainFree + 0x10000,
};

/** The memory root bounded to the main SRAM window. */
static inline void *heap_window(void) {
  return (void *)eg2_rm_ptr(TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR);
}

static inline void *volatile *heap_slot(uint32_t addr) {
  return (void *volatile *)__builtin_cheri_address_set(heap_window(), addr);
}

/** CLC of holder `i`; its tag. */
static inline uint32_t heap_holder_tag(uint32_t i) {
  void *c = *heap_slot(kHeapHolders + 8 * i);
  return __builtin_cheri_tag_get(c) ? 1 : 0;
}

static inline void heap_holder_store(uint32_t i, void *cap) {
  *heap_slot(kHeapHolders + 8 * i) = cap;
}

/**
 * An object of at least `len` bytes at or above `*next`: base aligned and length rounded so the
 * bounds are exact (CHERIoT ISA, representable bounds). Advances `*next`. NULL past kHeapTop.
 */
static inline void *heap_alloc(uint32_t *next, uint32_t len) {
  uint32_t mask = __builtin_cheri_representable_alignment_mask(len);
  uint32_t rlen = __builtin_cheri_round_representable_length(len);
  uint32_t base = (*next + ~mask) & mask;
  if (base + rlen > kHeapTop) {
    return NULL;
  }
  *next = base + rlen;
  void *c = __builtin_cheri_address_set(heap_window(), base);
  return __builtin_cheri_bounds_set_exact(c, rlen);
}

/** Sets or clears the revocation bit of every granule of the object `obj`. */
static inline void heap_mark(void *obj, bool revoked) {
  uint32_t base = (uint32_t)__builtin_cheri_base_get(obj);
  uint32_t len = (uint32_t)__builtin_cheri_length_get(obj);
  for (uint32_t a = base; a < base + len; a += 8) {
    uint32_t g = (a - TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR) / 8;
    volatile uint32_t *w = eg2_pbf_word(TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR + 4 * (g / 32));
    uint32_t bit = 1u << (g % 32);
    *w = revoked ? (*w | bit) : (*w & ~bit);
  }
}

/** Revocation bitmap words that are not zero. */
static inline uint32_t heap_revbm_nonzero(void) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_SIZE_BYTES / 4; ++i) {
    n += *eg2_pbf_word(TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR + 4 * i) != 0;
  }
  return n;
}

static inline uint32_t ms_rd(uint32_t off) {
  return eg2_dev_read32(kEg2DevCheriotMemSysRegs, off);
}

static inline void ms_wr(uint32_t off, uint32_t v) {
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, off, v);
}

/** Starts a sweep of `n` capabilities from `base`; false if the start was ignored (start_err). */
static inline bool tbre_start(uint32_t base, uint32_t n) {
  ms_wr(kMsTbreBase, base);
  ms_wr(kMsTbreNum, n);
  ms_wr(kMsTbreStart, 1);
  return (ms_rd(kMsTbreStatus) & kMsStatusStartErr) == 0;
}

/** Waits for the sweep's end; returns TBRE_STATUS then (busy set: it did not end). */
static inline uint32_t tbre_wait(uint32_t max_polls) {
  uint32_t st = ms_rd(kMsTbreStatus);
  for (uint32_t i = 0; i < max_polls && (st & kMsStatusBusy); ++i) {
    st = ms_rd(kMsTbreStatus);
  }
  // tbre_done is a level interrupt until written 1 (programmer's guide).
  ms_wr(kMsIntrState, 1);
  return st;
}

#endif  // __CHERIOT__

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PBF_EG2_PBF_HEAP_H_
