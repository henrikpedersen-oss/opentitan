// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_sec_locks_after_switch and _cheriot (EG2 verification spec REQ_SEC_02, "Security in
 * RISC-V mode"): once ROM and ROM_EXT have handed over, every lock they set stays set until reset,
 * in both modes, and the CHERIoT switch clears none of them. Behind the full secure-boot chain
 * (sim_dv_rom_ext); in the `_cheriot` build every read happens after the switch and lock.
 *
 * The locks ROM_EXT documents setting before the owner image (rom_ext.c, rom_ext_boot and
 * rom_ext_start; secure boot "ROM_EXT" step 7; logical security model "Execution Assumptions"),
 * each read back and then written:
 *   - RRAM controller: ROM_EXT's own slots read-only and locked (REGION_CFG_REGWEN_0/1 = 0,
 *     rom_ext_nvm_protect_self); the Creator info pages locked (INFO_REGWEN_0, _5, _6, _7 = 0:
 *     FactoryId, CreatorSecret, OwnerSecret, WaferAuthSecret, nvm_ctrl_creator_info_pages_lockdown);
 *   - OTP: the Creator software configuration read-locked (CREATOR_SW_CFG_READ_LOCK = 0, a read of
 *     it through the software window is refused), and "the OTP direct access interface until the
 *     next reset" (DIRECT_ACCESS_REGWEN = 0);
 *   - key manager: LOAD_ROOT_KEY_LOCK set, ENFORCE_SW_BINDING set;
 *   - the core's address translation windows locked (IBUS_REGWEN_0/1, DBUS_REGWEN_0/1 = 0,
 *     ibex_addr_remap_lockdown);
 *   - RISC-V mode: every ePMP entry with L set keeps its configuration and address.
 * The CHERIoT lock itself: the status module's second lock (in every CHERIoT test).
 * Each guarded register is written with another value (`wclass` 0 random, 1 all ones, 2 one bit
 * flipped) and must read back unchanged; its lock must still read locked. `order` 1 walks the
 * table backwards.
 *
 * Built with EG2_SEC_LOCKS_AFTER_SWITCH_INJECT_FAIL the first guarded register is expected to
 * take the write: the run must fail with `EG2CHK FAIL RRAM MP_REGION_CFG_0 unchanged`.
 */

#include "hw/top/keymgr_dpe_regs.h"    // Generated.
#include "hw/top/otp_ctrl_regs.h"      // Generated.
#include "hw/top/rram_ctrl_regs.h"     // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/tests/eg2_boot/eg2_boot.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();

#define LOCKS_PARAMS(X) \
  X(order, 0, 0, 1)     \
  X(wclass, 0, 0, 2)
EG2_RM_PARAMS(LOCKS_PARAMS)

typedef struct lock_row {
  uint8_t dev;         // eg2_dev_id_t
  uint8_t locked_val;  // the lock register's value when locked
  uint16_t lock_off;   // the lock (REGWEN) register
  uint16_t reg_off;    // the register it guards
} lock_row_t;

enum { kRows = 16, kLabel = 48 };

