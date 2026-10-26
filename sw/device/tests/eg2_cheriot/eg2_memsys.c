// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_cheriot/eg2_memsys.h"

#include "hw/top/alert_handler_regs.h"
#include "hw/top/aon_timer_regs.h"
#include "hw/top/cheriot_mem_sys_regs.h"
#include "hw/top/pwrmgr_regs.h"
#include "hw/top/rram_ctrl_regs.h"
#include "hw/top/rstmgr_regs.h"
#include "hw/top/rv_core_ibex_regs.h"
#include "hw/top/rv_plic_regs.h"
#include "hw/top/sram_ctrl_regs.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/testing/eg2/eg2_params.h"
#include "sw/device/lib/testing/eg2/eg2_result.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"

enum {
  kMuBi4True = 0x6,
  kMuBi4False = 0x9,
};

// --------------------------------------- Bench steps ----------------------------------------

/**
 * Written by the bench (chip_sw_eg2_rand_vseq) during the run: [0] the number of the last bench
 * step done, [1] what that step reports back. In .rodata (the RRAM image), the one place the
 * bench can write; read through a volatile so each poll reads RRAM.
 */
OT_USED OT_SECTION(".rodata") volatile const uint32_t kEg2BenchAck[2] = {0, 0};

static uint32_t bench_ack(uint32_t i) { return kEg2BenchAck[i]; }

uint32_t eg2_ms_bench_result(void) { return bench_ack(1); }

void eg2_ms_note(const char *what, uint32_t v) {
  eg2_puts("EG2 ");
  eg2_puts(what);
  eg2_puts(": 0x");
  eg2_puthex(v);
  eg2_puts("\r\n");
}

bool eg2_ms_bench(const char *op, uint32_t a, uint32_t b, bool bench) {
  // The step number continues from the last acknowledged one, so it keeps counting across the
  // resets of a test (the acknowledgement is in RRAM).
  uint32_t n = bench_ack(0) + 1;
  eg2_puts("EG2 BENCH ");
  eg2_putdec(n);
  eg2_puts(" ");
  eg2_puts(op);
  eg2_puts(" ");
  eg2_puthex(a);
  eg2_puts(" ");
  eg2_puthex(b);
  if (!bench) {
    eg2_puts(" skipped (no bench)\r\n");
    return true;
  }
  eg2_puts("\r\n");
  for (uint32_t i = 0; i < 20000000u; ++i) {
    if (bench_ack(0) == n) {
      return true;
    }
  }
  eg2_puts("EG2 BENCH ");
  eg2_putdec(n);
  eg2_puts(": no answer from the bench\r\n");
  return false;
}

void eg2_ms_dv_status(uint32_t status) {
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET, status);
}

void eg2_ms_spin(uint32_t n) {
  for (volatile uint32_t i = 0; i < n; ++i) {
  }
}

// ------------------------------------------ The map ------------------------------------------

uint32_t eg2_ms_tag_offset(uint32_t addr) {
  if (addr >= kEg2MsHeapBase && addr < kEg2MsHeapTop) {
    return kEg2MsSramTagOffset + (addr - kEg2MsHeapBase) / kEg2MsGranule / 8;
  }
  if (addr >= kEg2MsNvmBase && addr < kEg2MsNvmTop) {
    return kEg2MsNvmTagOffset + (addr - kEg2MsNvmBase) / kEg2MsGranule / 8;
  }
  return 0xffffffffu;
}

bool eg2_ms_tagged(uint32_t addr) { return eg2_ms_tag_offset(addr) != 0xffffffffu; }

bool eg2_ms_revbm_bit(uint32_t addr, uint32_t *word, uint32_t *bit) {
  if (addr < kEg2MsHeapBase || addr >= kEg2MsHeapTop) {
    return false;
  }
  uint32_t g = (addr - kEg2MsHeapBase) / kEg2MsGranule;
  *word = g / 32;
  *bit = g % 32;
  return true;
}

