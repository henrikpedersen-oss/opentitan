// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Full-model HLS feasibility trial, not an RTL equivalence harness. This dispatches
// external transaction events into the actual persistent cms::Model. It preserves
// its dynamic containers and set-valued predictions; it does not substitute a
// reduced model or take ownership/history from DUT internals.
//
// A future RTL miter must derive events from external handshakes, align transactions,
// supply independent external memories, and compare allowed sets/care masks. A
// successful synthesis of this function alone would not prove equivalence.

#include "cms_core.h"

// event: 0 model initialization, 1 core access, 2 bitmap access, 3 CSR read,
// 4 CSR write, 5 DUT reset, 6 external-data initialization, 7 tag initialization,
// 8 bitmap initialization, 9 externally observed sweep completion, 10 sweep read
// error, 11 sweep meta fault, 12 ambiguous core meta fault, 13 ambiguous bitmap
// write, 14 interrupt/fatal status only.
// ctl: opcode[2:0], size[4:3], byte mask[8:5], tag[9], mode[13:10], host error[14],
// meta error[15], bad command/data integrity[16], optional/ambiguous fault[17].
// addr is an absolute core/data address or a relative CSR/bitmap/meta offset.
// Initialization events set independent environment state, never observed DUT data.
// Return: data[63:32], error[0], allowed tag set[2:1], interrupt[3], fatal[4], active[5].
extern "C" uint64_t cms_system_hls_trial(unsigned event, uint32_t addr, uint32_t data,
                                         uint32_t ctl, uint32_t *care) {
  static cms::Model model(cms::Map::earlgrey());
  uint32_t result = 0;
  unsigned error = 0;
  cms::TagSet tags = cms::kTagAny;
  *care = 0;
  const uint8_t op = ctl & 7;
  const uint8_t size = (ctl >> 3) & 3;
  const uint8_t mask = (ctl >> 5) & 15;
  const uint8_t ena = (ctl >> 10) & 15;
  switch (event) {
    case 0: model.reset(); break;
    case 1: {
      cms::CoreReq req{op, addr, size, mask, data, bool(ctl & (1u << 9)), ena,
                       bool(ctl & (1u << 14)), bool(ctl & (1u << 15)),
                       bool(ctl & (1u << 16))};
      cms::CoreExp exp = model.predict_core(req);
      result = exp.rdata; *care = exp.rdata_care; error = exp.err; tags = exp.rtag;
      if (ctl & (1u << 17)) model.update_core_possible_fault(req);
      else model.update_core(req);
      break;
    }
    case 2: {
      cms::BitmapExp exp = model.predict_bitmap(op, addr, size, mask, ena);
      result = exp.rdata; *care = exp.rdata_care; error = exp.err;
      model.update_bitmap(op, addr, size, mask, data, ena);
      break;
    }
    case 3: {
      cms::CsrExp exp = model.predict_csr_read(addr);
      result = exp.rdata; *care = exp.rdata_care;
      break;
    }
    case 4: model.update_csr_write(addr, data); break;
    case 5: model.dut_reset(); break;
    case 6: model.backdoor_data(addr, data); break;
    case 7: model.backdoor_tag(addr, data & 1); break;
    case 8: model.backdoor_revbm(addr, data); break;
    case 9: model.sync_sweep_end(addr, data); break;
    case 10: model.sweep_read_error(addr); break;
    case 11: model.sweep_meta_fault(addr, ctl & (1u << 17)); break;
    case 12: model.open_core_fault(addr, op != cms::kGet); break;
    case 13: model.open_bitmap_write(addr, data); break;
    default: break;
  }
  return (uint64_t(result) << 32) | error | (unsigned(tags) << 1) |
         (unsigned(model.intr_expected()) << 3) |
         (unsigned(model.fatal_expected()) << 4) |
         (unsigned(model.sweep_active()) << 5);
}
