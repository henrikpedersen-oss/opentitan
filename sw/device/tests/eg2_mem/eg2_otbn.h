// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_OTBN_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_OTBN_H_

/**
 * @file
 * @brief OTBN from the core's side for chip_sw_otbn_isa and chip_sw_otbn_keymgr (eg2_mem), through
 * the device table in both modes (a bounded capability in CHERIoT mode): load an embedded OTBN
 * application (rules/otbn.bzl `otbn_binary`) into IMEM and DMEM, run it, wait, read DMEM.
 * OTBN's registers.md: CMD EXECUTE 0xd8, SEC_WIPE_DMEM 0xc3, SEC_WIPE_IMEM 0x1e; STATUS 0 idle;
 * ERR_BITS; INSN_CNT; INTR_STATE.done.
 */

#include <stdbool.h>
#include <stdint.h>

#include "hw/top/otbn_regs.h"  // Generated.
#include "sw/device/tests/eg2_mem/eg2_chk.h"

enum {
  kEg2OtbnCmdExecute = 0xd8,
  kEg2OtbnCmdSecWipeDmem = 0xc3,
  kEg2OtbnCmdSecWipeImem = 0x1e,
  kEg2OtbnStatusIdle = 0,
};

/**
 * An OTBN application as word tables (gen_otbn_image.py, from its otbn_binary ELF: the RV32 object
 * `otbn_binary` embeds does not link with CHERIoT code): IMEM and DMEM images from address 0.
 */
typedef struct eg2_otbn_app {
  const uint32_t *imem;
  uint32_t imem_words;
  const uint32_t *dmem;
  uint32_t dmem_words;
} eg2_otbn_app_t;

/** The application generated as eg2_otbn_<file>.h with `--name Name`. */
#define EG2_OTBN_APP(Name)                                                      \
  ((eg2_otbn_app_t){kEg2Otbn##Name##Imem, kEg2Otbn##Name##ImemWords, kEg2Otbn##Name##Dmem, \
                    kEg2Otbn##Name##DmemWords})

uint32_t eg2_otbn_reg(uint32_t off);
void eg2_otbn_reg_wr(uint32_t off, uint32_t v);

/** Waits for STATUS idle; false after `polls` reads. */
bool eg2_otbn_wait_idle(uint32_t polls);

/** Copies the application into IMEM and DMEM (DMEM's rest zero up to `dmem_clear` bytes). */
void eg2_otbn_load(const eg2_otbn_app_t *app, uint32_t dmem_clear);

/** The image sizes in bytes. */
uint32_t eg2_otbn_imem_bytes(const eg2_otbn_app_t *app);

/** EXECUTE, then waits for idle; returns ERR_BITS (0xffffffff on a timeout). */
uint32_t eg2_otbn_run(uint32_t polls);

/** Reads `n` words of DMEM from byte address `addr`. */
void eg2_otbn_dmem_read(uint32_t addr, uint32_t *out, uint32_t n);
/** A DMEM or IMEM word. */
uint32_t eg2_otbn_dmem_word(uint32_t addr);
uint32_t eg2_otbn_imem_word(uint32_t addr);

/** A secure wipe command (SEC_WIPE_DMEM / _IMEM), then waits for idle. */
bool eg2_otbn_sec_wipe(uint32_t cmd, uint32_t polls);

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_MEM_EG2_OTBN_H_
