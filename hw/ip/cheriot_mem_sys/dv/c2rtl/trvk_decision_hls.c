// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The revocation decision of one capability, from the C reference model (dv/cmodel): whether its
// base needs a bitmap lookup, and which bitmap word and bit. Bambu turns this into Verilog, and
// trvk_decision.ys proves it equal to cheriot_trvk_core's decision.
//
// The model (cms_core.h, sweep and may_revoke): a sealing capability is never revoked; a base
// outside main SRAM needs no lookup (reading Q18); otherwise bit = (base - sram_base) >> 3, word
// bit / 32, bit bit % 32. Main SRAM is the heap the bitmap covers: 3072 bytes, one bit per
// 8-byte granule, so 196608 bytes from the heap base (CMS ToO, "Revocation Bitmap").
//
// Bambu has no struct ports: the result is packed, bit 0 the lookup, bits 5:1 the bit, bits 15:6
// the word.

#include "cms_cap.h"

#define BITMAP_BYTES 3072u

unsigned trvk_decision_hls(unsigned w0, unsigned w1, unsigned heap_base) {
  uint32_t base = cms_cap_base(w0, w1);
  uint32_t off = base - heap_base;
  uint32_t bit = off >> 3;
  unsigned in_heap = off < BITMAP_BYTES * 8u * 8u;
  unsigned lookup = !cms_cap_sealing(w1) && in_heap;
  return lookup | ((bit & 31u) << 1) | (((bit >> 5) & 0x3ffu) << 6);
}
