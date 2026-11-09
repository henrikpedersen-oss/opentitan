// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The documented effect of an Access Register abstract command (transfer = 1) on the EG2 hart.
// Written from the documents only, not from dm_mem.sv:
//  - RISC-V Debug Specification 0.13.2, 3.6.1.1 "Access Register" and table 3.3 (regno map):
//    0x0000-0x0fff CSRs, 0x1000-0x101f GPRs, 0x1020-0x103f FPRs, 0x1040-0xbfff reserved,
//    0xc000-0xffff non-standard. A size larger than the register fails; smaller reads must work.
//    cmderr 2 (not supported) when the command is not supported.
//  - hw/ip/rv_dm/doc/theory_of_operation.md "CHERIoT Mode": aarsize 3 transfers a capability
//    (GPR 0x1000-0x101f, or the SCR named by regno 0x0-0x1f; higher CSR-range regno rejected).
//  - EG2: RV32 hart, no F extension, no non-standard registers, aarpostincrement not supported.
// Bambu synthesises this function; the Yosys proof compares it with what the RTL's program does.
//
// Result bits:
//   [0]     supported (0: cmderr 2, the hart must not change anything)
//   [1]     write (debugger -> register), else read (register -> data0)
//   [4:2]   class: 1 GPR, 2 CSR, 3 SCR
//   [16:5]  index (GPR number, CSR address or SCR number)
//   [19:17] width (aarsize)
//   [20]    postexec: the program buffer runs after the transfer

#define CLASS_GPR 1u
#define CLASS_CSR 2u
#define CLASS_SCR 3u

unsigned int dm_ar_semantic(unsigned int regno, unsigned int aarsize, unsigned int write,
                            unsigned int postinc, unsigned int postexec, unsigned int cheriot) {
  unsigned int cls = 0u;
  unsigned int idx = 0u;
  unsigned int ok = 1u;
  unsigned int r = regno & 0xffffu;
  unsigned int size = aarsize & 7u;
  // Largest register: 32 bits on RV32 (aarsize 2), a 64-bit capability in CHERIoT mode (3).
  unsigned int max_size = cheriot ? 3u : 2u;

  if (r < 0x1000u) {
    if (cheriot && size == 3u) {
      // CHERIoT, aarsize 3: regno 0x0-0x1f names an SCR, the rest of the CSR range is rejected.
      cls = CLASS_SCR;
      idx = r;
      if (r > 0x1fu) ok = 0u;
    } else {
      cls = CLASS_CSR;
      idx = r;
    }
  } else if (r < 0x1020u) {
    cls = CLASS_GPR;
    idx = r - 0x1000u;
  } else {
    // FPRs (the hart has no F extension), reserved and non-standard numbers.
    ok = 0u;
  }
  if (size > max_size) ok = 0u;
  if (postinc & 1u) ok = 0u;

  if (!ok) return 0u;
  return 1u | ((write & 1u) << 1) | (cls << 2) | (idx << 5) | (size << 17) | ((postexec & 1u) << 20);
}
