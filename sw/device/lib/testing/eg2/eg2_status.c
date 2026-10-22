// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_status.h"

#include "hw/top/alert_handler_regs.h"  // Generated.
#include "hw/top/rstmgr_regs.h"         // Generated.
#include "sw/device/lib/testing/eg2/eg2_result.h"

// Per-alert and per-class register arrays of the alert handler.
#define AH_ALERT_STRIDE \
  (ALERT_HANDLER_ALERT_EN_SHADOWED_1_REG_OFFSET - ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET)
#define AH_CLASS_STRIDE                                                                  \
  ((ALERT_HANDLER_CLASSD_CTRL_SHADOWED_REG_OFFSET -                                      \
    ALERT_HANDLER_CLASSA_CTRL_SHADOWED_REG_OFFSET) /                                     \
   3)
_Static_assert(AH_ALERT_STRIDE == 4, "one 32-bit register per alert");
_Static_assert(ALERT_HANDLER_CLASSD_STATE_REG_OFFSET ==
                   ALERT_HANDLER_CLASSA_STATE_REG_OFFSET + 3 * AH_CLASS_STRIDE,
               "class registers are evenly spaced");
_Static_assert(ALERT_HANDLER_PARAM_N_ALERTS == EG2_ALERT_COUNT,
               "eg2_tables.h is generated from this top");

enum {
  kAlertWords = (EG2_ALERT_COUNT + 31) / 32,
  kClassD = 3,
  kMisaE = 1u << 4,
  kMisaI = 1u << 8,
};

typedef struct eg2_alert {
  const char *name;
  eg2_dev_id_t dev;
  uint32_t test_offset;
  uint32_t bit;
} eg2_alert_t;

static const eg2_alert_t kAlerts[] = {
#define EG2_ALERT_ROW_(name, dev, off, bit) {name, dev, off, bit},
    EG2_ALERT_TABLE(EG2_ALERT_ROW_)
#undef EG2_ALERT_ROW_
};

typedef struct eg2_intr {
  eg2_dev_id_t dev;
  uint32_t state_offset;
  uint32_t test_offset;
  uint32_t mask;
} eg2_intr_t;

static const eg2_intr_t kIntrs[] = {
#define EG2_INTR_ROW_(dev, state, test, mask) {dev, state, test, mask},
    EG2_INTR_TABLE(EG2_INTR_ROW_)
#undef EG2_INTR_ROW_
};

typedef struct eg2_err {
  eg2_dev_id_t dev;
  uint32_t offset;
  uint32_t mask;
  const char *reg;
} eg2_err_t;

static const eg2_err_t kErrs[] = {
#define EG2_ERR_ROW_(dev, off, mask, reg) {dev, off, mask, reg},
    EG2_ERR_TABLE(EG2_ERR_ROW_)
#undef EG2_ERR_ROW_
};

static uint32_t expected_alerts[kAlertWords];
static uint32_t expected_intrs[kEg2DevCount];

static uint32_t ah_read(uint32_t offset) {
  return eg2_dev_read32(kEg2DevAlertHandler, offset);
}

static void ah_write_shadowed(uint32_t offset, uint32_t value) {
  eg2_dev_write32_shadowed(kEg2DevAlertHandler, offset, value);
}

static bool alert_expected(size_t alert) {
  return (expected_alerts[alert / 32] >> (alert % 32)) & 1;
}

static bool any_alert_expected(void) {
  for (size_t i = 0; i < kAlertWords; ++i) {
    if (expected_alerts[i] != 0) {
      return true;
    }
  }
  return false;
}

static const eg2_intr_t *intr_row(eg2_dev_id_t dev) {
  for (size_t i = 0; i < sizeof(kIntrs) / sizeof(kIntrs[0]); ++i) {
    if (kIntrs[i].dev == dev) {
      return &kIntrs[i];
    }
  }
  return NULL;
}

// One line per finding: `STATUS <what> <where> = <value>`.
static void report(const char *what, const char *where, const char *reg, uint32_t value) {
  eg2_puts("STATUS ");
  eg2_puts(what);
  eg2_puts(" ");
  eg2_puts(where);
  if (reg != NULL) {
    eg2_puts(" ");
    eg2_puts(reg);
  }
  eg2_puts(" = 0x");
  eg2_puthex(value);
  eg2_puts("\r\n");
}

