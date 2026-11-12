// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Pure rules shared by the transaction model and its C-to-RTL proofs. These functions know
// only their arguments; no DUT state, ownership taps, or simulator interfaces are consulted.
#ifndef CMS_RULES_H_
#define CMS_RULES_H_
#include <stdint.h>

// ToO, Access Checkers: strict CHERIoT mode, half-open region, aligned full-word Get/PutFull.
// PutFull's mask is a separate TL-UL legality condition, checked by the transaction model.
static inline int cms_addr_in_region(uint32_t a, uint32_t base, uint32_t top) {
  return a >= base && a < top;
}
static inline int cms_access_forward(uint32_t a, uint32_t base, uint32_t top,
                                     unsigned op, unsigned size, unsigned ena) {
  return ena == 6 && cms_addr_in_region(a, base, top) && (a & 3) == 0 &&
         size == 2 && (op == 4 || op == 0);
}

// ToO, Meta SRAM Address Map: SRAM granules precede NVM granules in the model's numbering;
// one tag bit per eight bytes, 32 tag bits per meta word. The two tag regions have separate bases.
static inline uint32_t cms_tag_granule(uint32_t a, uint32_t sb, uint32_t st,
                                       uint32_t nb, uint32_t sram_caps) {
  return cms_addr_in_region(a, sb, st) ? (a - sb) >> 3 : sram_caps + ((a - nb) >> 3);
}
static inline uint32_t cms_tag_word_off(uint32_t g, uint32_t sram_caps,
                                        uint32_t sram_tag_off, uint32_t nvm_tag_off) {
  return g < sram_caps ? sram_tag_off + (g / 32) * 4 :
                        nvm_tag_off + ((g - sram_caps) / 32) * 4;
}
static inline unsigned cms_tag_bit(uint32_t g, uint32_t sram_caps) {
  return g < sram_caps ? g % 32 : (g - sram_caps) % 32;
}

// ToO, WTRC: W0 opens a capability, W1 must close the same granule. This is transaction
// history, not the RTL's pipelined FSM (the proof states the corresponding decision context).
static inline int cms_wtrc_sequence_bad(unsigned upper, unsigned open,
                                        uint32_t granule, uint32_t previous_granule) {
  return upper ? !open || granule != previous_granule : open != 0;
}
static inline int cms_wtrc_word_match(uint32_t expected, uint32_t actual,
                                     unsigned refused, unsigned read_error,
                                     unsigned bad_integrity) {
  return !refused && !read_error && !bad_integrity && expected == actual;
}
static inline int cms_wtrc_response_ok(unsigned upper, unsigned w0_ok, unsigned match) {
  return match && (!upper || w0_ok);
}
static inline int cms_wtrc_tag_set(unsigned upper, unsigned w0_ok, unsigned match) {
  return upper && cms_wtrc_response_ok(upper, w0_ok, match);
}
#endif  // CMS_RULES_H_
