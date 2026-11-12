// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include "cms_rules.h"

// Packed result: tagged[0], NVM[1], absolute meta address[33:2], tag bit[38:34].
uint64_t tag_map_hls(uint32_t addr, uint32_t sb, uint32_t st,
                     uint32_t nb, uint32_t nt, uint32_t mb) {
  unsigned sram = cms_addr_in_region(addr, sb, st);
  unsigned nvm = cms_addr_in_region(addr, nb, nt);
  if (!sram && !nvm) return 0;
  uint32_t caps = (st - sb) / 8;
  uint32_t nvm_off = (st - sb) / 64;
  uint32_t sram_off = nvm_off + (nt - nb) / 64;
  uint32_t g = cms_tag_granule(addr, sb, st, nb, caps);
  uint32_t word = mb + cms_tag_word_off(g, caps, sram_off, nvm_off);
  return 1 | ((uint64_t)nvm << 1) | ((uint64_t)word << 2) |
         ((uint64_t)cms_tag_bit(g, caps) << 34);
}
