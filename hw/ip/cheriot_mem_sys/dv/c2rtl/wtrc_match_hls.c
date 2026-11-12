// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include "cms_rules.h"

// ctl: open[0], opcode[3:1], size[5:4], mask[9:6], cmd_bad[10], read_error[11],
// previous W0 match[12], response valid[13]. Integrity fields are independent 7-bit inputs.
// Result: refused[0], response_ok[1], tag_request[2], word_match[3], sequence_bad[4].
uint32_t wtrc_match_hls(uint32_t addr, uint32_t previous_addr, uint32_t ctl,
                        uint32_t expected, uint32_t actual,
                        uint32_t expected_intg, uint32_t actual_intg) {
  unsigned upper = (addr >> 2) & 1;
  unsigned seq_bad = cms_wtrc_sequence_bad(upper, ctl & 1, addr >> 3, previous_addr >> 3);
  unsigned full = ((ctl >> 1) & 7) == 0 && ((ctl >> 4) & 3) == 2 &&
                  ((ctl >> 6) & 15) == 15;
  unsigned refused = !full || seq_bad || ((ctl >> 10) & 1);
  unsigned match = cms_wtrc_word_match(expected, actual, refused, (ctl >> 11) & 1,
                                      expected_intg != actual_intg);
  unsigned w0_ok = (ctl >> 12) & 1;
  unsigned ok = cms_wtrc_response_ok(upper, w0_ok, match);
  unsigned tag = ((ctl >> 13) & 1) && cms_wtrc_tag_set(upper, w0_ok, match);
  return refused | (ok << 1) | (tag << 2) | (match << 3) | (seq_bad << 4);
}