void eg2_status_init(void) {
  for (uint32_t i = 0; i < ALERT_HANDLER_PARAM_N_ALERTS; ++i) {
    ah_write_shadowed(ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * i,
                      ALERT_HANDLER_ALERT_CLASS_SHADOWED_0_CLASS_A_0_VALUE_CLASSD);
    ah_write_shadowed(ALERT_HANDLER_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * i, 1);
  }
  for (uint32_t i = 0; i < ALERT_HANDLER_PARAM_N_LOC_ALERT; ++i) {
    ah_write_shadowed(ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_REG_OFFSET + 4 * i,
                      ALERT_HANDLER_LOC_ALERT_CLASS_SHADOWED_0_CLASS_LA_0_VALUE_CLASSD);
    ah_write_shadowed(ALERT_HANDLER_LOC_ALERT_EN_SHADOWED_0_REG_OFFSET + 4 * i, 1);
  }
  // Class D's CTRL is left at reset: EN clear, so it never escalates.
}

static bool check_alert_handler(void) {
  bool ok = true;
  for (size_t i = 0; i < kEg2AlertCount; ++i) {
    bool cause = ah_read(ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4 * (uint32_t)i) & 1;
    if (cause != alert_expected(i)) {
      report(cause ? "unexpected alert" : "missing alert", kAlerts[i].name, NULL, cause);
      ok = false;
    }
  }
  for (uint32_t i = 0; i < ALERT_HANDLER_PARAM_N_LOC_ALERT; ++i) {
    uint32_t cause = ah_read(ALERT_HANDLER_LOC_ALERT_CAUSE_0_REG_OFFSET + 4 * i) & 1;
    if (cause) {
      static const char *const kLocal[] = {
          "alert_ping_fail", "esc_ping_fail",           "alert_integfail",
          "esc_integfail",   "bus_integfail",           "shadow_reg_update_error",
          "shadow_reg_storage_error",
      };
      report("unexpected local alert", i < 7 ? kLocal[i] : "?", NULL, cause);
      ok = false;
    }
  }
  bool d_allowed = any_alert_expected();
  for (uint32_t c = 0; c < ALERT_HANDLER_PARAM_N_CLASSES; ++c) {
    static const char *const kClass[] = {"class A", "class B", "class C", "class D"};
    uint32_t accum = ah_read(ALERT_HANDLER_CLASSA_ACCUM_CNT_REG_OFFSET + c * AH_CLASS_STRIDE);
    uint32_t state = ah_read(ALERT_HANDLER_CLASSA_STATE_REG_OFFSET + c * AH_CLASS_STRIDE);
    if (accum != 0 && !(c == kClassD && d_allowed)) {
      report("alerts accumulated in", kClass[c], "ACCUM_CNT", accum);
      ok = false;
    }
    if (state != ALERT_HANDLER_CLASSA_STATE_CLASSA_STATE_VALUE_IDLE) {
      report("escalation in", kClass[c], "STATE", state);
      ok = false;
    }
  }
  uint32_t intr = ah_read(ALERT_HANDLER_INTR_STATE_REG_OFFSET);
  uint32_t allowed = d_allowed ? 1u << ALERT_HANDLER_INTR_STATE_CLASSD_BIT : 0;
  if (intr & ~allowed) {
    report("unexpected interrupt", "alert_handler", "INTR_STATE", intr);
    ok = false;
  }
  return ok;
}

static bool check_interrupts(void) {
  bool ok = true;
  for (size_t i = 0; i < sizeof(kIntrs) / sizeof(kIntrs[0]); ++i) {
    const eg2_intr_t *r = &kIntrs[i];
    uint32_t state = eg2_dev_read32(r->dev, r->state_offset);
    uint32_t expected = expected_intrs[r->dev];
    if (state & r->mask & ~expected) {
      report("unexpected interrupt", eg2_dev_name(r->dev), "INTR_STATE", state);
      ok = false;
    }
    if ((state & expected) != expected) {
      report("missing interrupt", eg2_dev_name(r->dev), "INTR_STATE", state);
      ok = false;
    }
  }
  return ok;
}