static const lock_row_t kRowsTable[kRows] = {
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET,
     RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET,
     RRAM_CTRL_MP_REGION_0_REG_OFFSET},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4,
     RRAM_CTRL_MP_REGION_CFG_0_REG_OFFSET + 4},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_REGION_CFG_REGWEN_0_REG_OFFSET + 4,
     RRAM_CTRL_MP_REGION_0_REG_OFFSET + 4},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_INFO_REGWEN_0_REG_OFFSET,
     RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_INFO_REGWEN_0_REG_OFFSET + 4 * 5,
     RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * 5},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_INFO_REGWEN_0_REG_OFFSET + 4 * 6,
     RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * 6},
    {kEg2DevRramCtrlCore, 0, RRAM_CTRL_INFO_REGWEN_0_REG_OFFSET + 4 * 7,
     RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * 7},
    {kEg2DevOtpCtrlCore, 0, OTP_CTRL_CREATOR_SW_CFG_READ_LOCK_REG_OFFSET,
     OTP_CTRL_CREATOR_SW_CFG_READ_LOCK_REG_OFFSET},
    {kEg2DevOtpCtrlCore, 0, OTP_CTRL_DIRECT_ACCESS_REGWEN_REG_OFFSET,
     OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET},
    {kEg2DevKeymgrDpe, 1, KEYMGR_DPE_LOAD_ROOT_KEY_LOCK_REG_OFFSET,
     KEYMGR_DPE_LOAD_ROOT_KEY_LOCK_REG_OFFSET},
    {kEg2DevKeymgrDpe, kMultiBitBool4True, KEYMGR_DPE_ENFORCE_SW_BINDING_REG_OFFSET,
     KEYMGR_DPE_ENFORCE_SW_BINDING_REG_OFFSET},
    {kEg2DevRvCoreIbexCfg, 0, RV_CORE_IBEX_IBUS_REGWEN_0_REG_OFFSET,
     RV_CORE_IBEX_IBUS_REMAP_ADDR_0_REG_OFFSET},
    {kEg2DevRvCoreIbexCfg, 0, RV_CORE_IBEX_IBUS_REGWEN_0_REG_OFFSET + 4,
     RV_CORE_IBEX_IBUS_REMAP_ADDR_0_REG_OFFSET + 4},
    {kEg2DevRvCoreIbexCfg, 0, RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET,
     RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET},
    {kEg2DevRvCoreIbexCfg, 0, RV_CORE_IBEX_DBUS_REGWEN_0_REG_OFFSET + 4,
     RV_CORE_IBEX_DBUS_REMAP_ADDR_0_REG_OFFSET + 4},
};

static const char kLabels[kRows][kLabel] = {
    "RRAM MP_REGION_CFG_0 (ROM_EXT slot A)", "RRAM MP_REGION_0 (ROM_EXT slot A)",
    "RRAM MP_REGION_CFG_1 (ROM_EXT slot B)", "RRAM MP_REGION_1 (ROM_EXT slot B)",
    "RRAM INFO_PAGE_CFG_0 (FactoryId)",      "RRAM INFO_PAGE_CFG_5 (CreatorSecret)",
    "RRAM INFO_PAGE_CFG_6 (OwnerSecret)",    "RRAM INFO_PAGE_CFG_7 (WaferAuthSecret)",
    "OTP CREATOR_SW_CFG_READ_LOCK",          "OTP DIRECT_ACCESS_ADDRESS",
    "keymgr LOAD_ROOT_KEY_LOCK",             "keymgr ENFORCE_SW_BINDING",
    "ibex IBUS_REMAP_ADDR_0",                "ibex IBUS_REMAP_ADDR_1",
    "ibex DBUS_REMAP_ADDR_0",                "ibex DBUS_REMAP_ADDR_1",
};

static uint32_t other_value(uint32_t v) {
  switch (EG2_PARAM(wclass)) {
    case 1:
      return v == 0xffffffffu ? 0 : 0xffffffffu;
    case 2:
      return v ^ (1u << eg2_rand_range(0, 31));
    default: {
      uint32_t r = eg2_rand();
      return r == v ? ~v : r;
    }
  }
}

static void say(const char *what, const char *label) {
  eg2_puts("EG2 ");
  eg2_puts(what);
  eg2_puts(" ");
  eg2_puts(label);
  eg2_puts("\r\n");
}

