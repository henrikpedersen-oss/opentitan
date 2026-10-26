// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_pbf/eg2_pbf.h"

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "sw/device/lib/base/multibits.h"
#include "sw/device/lib/testing/rstmgr_testutils.h"
#include "sw/device/silicon_creator/lib/base/chip.h"
#include "sw/device/silicon_creator/lib/boot_log.h"
#include "sw/device/silicon_creator/lib/drivers/retention_sram.h"

// Few globals: the CHERIoT build's globals stay under 4 KiB (REQ_STE_12).
static const char *pbf_area = "pbf";
static bool pbf_corrupt;

enum {
  // The test's words: the last 64 of the owner area (2 KiB at offset 2 KiB of retention SRAM),
  // a magic, the boot count, then kEg2PbfRetWords for the test.
  kRetBaseWord = (2048 + 2048) / 4 - 64,
  kRetMagic = 0x45473250,  // "EG2P"
  kClassStride = ALERT_HANDLER_CLASSB_REGWEN_REG_OFFSET - ALERT_HANDLER_CLASSA_REGWEN_REG_OFFSET,
};

static void put_head(const char *kind, const char *what) {
  eg2_puts("PBF ");
  eg2_puts(kind);
  eg2_puts(" ");
  eg2_puts(pbf_area);
  eg2_puts(": ");
  eg2_puts(what);
}

void eg2_pbf_begin(const char *area, bool inject_fail) {
  pbf_area = area;
  pbf_corrupt = inject_fail;
  put_head("test", eg2_pbf_cheriot() ? "CHERIoT mode" : "RISC-V mode");
  if (inject_fail) {
    eg2_puts(" (inject-fail build: the first check expects a wrong value and must fail)");
  }
  eg2_puts("\r\n");
}

bool eg2_pbf_step(const char *what, bool ok) {
  put_head(ok ? "ok" : "FAIL", what);
  eg2_puts("\r\n");
  return ok;
}

/** Whether this compare is the corrupted one (the first of an inject-fail build). */
static bool take_corruption(void) {
  bool c = pbf_corrupt;
  pbf_corrupt = false;
  return c;
}

bool eg2_pbf_check(const char *what, uint32_t expected, uint32_t got) {
  bool corrupt = take_corruption();
  uint32_t exp = corrupt ? expected ^ 1u : expected;
  bool ok = exp == got;
  put_head(ok ? "ok" : "FAIL", what);
  eg2_puts(" expected 0x");
  eg2_puthex(exp);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  if (corrupt) {
    eg2_puts(" (expected value corrupted: EG2_PBF_INJECT_FAIL)");
  }
  eg2_puts("\r\n");
  return ok;
}

bool eg2_pbf_check_bytes(const char *what, const uint8_t *expected, const uint8_t *got,
                         size_t len) {
  bool corrupt = take_corruption();
  size_t bad = len;
  for (size_t i = 0; i < len; ++i) {
    uint8_t e = (corrupt && i == 0) ? (uint8_t)(expected[i] ^ 1u) : expected[i];
    if (e != got[i]) {
      bad = i;
      break;
    }
  }
  bool ok = bad == len && !(corrupt && len == 0);
  put_head(ok ? "ok" : "FAIL", what);
  eg2_puts(" (");
  eg2_putdec((uint32_t)len);
  eg2_puts(" bytes)");
  if (!ok && bad < len) {
    eg2_puts(" first difference at byte ");
    eg2_putdec((uint32_t)bad);
    eg2_puts(": expected 0x");
    eg2_puthex((corrupt && bad == 0) ? (uint32_t)(expected[bad] ^ 1u) : expected[bad]);
    eg2_puts(" got 0x");
    eg2_puthex(got[bad]);
  }
  if (corrupt) {
    eg2_puts(" (expected value corrupted: EG2_PBF_INJECT_FAIL)");
  }
  eg2_puts("\r\n");
  return ok;
}

bool eg2_pbf_check_words(const char *what, const uint32_t *expected, const uint32_t *got,
                         size_t n) {
  bool corrupt = take_corruption();
  size_t bad = n;
  for (size_t i = 0; i < n; ++i) {
    uint32_t e = (corrupt && i == 0) ? expected[i] ^ 1u : expected[i];
    if (e != got[i]) {
      bad = i;
      break;
    }
  }
  bool ok = bad == n && !(corrupt && n == 0);
  put_head(ok ? "ok" : "FAIL", what);
  eg2_puts(" (");
  eg2_putdec((uint32_t)n);
  eg2_puts(" words)");
  if (!ok && bad < n) {
    eg2_puts(" first difference at word ");
    eg2_putdec((uint32_t)bad);
    eg2_puts(": expected 0x");
    eg2_puthex((corrupt && bad == 0) ? expected[bad] ^ 1u : expected[bad]);
    eg2_puts(" got 0x");
    eg2_puthex(got[bad]);
  }
  if (corrupt) {
    eg2_puts(" (expected value corrupted: EG2_PBF_INJECT_FAIL)");
  }
  eg2_puts("\r\n");
  return ok;
}