static bool check_error_registers(void) {
  bool ok = true;
  for (size_t i = 0; i < sizeof(kErrs) / sizeof(kErrs[0]); ++i) {
    const eg2_err_t *r = &kErrs[i];
    uint32_t value = eg2_dev_read32(r->dev, r->offset);
    if (value & r->mask) {
      report("error flag", eg2_dev_name(r->dev), r->reg, value);
      ok = false;
    }
  }
  return ok;
}

static bool check_core(void) {
  bool ok = true;
  uint32_t misa;
  __asm__ volatile("csrr %0, misa" : "=r"(misa));
#ifdef __CHERIOT__
  const uint32_t want = kMisaE;
#else
  const uint32_t want = kMisaI;
#endif
  if ((misa & (kMisaE | kMisaI)) != want) {
    report("wrong execution mode", "rv_core_ibex", "misa", misa);
    ok = false;
  }
  uint32_t info = eg2_dev_read32(kEg2DevRstmgr, RSTMGR_RESET_INFO_REG_OFFSET);
  if (info & ~(1u << RSTMGR_RESET_INFO_POR_BIT)) {
    report("unexpected reset", "rstmgr", "RESET_INFO", info);
    ok = false;
  }
  return ok;
}

bool eg2_status_check(void) {
  // Every part runs, so one sweep reports everything.
  bool ok = check_alert_handler();
  ok = check_interrupts() && ok;
  ok = check_error_registers() && ok;
  ok = check_core() && ok;
  return ok;
}

int eg2_status_alert_find(const char *name) {
  for (size_t i = 0; i < kEg2AlertCount; ++i) {
    const char *a = kAlerts[i].name;
    const char *b = name;
    while (*a != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*a == *b) {
      return (int)i;
    }
  }
  return -1;
}

const char *eg2_status_alert_name(size_t alert) { return kAlerts[alert].name; }

void eg2_status_expect_alert(size_t alert) {
  expected_alerts[alert / 32] |= 1u << (alert % 32);
}

void eg2_status_expect_intr(eg2_dev_id_t dev, uint32_t mask) { expected_intrs[dev] |= mask; }

void eg2_status_raise_alert(size_t alert) {
  const eg2_alert_t *a = &kAlerts[alert];
  eg2_dev_write32(a->dev, a->test_offset, 1u << a->bit);
}

void eg2_status_raise_intr(eg2_dev_id_t dev, uint32_t mask) {
  const eg2_intr_t *r = intr_row(dev);
  if (r != NULL) {
    eg2_dev_write32(dev, r->test_offset, mask);
  }
}

bool eg2_status_wait_alert(size_t alert, uint32_t max_polls) {
  for (uint32_t polls = 0; polls < max_polls; ++polls) {
    if (ah_read(ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4 * (uint32_t)alert) & 1) {
      return true;
    }
  }
  return false;
}

void eg2_status_clear_expected(void) {
  for (size_t i = 0; i < kEg2AlertCount; ++i) {
    if (alert_expected(i)) {
      eg2_dev_write32(kEg2DevAlertHandler,
                      ALERT_HANDLER_ALERT_CAUSE_0_REG_OFFSET + 4 * (uint32_t)i, 1);
    }
  }
  if (any_alert_expected()) {
    ah_write_shadowed(ALERT_HANDLER_CLASSD_CLR_SHADOWED_REG_OFFSET, 1);
    eg2_dev_write32(kEg2DevAlertHandler, ALERT_HANDLER_INTR_STATE_REG_OFFSET,
                    1u << ALERT_HANDLER_INTR_STATE_CLASSD_BIT);
  }
  for (size_t i = 0; i < sizeof(kIntrs) / sizeof(kIntrs[0]); ++i) {
    const eg2_intr_t *r = &kIntrs[i];
    if (expected_intrs[r->dev] != 0) {
      eg2_dev_write32(r->dev, r->state_offset, expected_intrs[r->dev]);
    }
  }
  for (size_t i = 0; i < kAlertWords; ++i) {
    expected_alerts[i] = 0;
  }
  for (size_t i = 0; i < kEg2DevCount; ++i) {
    expected_intrs[i] = 0;
  }
}
