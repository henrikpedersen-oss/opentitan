// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The capability behind an MMIO address in the EG2 framework's CHERIoT build
// (sw/device/lib/base/cheriot_mmio.h): what makes OpenTitan's DIFs, testutils and `abs_mmio`
// work in CHERIoT mode unchanged.

#include "sw/device/lib/base/cheriot_mmio.h"

#include <stddef.h>

#include "sw/device/lib/testing/eg2/eg2_tables.h"

#ifdef __CHERIOT__

typedef struct eg2_window {
  uint32_t base;
  uint32_t size;
} eg2_window_t;

static const eg2_window_t kWindows[] = {
#define EG2_WINDOW_ROW_(name, base, size) {base, size},
    EG2_WINDOW_TABLE(EG2_WINDOW_ROW_)
#undef EG2_WINDOW_ROW_
};

// The window of the previous call, tried first: consecutive accesses nearly always go to the same
// device. Each call still checks the address against the window, so a stale index (an interrupt
// handler's access in between) only costs the search. Without it every MMIO access searched the
// whole table, about 500 instructions (2026-10-27: chip_sw_eg2_nist_aes_kw_cheriot spent its
// 8 hours on Xcelium, and the CHERIoT NIST tests their hour on Verilator, in this loop).
static size_t last_window;

volatile void *ot_cheriot_mmio_cap(uintptr_t address) {
  const size_t addr = (size_t)address;
  const size_t n = sizeof(kWindows) / sizeof(kWindows[0]);
  for (size_t k = 0; k < n; ++k) {
    const size_t i = k == 0 ? last_window : (k <= last_window ? k - 1 : k);
    const eg2_window_t *w = &kWindows[i];
    if (addr - w->base < w->size) {
      last_window = i;
      // The memory root stays in MTDC for the whole test. Windows are powers of two aligned to
      // their size, so exact bounds are representable; were one not, the tag would be cleared
      // and the first access would trap.
      void *root;
      __asm__ volatile("cspecialr %0, mtdc" : "=C"(root));
      void *cap = __builtin_cheri_address_set(root, w->base);
      cap = __builtin_cheri_bounds_set_exact(cap, w->size);
      return (volatile void *)__builtin_cheri_address_set(cap, addr);
    }
  }
  // In no window: an untagged pointer, so any access through it traps.
  return (volatile void *)address;
}

#endif  // __CHERIOT__