void eg2_ms_write32(uint32_t addr, uint32_t value) {
  volatile uint32_t *p = eg2_mem(addr, 4);
  *p = value;
}

uint32_t eg2_ms_read32(uint32_t addr) {
  volatile uint32_t *p = eg2_mem(addr, 4);
  return *p;
}

// --------------------------------- Capabilities (CHERIoT) -------------------------------------

#ifdef __CHERIOT__

void *eg2_ms_data_cap(uint32_t base, uint32_t len) { return eg2_mem(base, len); }

void *eg2_ms_auth(uint32_t addr) { return eg2_mem(addr & ~7u, 8); }

void eg2_ms_csc(uint32_t addr, void *cap) {
  void *a = eg2_ms_auth(addr);
  __asm__ volatile("csc %1, 0(%0)" : : "C"(a), "C"(cap) : "memory");
}

void *eg2_ms_clc(uint32_t addr) {
  void *a = eg2_ms_auth(addr);
  void *c;
  __asm__ volatile("clc %0, 0(%1)" : "=C"(c) : "C"(a) : "memory");
  return c;
}

uint32_t eg2_ms_cap_tag(void *c) {
  uint32_t t;
  __asm__ volatile("cgettag %0, %1" : "=r"(t) : "C"(c));
  return t;
}

uint32_t eg2_ms_cap_base(void *c) {
  uint32_t b;
  __asm__ volatile("cgetbase %0, %1" : "=r"(b) : "C"(c));
  return b;
}

uint32_t eg2_ms_cap_addr(void *c) {
  uint32_t a;
  // CGetAddr (the assembler turns `cgetaddr` into `mv`).
  __asm__ volatile(".insn r 0x5b, 0, 0x7f, %0, %1, x15" : "=r"(a) : "C"(c));
  return a;
}

uint32_t eg2_ms_cap_high(void *c) {
  uint32_t h;
  __asm__ volatile("cgethigh %0, %1" : "=r"(h) : "C"(c));
  return h;
}

uint32_t eg2_ms_tag(uint32_t addr) { return eg2_ms_cap_tag(eg2_ms_clc(addr)); }

void *eg2_ms_sealing_cap(void) {
  void *c;
  __asm__ volatile("cspecialr %0, mscratchc" : "=C"(c));
  return c;
}

void *eg2_ms_object_cap(uint32_t base, uint32_t len, bool sealed) {
  void *c = eg2_mem(base, len);
  if (sealed) {
    void *s = eg2_ms_sealing_cap();
    __asm__ volatile("csetaddr %0, %0, %1" : "+C"(s) : "r"(9u));
    __asm__ volatile("cseal %0, %0, %1" : "+C"(c) : "C"(s));
  }
  return c;
}

uint32_t eg2_ms_revbm_read(uint32_t i) { return eg2_ms_read32(kEg2MsMetaBase + 4 * i); }

void eg2_ms_revbm_write(uint32_t i, uint32_t value) {
  eg2_ms_write32(kEg2MsMetaBase + 4 * i, value);
}

void eg2_ms_revbm_mark(uint32_t addr, bool revoked) {
  uint32_t w, b;
  if (!eg2_ms_revbm_bit(addr, &w, &b)) {
    return;
  }
  uint32_t v = eg2_ms_revbm_read(w);
  v = revoked ? (v | (1u << b)) : (v & ~(1u << b));
  eg2_ms_revbm_write(w, v);
}

void eg2_ms_revbm_clear(void) {
  for (uint32_t i = 0; i < kEg2MsRevbmSize / 4; ++i) {
    eg2_ms_revbm_write(i, 0);
  }
}

#endif  // __CHERIOT__

// ------------------------------------- Revocation engine --------------------------------------

uint32_t eg2_ms_tbre_epoch(void) {
  return eg2_dev_read32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_EPOCH_REG_OFFSET);
}

uint32_t eg2_ms_tbre_status(void) {
  return eg2_dev_read32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET);
}

