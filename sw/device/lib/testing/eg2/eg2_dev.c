// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_dev.h"

typedef struct eg2_dev_window {
  uint32_t base;
  uint32_t size;
} eg2_dev_window_t;

static const eg2_dev_window_t kWindows[] = {
#define EG2_DEV_WINDOW_(id, name, base, size) {base, size},
    EG2_DEV_TABLE(EG2_DEV_WINDOW_)
#undef EG2_DEV_WINDOW_
};

static const char *const kNames[] = {
#define EG2_DEV_NAME_(id, name, base, size) name,
    EG2_DEV_TABLE(EG2_DEV_NAME_)
#undef EG2_DEV_NAME_
};

_Static_assert(sizeof(kWindows) / sizeof(kWindows[0]) == kEg2DevCount,
               "one window per device");

volatile uint32_t *eg2_dev(eg2_dev_id_t id) {
  const eg2_dev_window_t *w = &kWindows[id];
#ifdef __CHERIOT__
  // The memory root stays in MTDC for the whole test (the CRT never replaces it). A window's
  // base and size are powers of two aligned to the size, so exact bounds are representable;
  // were they not, the tag would be cleared and the first access would trap.
  void *root;
  __asm__ volatile("cspecialr %0, mtdc" : "=C"(root));
  void *cap = __builtin_cheri_address_set(root, w->base);
  return (volatile uint32_t *)__builtin_cheri_bounds_set_exact(cap, w->size);
#else
  return (volatile uint32_t *)(uintptr_t)w->base;
#endif
}

const char *eg2_dev_name(eg2_dev_id_t id) { return kNames[id]; }

uint32_t eg2_dev_read32(eg2_dev_id_t id, uint32_t offset) {
  return eg2_dev(id)[offset / sizeof(uint32_t)];
}

void eg2_dev_write32(eg2_dev_id_t id, uint32_t offset, uint32_t value) {
  eg2_dev(id)[offset / sizeof(uint32_t)] = value;
}

void eg2_dev_write32_shadowed(eg2_dev_id_t id, uint32_t offset, uint32_t value) {
  volatile uint32_t *reg = &eg2_dev(id)[offset / sizeof(uint32_t)];
  *reg = value;
  *reg = value;
}
