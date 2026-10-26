// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_EPMP_HANDOVER_PARAMS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_EPMP_HANDOVER_PARAMS_H_

/**
 * chip_sw_cheriot_epmp_handover's knobs (EG2 spec "Mode switch and handover"), shared by its
 * assembly and its C. A value at the top of a range means "draw it from the seed".
 *
 * epmp_extra    locked deny-all entries over sec SRAM, the RRAM test area, retention SRAM and
 *               device windows (rv_timer, gpio, aon_timer, uart1, sram_ctrl_sec.regs), 0-8; 9 draws
 * mml           mseccfg.MML: 0, 1; 2 draws
 * first_access  the first access after the switch into a denied region: 1 fetch, 2 load, 3 store;
 *               0 draws
 */
#define EG2_EPMP_HANDOVER_PARAMS(X) \
  X(epmp_extra, 9, 0, 9)            \
  X(mml, 2, 0, 2)                   \
  X(first_access, 0, 0, 3)

#define EG2_EPMP_INFO_EXTRA 0
#define EG2_EPMP_INFO_MML 4
#define EG2_EPMP_INFO_PMPCFG0 8   // pmpcfg0-3 as read back after programming (16 bytes)
#define EG2_EPMP_INFO_MSECCFG 24

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_EPMP_HANDOVER_PARAMS_H_