void eg2_ms_tbre_clear(void) {
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_STATUS_REG_OFFSET,
                  (1u << CHERIOT_MEM_SYS_TBRE_STATUS_START_ERR_BIT) |
                      (1u << CHERIOT_MEM_SYS_TBRE_STATUS_SWEEP_ERR_BIT));
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_INTR_STATE_REG_OFFSET,
                  1u << CHERIOT_MEM_SYS_INTR_STATE_TBRE_DONE_BIT);
}

void eg2_ms_tbre_start(uint32_t base, uint32_t n_caps, eg2_ms_sweep_t *out) {
  out->epoch_before = eg2_ms_tbre_epoch();
  out->epoch_after = out->epoch_before;
  out->polls = 0;
  out->done = false;
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_BASE_ADDR_REG_OFFSET, base);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_NUM_CAPS_REG_OFFSET, n_caps);
  eg2_dev_write32(kEg2DevCheriotMemSysRegs, CHERIOT_MEM_SYS_TBRE_START_REG_OFFSET, 1);
  out->status = eg2_ms_tbre_status();
  out->started = (out->status & (1u << CHERIOT_MEM_SYS_TBRE_STATUS_START_ERR_BIT)) == 0;
}

void eg2_ms_tbre_wait(eg2_ms_sweep_t *out, uint32_t max_polls) {
  uint32_t s = eg2_ms_tbre_status();
  uint32_t i = 0;
  while ((s & (1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT)) && i < max_polls) {
    s = eg2_ms_tbre_status();
    ++i;
  }
  out->status = s;
  out->polls = i;
  out->done = (s & (1u << CHERIOT_MEM_SYS_TBRE_STATUS_BUSY_BIT)) == 0;
  out->epoch_after = eg2_ms_tbre_epoch();
}

// -------------------------------------- PLIC and timers ---------------------------------------

void eg2_ms_plic_enable(uint32_t id, bool enable) {
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_PRIO_0_REG_OFFSET + 4 * id, enable ? 1 : 0);
  uint32_t off = RV_PLIC_IE0_0_REG_OFFSET + 4 * (id / 32);
  uint32_t v = eg2_dev_read32(kEg2DevRvPlic, off);
  v = enable ? (v | (1u << (id % 32))) : (v & ~(1u << (id % 32)));
  eg2_dev_write32(kEg2DevRvPlic, off, v);
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_THRESHOLD0_REG_OFFSET, 0);
}

bool eg2_ms_plic_pending(uint32_t id) {
  return (eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_IP_0_REG_OFFSET + 4 * (id / 32)) >> (id % 32)) &
         1;
}

uint32_t eg2_ms_plic_claim(void) {
  return eg2_dev_read32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET);
}

void eg2_ms_plic_complete(uint32_t id) {
  eg2_dev_write32(kEg2DevRvPlic, RV_PLIC_CC0_REG_OFFSET, id);
}

// ------------------------------------------ RRAM ---------------------------------------------

static uint32_t rram_rd(uint32_t off) { return eg2_dev_read32(kEg2DevRramCtrlCore, off); }
static void rram_wr(uint32_t off, uint32_t v) { eg2_dev_write32(kEg2DevRramCtrlCore, off, v); }

bool eg2_ms_rram_setup(void) {
  if (!(rram_rd(RRAM_CTRL_STATUS_REG_OFFSET) & (1u << RRAM_CTRL_STATUS_INIT_DONE_BIT))) {
    rram_wr(RRAM_CTRL_INIT_REG_OFFSET, 1);
    uint32_t i = 0;
    while (!(rram_rd(RRAM_CTRL_STATUS_REG_OFFSET) & (1u << RRAM_CTRL_STATUS_INIT_DONE_BIT))) {
      if (++i > 100000) {
        return false;
      }
    }
  }
  // Read and write allowed where no region says otherwise; scrambling and ECC as the default
  // region had them (the host reads them back the same way).
  uint32_t d = rram_rd(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET);
  d &= ~((RRAM_CTRL_DEFAULT_REGION_RD_EN_MASK << RRAM_CTRL_DEFAULT_REGION_RD_EN_OFFSET) |
         (RRAM_CTRL_DEFAULT_REGION_WR_EN_MASK << RRAM_CTRL_DEFAULT_REGION_WR_EN_OFFSET));
  d |= (kMuBi4True << RRAM_CTRL_DEFAULT_REGION_RD_EN_OFFSET) |
       (kMuBi4True << RRAM_CTRL_DEFAULT_REGION_WR_EN_OFFSET);
  rram_wr(RRAM_CTRL_DEFAULT_REGION_REG_OFFSET, d);
  return true;
}

