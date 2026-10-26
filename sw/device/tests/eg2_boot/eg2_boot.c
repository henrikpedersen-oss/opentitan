// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_boot/eg2_boot.h"

#include <assert.h>

#include "hw/top/keymgr_dpe_regs.h"  // Generated.
#include "hw/top/lc_ctrl_regs.h"     // Generated.
#include "hw/top/otp_ctrl_regs.h"    // Generated.
#include "hw/top/rram_ctrl_regs.h"   // Generated.
#include "hw/top/rstmgr_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/base/multibits.h"
#include "sw/device/silicon_creator/lib/boot_log.h"

// ------------------------------------------------------------------------------------------------
// Checks

static uint32_t checks_made;
static uint32_t checks_failed;

bool eg2_boot_expect(const char *what, bool cond, uint32_t got) {
  ++checks_made;
  if (cond) {
    eg2_puts("EG2CHK ok ");
    eg2_puts(what);
    eg2_puts(" 0x");
    eg2_puthex(got);
    eg2_puts("\r\n");
    return true;
  }
  ++checks_failed;
  eg2_puts("EG2CHK FAIL ");
  eg2_puts(what);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  eg2_puts("\r\n");
  return false;
}

bool eg2_boot_check(const char *what, uint32_t exp, uint32_t got) {
  ++checks_made;
  if (exp == got) {
    eg2_puts("EG2CHK ok ");
    eg2_puts(what);
    eg2_puts(" 0x");
    eg2_puthex(got);
    eg2_puts("\r\n");
    return true;
  }
  ++checks_failed;
  eg2_puts("EG2CHK FAIL ");
  eg2_puts(what);
  eg2_puts(" exp 0x");
  eg2_puthex(exp);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  eg2_puts("\r\n");
  return false;
}

void eg2_boot_note(const char *what, uint32_t value) {
  eg2_puts("EG2 ");
  eg2_puts(what);
  eg2_puts(" 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

bool eg2_boot_summary(void) {
  eg2_puts("EG2CHK checks ");
  eg2_putdec(checks_made);
  eg2_puts(", failed ");
  eg2_putdec(checks_failed);
  eg2_puts("\r\n");
  if (checks_made == 0) {
    eg2_puts("EG2CHK FAIL no check was made\r\n");
    return false;
  }
  return checks_failed == 0;
}

// ------------------------------------------------------------------------------------------------
// Probes (eg2_boot_probe.S)

uint32_t eg2_boot_probe_asm(volatile void *ptr, uint32_t op, uint32_t wdata, eg2_boot_out_t *out);

uint32_t eg2_boot_probe(uint32_t addr, eg2_boot_op_t op, uint32_t wdata, eg2_boot_out_t *out) {
#ifdef __CHERIOT__
  if (op == kEg2BootFetch) {
    // The executable root's bounds: the whole address space.
    return eg2_boot_probe_cap(eg2_boot_code_cap(0, 0xffffffffu, addr), op, wdata, out);
  }
#endif
  return eg2_boot_probe_asm(OT_MMIO_ADDR_TO_PTR(addr), op, wdata, out);
}

#ifdef __CHERIOT__
uint32_t eg2_boot_probe_cap(void *target, eg2_boot_op_t op, uint32_t wdata, eg2_boot_out_t *out) {
  return eg2_boot_probe_asm(target, op, wdata, out);
}

void *eg2_boot_code_cap(uint32_t base, uint32_t len, uint32_t addr) {
  void *pcc;
  __asm__ volatile("auipcc %0, 0" : "=C"(pcc));
  pcc = __builtin_cheri_address_set(pcc, base);
  if (len != 0xffffffffu) {
    pcc = __builtin_cheri_bounds_set_exact(pcc, len);
  }
  return __builtin_cheri_address_set(pcc, addr);
}
#endif

// ------------------------------------------------------------------------------------------------
// Retention SRAM, boot log, state

volatile retention_sram_t *eg2_boot_retram(void) {
  return (volatile retention_sram_t *)OT_MMIO_ADDR_TO_PTR(
      TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR);
}

volatile eg2_boot_state_t *eg2_boot_state(void) {
  static_assert(sizeof(eg2_boot_state_t) <= sizeof(retention_sram_owner_t),
                "the test's state must fit the owner area");
  volatile eg2_boot_state_t *s =
      (volatile eg2_boot_state_t *)&eg2_boot_retram()->owner.reserved[0];
  // After a power-on reset the owner area holds whatever the SRAM powered up with, and the test
  // ROM skips the retention SRAM's initialisation: reading it gives an integrity error (an NMI on
  // the chip; eg2sim has no integrity, regr-vlt 2026-10-25). So on a power-on reset the state is
  // written before it is ever read. The reset reasons are the word the test ROM stored, valid.
  bool por = (eg2_boot_retram()->creator.reset_reasons & 0x1u) != 0;  // kDifRstmgrResetInfoPor
  if (por || s->magic != kEg2BootStateMagic) {
    s->magic = kEg2BootStateMagic;
    s->step = 0;
    s->boots = 0;
    s->fails = 0;
    for (size_t i = 0; i < sizeof(s->data) / sizeof(s->data[0]); ++i) {
      s->data[i] = 0;
    }
  }
  s->boots = s->boots + 1;
  eg2_boot_note("boot", s->boots);
  eg2_boot_note("step", s->step);
  return s;
}

void eg2_boot_sw_reset(void) {
  eg2_puts("EG2 software reset\r\n");
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_REQ_REG_OFFSET,
                  kMultiBitBool4True);
  while (true) {
    __asm__ volatile("wfi");
  }
}

uint32_t eg2_boot_reset_info(void) {
  return eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET);
}