void eg2_pbf_note(const char *what, uint32_t value) {
  put_head("note", what);
  eg2_puts(" 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

void eg2_pbf_activity(const char *name, uint32_t count) {
  put_head("activity", name);
  eg2_puts(" ");
  eg2_putdec(count);
  eg2_puts("\r\n");
}

void eg2_pbf_gap(const char *what, const char *why) {
  put_head("gap", what);
  eg2_puts(": ");
  eg2_puts(why);
  eg2_puts("\r\n");
}

void eg2_pbf_model_gap(const char *what) {
  put_head("model_gap", what);
  eg2_puts(" (skipped: model_gaps = 1, the SystemC model does not model it)\r\n");
}

uint32_t eg2_pbf_boot_log_word(uint32_t index) {
  const boot_log_t *log = &retention_sram_get()->creator.boot_log;
  if (log->identifier != kBootLogIdentifier || index >= sizeof(boot_log_t) / 4) {
    return 0;
  }
  return ((const volatile uint32_t *)log)[index];
}

eg2_pbf_boot_stage_t eg2_pbf_boot_stage(void) {
  retention_sram_t *rr = retention_sram_get();
  const uint32_t n = sizeof(rr->creator.reserved) / sizeof(rr->creator.reserved[0]);
  eg2_pbf_boot_stage_t stage = kEg2PbfBootUnknown;
  if (rr->creator.reserved[n - 1] == TEST_ROM_IDENTIFIER) {
    stage = kEg2PbfBootTestRom;
  } else if (rr->creator.boot_log.identifier == kBootLogIdentifier) {
    stage = kEg2PbfBootRomExt;
  }
  put_head("boot", stage == kEg2PbfBootRomExt   ? "ROM_EXT (behind secure boot)"
                   : stage == kEg2PbfBootTestRom ? "test ROM (not run behind secure boot)"
                                                 : "unknown boot stage");
  if (stage == kEg2PbfBootRomExt) {
    eg2_puts(", ROM_EXT slot 0x");
    eg2_puthex(rr->creator.boot_log.rom_ext_slot);
    eg2_puts(", BL0 slot 0x");
    eg2_puthex(rr->creator.boot_log.bl0_slot);
  }
  eg2_puts("\r\n");
  return stage;
}

static volatile uint32_t *ret_word(uint32_t i) {
  return (volatile uint32_t *)retention_sram_get() + kRetBaseWord + i;
}

uint32_t eg2_pbf_reset_reason(void) { return rstmgr_testutils_reason_get(); }

uint32_t eg2_pbf_boot_index(void) {
  uint32_t reason = eg2_pbf_reset_reason();
  uint32_t boot;
  if ((reason & (1u << RSTMGR_RESET_INFO_POR_BIT)) || *ret_word(0) != kRetMagic) {
    *ret_word(0) = kRetMagic;
    for (uint32_t i = 1; i < 2 + kEg2PbfRetWords; ++i) {
      *ret_word(i) = 0;
    }
    boot = 0;
  } else {
    boot = *ret_word(1) + 1;
  }
  *ret_word(1) = boot;
  put_head("boot index", "");
  eg2_putdec(boot);
  eg2_puts(", reset reason 0x");
  eg2_puthex(reason);
  eg2_puts("\r\n");
  return boot;
}

volatile uint32_t *eg2_pbf_ret(uint32_t i) {
  return ret_word(2 + (i < kEg2PbfRetWords ? i : kEg2PbfRetWords - 1));
}

void eg2_pbf_sw_reset(void) {
  put_head("reset", "software reset requested\r\n");
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_REQ_REG_OFFSET, kMultiBitBool4True);
  while (true) {
    __asm__ volatile("wfi");
  }
}

static void ah_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

static uint32_t class_reg(eg2_pbf_class_t cls, uint32_t class_a_offset) {
  return class_a_offset + (uint32_t)cls * kClassStride;
}

void eg2_pbf_escalate(const eg2_pbf_esc_t *esc, const uint32_t *alerts, size_t num_alerts) {
  for (size_t i = 0; i < num_alerts; ++i) {
    ah_write(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * alerts[i], (uint32_t)esc->cls);
    ah_write(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * alerts[i], 1);
  }
  eg2_pbf_class_t c = esc->cls;
  ah_write(class_reg(c, ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET), esc->accum_thresh);
  ah_write(class_reg(c, ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET), 0);
  static const uint32_t kPhase[4] = {
      ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET,
      ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET,
  };
  for (uint32_t p = 0; p < 4; ++p) {
    ah_write(class_reg(c, kPhase[p]), esc->phase_cycles[p]);
  }
  ah_write(class_reg(c, ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET),
           esc->crashdump_phase);
  uint32_t ctrl = (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
                  (esc->lock ? 1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_LOCK_BIT : 0) |
                  ((esc->signal_en & 0xf) << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E0_BIT) |
                  ((esc->map[0] & 3) << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E0_OFFSET) |
                  ((esc->map[1] & 3) << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E1_OFFSET) |
                  ((esc->map[2] & 3) << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E2_OFFSET) |
                  ((esc->map[3] & 3) << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E3_OFFSET);
  ah_write(class_reg(c, ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET), ctrl);
}

uint32_t eg2_pbf_class_state(eg2_pbf_class_t cls) {
  return eg2_dev_read32(kEg2DevAlertHandler,
                        class_reg(cls, ALERT_HANDLER_CLASSA_STATE_REG_OFFSET)) & 0x7;
}

uint32_t eg2_pbf_class_accum(eg2_pbf_class_t cls) {
  return eg2_dev_read32(kEg2DevAlertHandler,
                        class_reg(cls, ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET)) & 0xffff;
}

void eg2_pbf_class_clear(eg2_pbf_class_t cls) {
  ah_write(class_reg(cls, ALERT_HANDLER_CLASSA_CLR_SHADOWED_REG_OFFSET), 1);
}

void eg2_pbf_spin(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; ++i) {
  }
}