bool eg2_ms_rram_program(uint32_t addr, const uint32_t *words, uint32_t n, uint32_t *err_code) {
  *err_code = 0;
  // Clear what an earlier operation left.
  rram_wr(RRAM_CTRL_OP_STATUS_REG_OFFSET, 0);
  rram_wr(RRAM_CTRL_ADDR_REG_OFFSET, (addr - kEg2MsNvmBase) & RRAM_CTRL_ADDR_START_MASK);
  rram_wr(RRAM_CTRL_CONTROL_REG_OFFSET,
          (1u << RRAM_CTRL_CONTROL_START_BIT) |
              (RRAM_CTRL_CONTROL_OP_VALUE_WRITE << RRAM_CTRL_CONTROL_OP_OFFSET) |
              (((n - 1) & RRAM_CTRL_CONTROL_NUM_MASK) << RRAM_CTRL_CONTROL_NUM_OFFSET));
  for (uint32_t i = 0; i < n; ++i) {
    // The write FIFO stalls the controller when empty and the bus when full.
    eg2_dev_write32(kEg2DevRramCtrlCore, RRAM_CTRL_WR_FIFO_REG_OFFSET, words[i]);
  }
  uint32_t s, polls = 0;
  do {
    s = rram_rd(RRAM_CTRL_OP_STATUS_REG_OFFSET);
  } while (!(s & (1u << RRAM_CTRL_OP_STATUS_DONE_BIT)) && ++polls < 1000000);
  if (s & (1u << RRAM_CTRL_OP_STATUS_ERR_BIT)) {
    *err_code = rram_rd(RRAM_CTRL_ERR_CODE_REG_OFFSET);
    // ERR_CODE is rw1c.
    rram_wr(RRAM_CTRL_ERR_CODE_REG_OFFSET, *err_code);
  }
  rram_wr(RRAM_CTRL_OP_STATUS_REG_OFFSET, 0);
  return (s & (1u << RRAM_CTRL_OP_STATUS_DONE_BIT)) && !(s & (1u << RRAM_CTRL_OP_STATUS_ERR_BIT));
}

// ------------------------------------------ SRAM ---------------------------------------------

bool eg2_ms_sram_ctrl(eg2_dev_id_t regs, bool renew_key, bool init) {
  uint32_t ctrl = (renew_key ? 1u << SRAM_CTRL_CTRL_RENEW_SCR_KEY_BIT : 0) |
                  (init ? 1u << SRAM_CTRL_CTRL_INIT_BIT : 0);
  eg2_dev_write32(regs, SRAM_CTRL_CTRL_REG_OFFSET, ctrl);
  uint32_t want = init ? (1u << SRAM_CTRL_STATUS_INIT_DONE_BIT)
                       : (1u << SRAM_CTRL_STATUS_SCR_KEY_VALID_BIT);
  for (uint32_t i = 0; i < 1000000; ++i) {
    uint32_t s = eg2_dev_read32(regs, SRAM_CTRL_STATUS_REG_OFFSET);
    if (s & (1u << SRAM_CTRL_STATUS_INIT_ERROR_BIT)) {
      return false;
    }
    if (s & want) {
      return true;
    }
  }
  return false;
}

// ----------------------------------- Boot record and resets -----------------------------------