bool eg2_boot_log_check(uint32_t exp_rom_ext_slot, uint32_t exp_bl0_slot) {
  volatile boot_log_t *log = &eg2_boot_retram()->creator.boot_log;
  eg2_boot_note("boot_log rom_ext_slot", log->rom_ext_slot);
  eg2_boot_note("boot_log rom_ext_version", (log->rom_ext_major << 16) | log->rom_ext_minor);
  eg2_boot_note("boot_log bl0_slot", log->bl0_slot);
  eg2_boot_note("boot_log ownership_state", log->ownership_state);
  eg2_boot_note("boot_log ownership_transfers", log->ownership_transfers);
  eg2_boot_note("boot_log rom_ext_min_sec_ver", log->rom_ext_min_sec_ver);
  eg2_boot_note("boot_log bl0_min_sec_ver", log->bl0_min_sec_ver);
  eg2_boot_note("boot_log primary_bl0_slot", log->primary_bl0_slot);
  bool ok = eg2_boot_check("boot_log identifier (ROM_EXT ran)", kEg2BootLogId, log->identifier);
  ok = eg2_boot_check("boot_log rom_ext_slot", exp_rom_ext_slot, log->rom_ext_slot) && ok;
  ok = eg2_boot_check("boot_log bl0_slot", exp_bl0_slot, log->bl0_slot) && ok;
  return ok;
}

// ------------------------------------------------------------------------------------------------
// RRAM controller

uint32_t eg2_rram_reg(uint32_t offset) {
  return eg2_dev_read32(kEg2DevRramCtrlCore, offset);
}

void eg2_rram_reg_wr(uint32_t offset, uint32_t value) {
  eg2_dev_write32(kEg2DevRramCtrlCore, offset, value);
}

static uint32_t mubi4(bool b) { return b ? kMultiBitBool4True : kMultiBitBool4False; }

uint32_t eg2_rram_info_cfg(uint32_t page, bool rd, bool wr, bool scramble, bool ecc) {
  uint32_t v = (mubi4(true) << RRAM_CTRL_INFO_PAGE_CFG_0_EN_0_OFFSET) |
               (mubi4(rd) << RRAM_CTRL_INFO_PAGE_CFG_0_RD_EN_0_OFFSET) |
               (mubi4(wr) << RRAM_CTRL_INFO_PAGE_CFG_0_WR_EN_0_OFFSET) |
               (mubi4(scramble) << RRAM_CTRL_INFO_PAGE_CFG_0_SCRAMBLE_EN_0_OFFSET) |
               (mubi4(ecc) << RRAM_CTRL_INFO_PAGE_CFG_0_ECC_EN_0_OFFSET);
  eg2_rram_reg_wr(RRAM_CTRL_INFO_PAGE_CFG_0_REG_OFFSET + 4 * page, v);
  return v;
}

