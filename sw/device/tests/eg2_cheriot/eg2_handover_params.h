// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_HANDOVER_PARAMS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_HANDOVER_PARAMS_H_

/**
 * chip_sw_cheriot_handover's knobs (EG2 spec "Mode switch and handover"), shared by its assembly
 * (which reads them in RISC-V mode) and its C. A value at the top of a range means "draw it from
 * the seed" (no bench).
 *
 * switch_point  the instruction next to the CHERIOT_LOCK store: 0 a store just before it, 1 WFI after
 *               it, 2 a taken branch after it, 3 a CSR write after it, 4 a load just before it;
 *               5 draws
 * gpr_fill      the values of x1-x31, mtvec and mepc: 0 zero, 1 all ones, 2 random, 3 pointer-like
 *               (heap addresses); 4 draws
 * icache        cpuctrlsts.icache_enable before the switch: 0, 1; 2 as the boot left it
 * dummy_instr   cpuctrlsts.dummy_instr_en before the switch: 0, 1; 2 draws
 */
#define EG2_HANDOVER_PARAMS(X) \
  X(switch_point, 5, 0, 5)     \
  X(gpr_fill, 4, 0, 4)         \
  X(icache, 2, 0, 2)           \
  X(dummy_instr, 2, 0, 2)

#define EG2_HANDOVER_INFO_SWITCH_POINT 0
#define EG2_HANDOVER_INFO_FILL 4
#define EG2_HANDOVER_INFO_MTVEC 8
#define EG2_HANDOVER_INFO_MEPC 12
#define EG2_HANDOVER_INFO_CPUCTRL 16

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_HANDOVER_PARAMS_H_