uint32_t eg2_ms_reset_reason(void) {
  // retention_sram_t: version (4 bytes), then creator.reset_reasons (retention_sram.h).
  return eg2_ms_read32(kEg2MsRetBase + 4);
}

static eg2_ms_rec_t *rec_ptr(void) {
  return (eg2_ms_rec_t *)eg2_mem(kEg2MsRecAddr, sizeof(eg2_ms_rec_t));
}

eg2_ms_rec_t *eg2_ms_rec(void) { return rec_ptr(); }

eg2_ms_rec_t *eg2_ms_rec_boot(void) {
  volatile eg2_ms_rec_t *r = rec_ptr();
  uint32_t reason = eg2_ms_reset_reason();
  // On a power-on reset the record's words are uninitialised (a read would meet bad integrity):
  // write every word before reading any.
  if ((reason & kEg2MsResetPor) || r->magic != kEg2MsRecMagic) {
    r->magic = kEg2MsRecMagic;
    r->boot = 0;
    r->phase = 0;
    for (uint32_t i = 0; i < ARRAYSIZE(r->data); ++i) {
      r->data[i] = 0;
    }
  }
  r->boot = r->boot + 1;
  return (eg2_ms_rec_t *)r;
}

const char *eg2_ms_reset_name(eg2_ms_reset_kind_t kind) {
  switch (kind) {
    case kEg2MsResetKindPor:
      return "POR";
    case kEg2MsResetKindSw:
      return "software";
    case kEg2MsResetKindNdm:
      return "NDM";
    case kEg2MsResetKindEscalation:
      return "escalation";
    case kEg2MsResetKindDeepSleep:
      return "deep-sleep wake-up";
    default:
      return "watchdog bite";
  }
}

uint32_t eg2_ms_reset_expected(eg2_ms_reset_kind_t kind) {
  switch (kind) {
    case kEg2MsResetKindPor:
      return kEg2MsResetPor;
    case kEg2MsResetKindSw:
      return kEg2MsResetSw;
    case kEg2MsResetKindNdm:
      return kEg2MsResetNdm;
    case kEg2MsResetKindEscalation:
      return kEg2MsResetEscalation;
    case kEg2MsResetKindDeepSleep:
      return kEg2MsResetLowPowerExit;
    default:
      return kEg2MsResetWatchdog;
  }
}

static void ah_write(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

void eg2_ms_escalation_arm(size_t alert) {
  ah_write(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * (uint32_t)alert,
           ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSA);
  ah_write(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * (uint32_t)alert, 1);
  ah_write(ALERT_HANDLER_CLASSA_ACCUM_THRESH_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_TIMEOUT_CYC_SHADOWED_REG_OFFSET, 0);
  ah_write(ALERT_HANDLER_CLASSA_PHASE0_CYC_SHADOWED_REG_OFFSET, 200);
  ah_write(ALERT_HANDLER_CLASSA_PHASE1_CYC_SHADOWED_REG_OFFSET, 100);
  ah_write(ALERT_HANDLER_CLASSA_PHASE2_CYC_SHADOWED_REG_OFFSET, 100);
  ah_write(ALERT_HANDLER_CLASSA_PHASE3_CYC_SHADOWED_REG_OFFSET, 100);
  ah_write(ALERT_HANDLER_CLASSA_CRASHDUMP_TRIGGER_SHADOWED_REG_OFFSET, 0);
  // Escalation signal 3 (the escalation reset, pwrmgr) in phase 0: nothing else.
  ah_write(ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET,
           (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_BIT) |
               (1u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_EN_E3_BIT) |
               (0u << ALERT_HANDLER_CLASSA_CTRL_SHADOWED_MAP_E3_OFFSET));
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  1u << RSTMGR_ALERT_INFO_CTRL_EN_BIT);
}