eg2_rram_res_t eg2_rram_op(uint32_t op, uint32_t partition, uint32_t addr, uint32_t nwords,
                           uint32_t *data) {
  eg2_rram_res_t res = {0, 0};
  // 1. No operation in flight (a previous one completed: done is cleared below).
  eg2_rram_reg_wr(RRAM_CTRL_ADDR_REG_OFFSET, addr);
  uint32_t ctrl = (1u << RRAM_CTRL_CONTROL_START_BIT) |
                  ((op & RRAM_CTRL_CONTROL_OP_MASK) << RRAM_CTRL_CONTROL_OP_OFFSET) |
                  ((partition & 1u) << RRAM_CTRL_CONTROL_PARTITION_BIT) |
                  (((nwords - 1) & RRAM_CTRL_CONTROL_NUM_MASK) << RRAM_CTRL_CONTROL_NUM_OFFSET);
  eg2_rram_reg_wr(RRAM_CTRL_CONTROL_REG_OFFSET, ctrl);
  // 2. The data, through the FIFO windows, until the operation is done. An operation the
  // controller refuses completes at once with OP_STATUS.err; the FIFOs are then not used.
  uint32_t moved = 0;
  for (uint32_t polls = 0; polls < 100000; ++polls) {
    res.op_status = eg2_rram_reg(RRAM_CTRL_OP_STATUS_REG_OFFSET);
    if (res.op_status & (1u << RRAM_CTRL_OP_STATUS_DONE_BIT)) {
      break;
    }
    uint32_t status = eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET);
    if (op == kEg2RramOpRead) {
      if (moved < nwords && !(status & (1u << RRAM_CTRL_STATUS_RD_EMPTY_BIT))) {
        data[moved++] = eg2_rram_reg(RRAM_CTRL_RD_FIFO_REG_OFFSET);
      }
    } else if (moved < nwords && !(status & (1u << RRAM_CTRL_STATUS_WR_FULL_BIT))) {
      eg2_rram_reg_wr(RRAM_CTRL_WR_FIFO_REG_OFFSET, data[moved++]);
    }
  }
  // Words still in the read FIFO when done was seen.
  while (op == kEg2RramOpRead && moved < nwords &&
         !(eg2_rram_reg(RRAM_CTRL_STATUS_REG_OFFSET) & (1u << RRAM_CTRL_STATUS_RD_EMPTY_BIT))) {
    data[moved++] = eg2_rram_reg(RRAM_CTRL_RD_FIFO_REG_OFFSET);
  }
  res.err_code = eg2_rram_reg(RRAM_CTRL_ERR_CODE_REG_OFFSET);
  eg2_rram_reg_wr(RRAM_CTRL_ERR_CODE_REG_OFFSET, res.err_code);
  eg2_rram_reg_wr(RRAM_CTRL_OP_STATUS_REG_OFFSET, 0);
  return res;
}

// ------------------------------------------------------------------------------------------------
// keymgr_dpe

uint32_t eg2_kmgr_reg(uint32_t offset) { return eg2_dev_read32(kEg2DevKeymgrDpe, offset); }

void eg2_kmgr_reg_wr(uint32_t offset, uint32_t value) {
  eg2_dev_write32(kEg2DevKeymgrDpe, offset, value);
}

void eg2_kmgr_reg_wr_shadowed(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevKeymgrDpe, offset, value);
}

uint32_t eg2_kmgr_state(void) {
  return eg2_kmgr_reg(KEYMGR_DPE_WORKING_STATE_REG_OFFSET) & KEYMGR_DPE_WORKING_STATE_STATE_MASK;
}

uint32_t eg2_kmgr_meta_lo(uint32_t slot) {
  return eg2_kmgr_reg(KEYMGR_DPE_METADATA_LOW_0_REG_OFFSET + 4 * slot);
}

uint32_t eg2_kmgr_meta_hi(uint32_t slot) {
  return eg2_kmgr_reg(KEYMGR_DPE_METADATA_HIGH_0_REG_OFFSET + 4 * slot);
}

