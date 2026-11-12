// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include "cms_rules.h"

uint32_t access_rules_hls(uint32_t addr, uint32_t base, uint32_t top,
                          uint32_t op, uint32_t size, uint32_t ena) {
  return cms_access_forward(addr, base, top, op, size, ena);
}
