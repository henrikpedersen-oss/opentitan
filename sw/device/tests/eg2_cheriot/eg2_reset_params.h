// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_RESET_PARAMS_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_RESET_PARAMS_H_

/**
 * The knobs of chip_sw_cheriot_reset_state and chip_sw_cheriot_meta_init_after_reset (EG2 spec
 * "Mode switch and handover", "Revocation"), shared by their assembly and C. A value at the top of
 * a range means "draw it from the seed".
 *
 * reset_type      1 POR, 2 software, 3 NDM, 4 escalation, 5 deep-sleep wake-up, 6 watchdog bite;
 *                 0 draws (POR and NDM are bench steps)
 * session_len     loop iterations in CHERIoT mode before the reset (reset_state); 0 draws
 * tags_set        capabilities stored before the reset (reset_state, 0-64); 65 draws
 * n_tags          capabilities stored and bitmap bits set before the reset (meta_init, 0-256);
 *                 257 draws
 * sweep_running   a sweep running when the reset comes (meta_init): 0, 1; 2 draws
 * rely_on_crt_meta_init  stimulus only: 1 has the test's start-up code zero the meta SRAM on every
 *                 boot, as boot code must (REQ_CMS_15, open ticket F11); 0 leaves it to the boot
 *                 chain after the first boot
 * por_done        written by the bench on the boot after a power-on reset it applied
 * model_gaps      skip what the SystemC model cannot do (bench steps)
 */
#define EG2_RESET_PARAMS(X)          \
  X(reset_type, 0, 0, 6)             \
  X(session_len, 0, 0, 10000)        \
  X(tags_set, 65, 0, 65)             \
  X(n_tags, 257, 0, 257)             \
  X(sweep_running, 2, 0, 2)          \
  X(rely_on_crt_meta_init, EG2_RESET_RELY_DEFAULT, 0, 1) \
  X(por_done, 0, 0, 1)               \
  X(model_gaps, 0, 0, 1)

#ifdef EG2_META_INIT_TEST
#define EG2_RESET_RELY_DEFAULT 0
#else
#define EG2_RESET_RELY_DEFAULT 1
#endif

// Capture area words (EG2_LATE_INFO + ...), written in RISC-V mode before the switch.
#define EG2_RESET_INFO_ENA 0
#define EG2_RESET_INFO_MISA 4
#define EG2_RESET_INFO_REASON 8
#define EG2_RESET_INFO_META_INIT 12
#define EG2_RESET_INFO_MAGIC 16
#define EG2_RESET_MAGIC 0x5e57a7e0

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_CHERIOT_EG2_RESET_PARAMS_H_
