// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_debug/eg2_dm_cap.h"

#include "sw/device/tests/eg2_debug/eg2_dm_corners.h"

void eg2_dmx_ctx_init(eg2_dmx_ctx_t *ctx) {
  volatile uint8_t *b = (volatile uint8_t *)ctx;
  for (size_t i = 0; i < sizeof(*ctx); ++i) {
    b[i] = 0;
  }
  volatile uint32_t *scratch = eg2_dm_scratch();
  for (uint32_t i = 0; i <= kEg2DmxScratchArg3; ++i) {
    scratch[i] = 0;
  }
  ctx->stop.p = (void *)(uintptr_t)eg2_dm_stop();
  ctx->flag.p = (void *)(uintptr_t)&scratch[kEg2DmxScratchFlag];
}
