// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_SWITCH_PARAMS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_SWITCH_PARAMS_H_

/**
 * chip_sw_rv_core_ibex_cheriot_switch's knobs (EG2 spec "Mode switch and handover"), shared by its
 * assembly and its C. A value at the top of a range means "draw it from the seed".
 *
 * lock_source  1 the core, 2 SBA (a bench step); 0 draws (70 : 30)
 * ena_writes   writes of CHERIOT_ENA before the final one and the lock, 0-8; 9 draws
 * ena_class    each write's value class: 1 MuBi4True, 2 MuBi4False, 3 another encoding; 0 draws
 *              per write (50 : 25 : 25)
 * final_mode   1 CHERIoT, 2 ePMP; 0 draws
 * model_gaps   no bench: the core locks
 */
#define EG2_SWITCH_PARAMS(X) \
  X(lock_source, 0, 0, 2)    \
  X(ena_writes, 9, 0, 9)     \
  X(ena_class, 0, 0, 3)      \
  X(final_mode, 0, 0, 2)     \
  X(model_gaps, 0, 0, 1)

// Capture area words (EG2_LATE_INFO + ...).
#define EG2_SWITCH_INFO_WRITES 0     // CHERIOT_ENA writes made
#define EG2_SWITCH_INFO_READBACK 4   // writes whose value did not read back
#define EG2_SWITCH_INFO_SOURCE 8     // 1 core, 2 SBA
#define EG2_SWITCH_INFO_SBA_ERR 12   // the bench's sberror for an SBA lock

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_SWITCH_PARAMS_H_