uint32_t eg2_kmgr_op(uint32_t op, uint32_t dest, uint32_t src, uint32_t dst, bool sw_binding_only,
                     uint32_t *err) {
  uint32_t ctrl =
      ((op & KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_MASK)
       << KEYMGR_DPE_CONTROL_SHADOWED_OPERATION_OFFSET) |
      ((dest & KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_MASK)
       << KEYMGR_DPE_CONTROL_SHADOWED_DEST_SEL_OFFSET) |
      ((src & KEYMGR_DPE_CONTROL_SHADOWED_SLOT_SRC_SEL_MASK)
       << KEYMGR_DPE_CONTROL_SHADOWED_SLOT_SRC_SEL_OFFSET) |
      ((dst & KEYMGR_DPE_CONTROL_SHADOWED_SLOT_DST_SEL_MASK)
       << KEYMGR_DPE_CONTROL_SHADOWED_SLOT_DST_SEL_OFFSET) |
      ((sw_binding_only ? 1u : 0u) << KEYMGR_DPE_CONTROL_SHADOWED_SW_BINDING_ONLY_BIT);
  eg2_kmgr_reg_wr_shadowed(KEYMGR_DPE_CONTROL_SHADOWED_REG_OFFSET, ctrl);
  eg2_kmgr_reg_wr(KEYMGR_DPE_START_REG_OFFSET, 1u << KEYMGR_DPE_START_EN_BIT);
  uint32_t status = 1;
  for (uint32_t polls = 0; polls < 1000000; ++polls) {
    status = eg2_kmgr_reg(KEYMGR_DPE_OP_STATUS_REG_OFFSET) & KEYMGR_DPE_OP_STATUS_STATUS_MASK;
    if (status != KEYMGR_DPE_OP_STATUS_STATUS_VALUE_WIP &&
        status != KEYMGR_DPE_OP_STATUS_STATUS_VALUE_IDLE) {
      break;
    }
  }
  // OP_STATUS is rw1c: clear it for the next operation.
  eg2_kmgr_reg_wr(KEYMGR_DPE_OP_STATUS_REG_OFFSET, status);
  uint32_t e = eg2_kmgr_reg(KEYMGR_DPE_ERR_CODE_REG_OFFSET);
  eg2_kmgr_reg_wr(KEYMGR_DPE_ERR_CODE_REG_OFFSET, e);
  if (err != NULL) {
    *err = e;
  }
  return status;
}

void eg2_kmgr_sw_output(uint32_t share0[8], uint32_t share1[8]) {
  for (uint32_t i = 0; i < 8; ++i) {
    share0[i] = eg2_kmgr_reg(KEYMGR_DPE_SW_SHARE0_OUTPUT_0_REG_OFFSET + 4 * i);
    share1[i] = eg2_kmgr_reg(KEYMGR_DPE_SW_SHARE1_OUTPUT_0_REG_OFFSET + 4 * i);
  }
}

void eg2_boot_note_key(const char *what, const uint32_t key[8]) {
  eg2_puts("EG2 ");
  eg2_puts(what);
  for (uint32_t i = 0; i < 8; ++i) {
    eg2_puts(" ");
    eg2_puthex(key[i]);
  }
  eg2_puts("\r\n");
}

bool eg2_boot_key_eq(const uint32_t a[8], const volatile uint32_t b[8]) {
  uint32_t diff = 0;
  for (uint32_t i = 0; i < 8; ++i) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

static bool chain_gen(const uint32_t salt[8], uint32_t slot, uint32_t key[8]) {
  eg2_kmgr_reg_wr(KEYMGR_DPE_KEY_VERSION_REG_OFFSET, 0);
  for (uint32_t i = 0; i < 8; ++i) {
    eg2_kmgr_reg_wr(KEYMGR_DPE_SALT_0_REG_OFFSET + 4 * i, salt[i]);
  }
  bool ok = eg2_boot_check("chain: generate SW key", kEg2KmgrDone,
                           eg2_kmgr_op(kEg2KmgrOpGenSw, 0, slot, 0, false, NULL));
  uint32_t s0[8], s1[8];
  eg2_kmgr_sw_output(s0, s1);
  for (uint32_t i = 0; i < 8; ++i) {
    key[i] = s0[i] ^ s1[i];
  }
  return ok;
}

bool eg2_kmgr_chain(uint32_t slot, const uint32_t binding[8], const uint32_t salt[8],
                    uint32_t key1[8], uint32_t key2[8]) {
  enum {
    kChild = 1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_CHILD_BIT,
    kKeygen = 1u << KEYMGR_DPE_SLOT_POLICY_ALLOW_KEYGEN_BIT,
  };
  eg2_kmgr_reg_wr_shadowed(KEYMGR_DPE_MAX_KEY_VER_SHADOWED_REG_OFFSET, 0);
  uint32_t first = eg2_kmgr_state() == kEg2KmgrReset ? kEg2KmgrOpAdvance : kEg2KmgrOpLoadRoot;
  bool ok = eg2_boot_check("chain: UDS latched", kEg2KmgrDone,
                           eg2_kmgr_op(first, 0, 0, slot, false, NULL));
  for (uint32_t i = 0; i < 8; ++i) {
    eg2_kmgr_reg_wr(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + 4 * i, binding[i]);
  }
  eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, kChild | kKeygen);
  ok = eg2_boot_check("chain: advance to stage 1", kEg2KmgrDone,
                      eg2_kmgr_op(kEg2KmgrOpAdvance, 0, slot, slot, false, NULL)) &&
       ok;
  ok = chain_gen(salt, slot, key1) && ok;
  for (uint32_t i = 0; i < 8; ++i) {
    eg2_kmgr_reg_wr(KEYMGR_DPE_SW_BINDING_0_REG_OFFSET + 4 * i, binding[i]);
  }
  eg2_kmgr_reg_wr(KEYMGR_DPE_SLOT_POLICY_REG_OFFSET, kKeygen);
  ok = eg2_boot_check("chain: advance to stage 2", kEg2KmgrDone,
                      eg2_kmgr_op(kEg2KmgrOpAdvance, 0, slot, slot, false, NULL)) &&
       ok;
  ok = chain_gen(salt, slot, key2) && ok;
  return ok;
}