enum {
  // The crash dump (alert_crashdump_t), from the least significant member: the class states, the
  // escalation counts, the accumulators, the local causes, the causes.
  kDumpStateBits = 3,
  kDumpAccumPos = 4 * kDumpStateBits + 4 * ALERT_HANDLER_PARAM_ESC_CNT_DW,
  kDumpLocCausePos = kDumpAccumPos + 4 * ALERT_HANDLER_PARAM_ACCU_CNT_DW,
  kDumpCausePos = kDumpLocCausePos + ALERT_HANDLER_PARAM_N_LOC_ALERT,
};

bool eg2_ms_crashdump_cause(size_t alert) {
  uint32_t pos = kDumpCausePos + (uint32_t)alert;
  eg2_dev_write32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_CTRL_REG_OFFSET,
                  (pos / 32) << RSTMGR_ALERT_INFO_CTRL_INDEX_OFFSET);
  uint32_t w = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_ALERT_INFO_REG_OFFSET);
  return (w >> (pos % 32)) & 1;
}

static void pwrmgr_sync(void) {
  eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET, 1);
  for (uint32_t i = 0; i < 100000; ++i) {
    if (!(eg2_dev_read32(kEg2DevPwrmgr, PWRMGR_CFG_CDC_SYNC_REG_OFFSET) & 1)) {
      return;
    }
  }
}

void eg2_ms_reset(eg2_ms_reset_kind_t kind, bool bench) {
  eg2_puts("EG2 reset: ");
  eg2_puts(eg2_ms_reset_name(kind));
  eg2_puts("\r\n");
  switch (kind) {
    case kEg2MsResetKindPor:
    case kEg2MsResetKindNdm:
      if (bench) {
        // The bench applies the reset and does not answer: the boot after it continues.
        (void)eg2_ms_bench(kind == kEg2MsResetKindPor ? "por" : "ndm_reset", 0, 0, true);
        break;
      }
      eg2_puts("EG2 reset: no bench, a software reset instead\r\n");
      OT_FALLTHROUGH_INTENDED;
    case kEg2MsResetKindSw:
      eg2_dev_write32(kEg2DevRstmgr, RSTMGR_RESET_REQ_REG_OFFSET, kMuBi4True);
      break;
    case kEg2MsResetKindEscalation: {
      int a = eg2_status_alert_find("rv_core_ibex_fatal_sw_err");
      if (a < 0) {
        return;
      }
      eg2_ms_escalation_arm((size_t)a);
      eg2_status_raise_alert((size_t)a);
      break;
    }
    case kEg2MsResetKindDeepSleep:
      // The AON timer wakes the chip from deep sleep; the wake-up is a reset (low-power exit).
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_COUNT_LO_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_COUNT_HI_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_THOLD_LO_REG_OFFSET, 20);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_THOLD_HI_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_WAKEUP_EN_REG_OFFSET,
                      1u << PWRMGR_WAKEUP_EN_EN_4_BIT);
      eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_CONTROL_REG_OFFSET,
                      1u << PWRMGR_CONTROL_LOW_POWER_HINT_BIT);
      pwrmgr_sync();
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WKUP_CTRL_REG_OFFSET,
                      1u << AON_TIMER_WKUP_CTRL_ENABLE_BIT);
      __asm__ volatile("wfi");
      break;
    default:
      // The watchdog: a bite resets the chip once pwrmgr lets it (RESET_EN, aon_timer's request).
      eg2_dev_write32(kEg2DevPwrmgr, PWRMGR_RESET_EN_REG_OFFSET, 1u << PWRMGR_RESET_EN_EN_1_BIT);
      pwrmgr_sync();
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_COUNT_REG_OFFSET, 0);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BARK_THOLD_REG_OFFSET, 0xffffffffu);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_BITE_THOLD_REG_OFFSET, 20);
      eg2_dev_write32(kEg2DevAonTimer, AON_TIMER_WDOG_CTRL_REG_OFFSET,
                      1u << AON_TIMER_WDOG_CTRL_ENABLE_BIT);
      break;
  }
  // The reset comes some cycles after the request.
  eg2_ms_spin(200000);
  eg2_puts("EG2 reset: no reset came\r\n");
}
