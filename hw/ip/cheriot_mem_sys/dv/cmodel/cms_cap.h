// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The capability decoding of the cheriot_mem_sys C reference model, in plain C: the base a
// capability's bounds give and whether it is a sealing capability. cms_core.h (the UVM bench's and
// eg2sim's model) uses these, and so does the C-to-RTL proof of the revocation decision
// (dv/c2rtl, Bambu turns this C into Verilog), so the model and the proof share one source.
//
// Source: [ISA] CHERIoT ISA v1.0 (capability encoding, sealing).

#ifndef CMS_CAP_H_
#define CMS_CAP_H_

#include <stdint.h>

// [ISA] capability bounds: word 1 holds R 31, p 30:25, otype 24:22, E 21:18 (15 meaning 24),
// T 17:9, B 8:0; base = ((a >> (E+9)) + c_b) << (E+9) | B << E, c_b = -1 when a[E+8:E] < B.
static inline uint32_t cms_cap_base(uint32_t addr, uint32_t meta) {
  unsigned ce = (meta >> 18) & 0xf, e = ce == 15 ? 24 : ce;
  uint64_t b = meta & 0x1ff, mid = ((uint64_t)addr >> e) & 0x1ff, top = (uint64_t)addr >> (e + 9);
  if (mid < b) top -= 1;
  return (uint32_t)((top << (e + 9)) | (b << e));
}

// [ISA] a sealing capability: compressed permissions p[4:3] == 0 with any of p[2:0]. A capability
// with no permissions is not sealing (reading Q17).
static inline int cms_cap_sealing(uint32_t meta) {
  uint32_t p = (meta >> 25) & 0x3f;
  return ((p >> 3) & 3) == 0 && (p & 7) != 0;
}

#endif  // CMS_CAP_H_
