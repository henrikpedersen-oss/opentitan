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

volatile void *ot_cheriot_mmio_cap(uintptr_t address) {
  const size_t addr = (size_t)address;
  for (size_t i = 0; i < sizeof(kWindows) / sizeof(kWindows[0]); ++i) {
    const eg2_window_t *w = &kWindows[i];
    if (addr - w->base < w->size) {
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