// ------------------------------------------------------------------------------------------------
// OTP DAI

uint32_t eg2_otp_dai(uint32_t cmd, uint32_t addr, const uint32_t wdata[2], uint32_t rdata[2]) {
  if (rdata != NULL) {
    rdata[0] = 0;
    rdata[1] = 0;
  }
  if (eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_REGWEN_REG_OFFSET) == 0) {
    return 0xff;
  }
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_ADDRESS_REG_OFFSET, addr);
  if (wdata != NULL) {
    eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_WDATA_0_REG_OFFSET, wdata[0]);
    eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_WDATA_1_REG_OFFSET, wdata[1]);
  }
  eg2_dev_write32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_CMD_REG_OFFSET, cmd);
  for (uint32_t polls = 0; polls < 100000; ++polls) {
    if (eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_STATUS_REG_OFFSET) &
        (1u << OTP_CTRL_STATUS_DAI_IDLE_BIT)) {
      break;
    }
  }
  // The DAI's entry follows the partitions' (otp_ctrl.hjson ERR_CODE: NumPart partitions, then
  // the DAI, then the LCI).
  uint32_t err = eg2_dev_read32(kEg2DevOtpCtrlCore,
                                OTP_CTRL_ERR_CODE_0_REG_OFFSET + 4 * OTP_CTRL_PARAM_NUM_PART) &
                 OTP_CTRL_ERR_CODE_0_ERR_CODE_0_MASK;
  if (rdata != NULL) {
    rdata[0] = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_RDATA_0_REG_OFFSET);
    rdata[1] = eg2_dev_read32(kEg2DevOtpCtrlCore, OTP_CTRL_DIRECT_ACCESS_RDATA_1_REG_OFFSET);
  }
  return err;
}

uint64_t eg2_otp_digest(uint32_t digest0_offset) {
  uint64_t lo = eg2_dev_read32(kEg2DevOtpCtrlCore, digest0_offset);
  uint64_t hi = eg2_dev_read32(kEg2DevOtpCtrlCore, digest0_offset + 4);
  return (hi << 32) | lo;
}

void eg2_otp_dai_clear_error(void) {
  eg2_status_expect_intr(kEg2DevOtpCtrlCore, 1u << OTP_CTRL_INTR_STATE_OTP_ERROR_BIT);
  // A good command replaces the DAI's error code.
  uint32_t r[2];
  (void)eg2_otp_dai(kEg2OtpDaiRead, OTP_CTRL_PARAM_DEVICE_ID_OFFSET, NULL, r);
  // The interrupt stays: the status sweep requires an expected interrupt to be present.
}

// ------------------------------------------------------------------------------------------------
// Life cycle

uint32_t eg2_lc_state(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_STATE_REG_OFFSET);
}

uint32_t eg2_lc_count(void) {
  return eg2_dev_read32(kEg2DevLcCtrlRegs, LC_CTRL_LC_TRANSITION_CNT_REG_OFFSET);
}

uint32_t eg2_lc_state_decoded(void) {
  uint32_t raw = eg2_lc_state();
  uint32_t s = raw & 0x1f;
  for (uint32_t i = 1; i < 6; ++i) {
    if (((raw >> (5 * i)) & 0x1f) != s) {
      return kEg2LcInvalid;
    }
  }
  return s;
}