static bool check_row(uint32_t i, bool first) {
  const lock_row_t *r = &kRowsTable[i];
  eg2_dev_id_t dev = (eg2_dev_id_t)r->dev;
  say("lock", kLabels[i]);
  uint32_t lock = eg2_dev_read32(dev, r->lock_off);
  bool ok = eg2_boot_check("  locked by ROM / ROM_EXT", r->locked_val, lock);
  uint32_t before = eg2_dev_read32(dev, r->reg_off);
  uint32_t value = other_value(before);
  eg2_dev_write32(dev, r->reg_off, value);
  uint32_t after = eg2_dev_read32(dev, r->reg_off);
#ifdef EG2_SEC_LOCKS_AFTER_SWITCH_INJECT_FAIL
  if (first) {
    before = value;
  }
#endif
  (void)first;
  eg2_puts("EG2 ");
  eg2_puts(kLabels[i]);
  eg2_puts("\r\n");
  ok = eg2_boot_check(i == 0 ? "RRAM MP_REGION_CFG_0 unchanged" : "  guarded register unchanged",
                      before, after) &&
       ok;
  ok = eg2_boot_check("  still locked after the write", r->locked_val,
                      eg2_dev_read32(dev, r->lock_off)) &&
       ok;
  return ok;
}

#ifndef __CHERIOT__
/** RISC-V mode: every locked ePMP entry keeps its configuration and address. */
static bool epmp_locked_entries(void) {
  bool ok = true;
  uint32_t cfg[4];
  __asm__ volatile("csrr %0, pmpcfg0" : "=r"(cfg[0]));
  __asm__ volatile("csrr %0, pmpcfg1" : "=r"(cfg[1]));
  __asm__ volatile("csrr %0, pmpcfg2" : "=r"(cfg[2]));
  __asm__ volatile("csrr %0, pmpcfg3" : "=r"(cfg[3]));
  for (uint32_t i = 0; i < 16; ++i) {
    uint32_t c = (cfg[i / 4] >> (8 * (i % 4))) & 0xff;
    eg2_boot_note("ePMP entry", i);
    eg2_boot_note("  pmpcfg", c);
    if (!(c & 0x80)) {
      continue;
    }
    // A locked entry: try to clear its configuration byte.
    uint32_t mask = 0xffu << (8 * (i % 4));
    uint32_t before[4] = {cfg[0], cfg[1], cfg[2], cfg[3]};
    switch (i / 4) {
      case 0:
        __asm__ volatile("csrc pmpcfg0, %0" ::"r"(mask));
        __asm__ volatile("csrr %0, pmpcfg0" : "=r"(cfg[0]));
        break;
      case 1:
        __asm__ volatile("csrc pmpcfg1, %0" ::"r"(mask));
        __asm__ volatile("csrr %0, pmpcfg1" : "=r"(cfg[1]));
        break;
      case 2:
        __asm__ volatile("csrc pmpcfg2, %0" ::"r"(mask));
        __asm__ volatile("csrr %0, pmpcfg2" : "=r"(cfg[2]));
        break;
      default:
        __asm__ volatile("csrc pmpcfg3, %0" ::"r"(mask));
        __asm__ volatile("csrr %0, pmpcfg3" : "=r"(cfg[3]));
        break;
    }
    ok = eg2_boot_check("  locked ePMP entry unchanged", before[i / 4], cfg[i / 4]) && ok;
  }
  return ok;
}
#endif

bool eg2_test_main(void) {
  bool ok = eg2_rm_params_begin();
  ok = eg2_boot_log_check(kEg2BootSlotA, kEg2BootSlotA) && ok;
  eg2_boot_irq_off();
  for (uint32_t n = 0; n < kRows; ++n) {
    uint32_t i = EG2_PARAM(order) == 0 ? n : kRows - 1 - n;
    ok = check_row(i, i == 0) && ok;
  }
  // The read lock in effect: a read of CREATOR_SW_CFG through the software window is refused.
  eg2_boot_out_t out;
  uint32_t cause = eg2_boot_probe(TOP_EARLGREY_OTP_CTRL_CORE_BASE_ADDR +
                                      OTP_CTRL_SW_CFG_WINDOW_REG_OFFSET +
                                      OTP_CTRL_PARAM_CREATOR_SW_CFG_OFFSET,
                                  kEg2BootLoad, 0, &out);
  ok = eg2_boot_check("CREATOR_SW_CFG read through the window refused", kEg2CauseLoadFault,
                      cause) &&
       ok;
#ifndef __CHERIOT__
  ok = epmp_locked_entries() && ok;
#endif
  return eg2_boot_summary() && ok;
}
