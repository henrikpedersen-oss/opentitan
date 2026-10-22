// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_result.h"

#include "hw/top/gpio_regs.h"         // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/uart_regs.h"          // Generated.
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"

// The DV status window's words, as OTTF's `test_status_set` writes them.
enum {
  kEg2DvInTest = 0x4354,
  kEg2DvPassed = 0x900d,
  kEg2DvFailed = 0xbaad,
};

void eg2_console_init(void) {
  eg2_dev_write32(kEg2DevUart0, UART_CTRL_REG_OFFSET,
                  (1u << UART_CTRL_TX_BIT) |
                      ((kUartNCOValue & UART_CTRL_NCO_MASK) << UART_CTRL_NCO_OFFSET));
}

static void putc_(char c) {
  while (eg2_dev_read32(kEg2DevUart0, UART_STATUS_REG_OFFSET) &
         (1u << UART_STATUS_TXFULL_BIT)) {
  }
  eg2_dev_write32(kEg2DevUart0, UART_WDATA_REG_OFFSET, (uint8_t)c);
}

void eg2_puts(const char *s) {
  for (; *s != '\0'; ++s) {
    putc_(*s);
  }
}

void eg2_puthex(uint32_t value) {
  for (int shift = 28; shift >= 0; shift -= 4) {
    uint32_t nibble = (value >> shift) & 0xf;
    putc_((char)(nibble < 10 ? '0' + nibble : 'a' + nibble - 10));
  }
}

void eg2_putdec(uint32_t value) {
  char digits[10];
  int n = 0;
  do {
    digits[n++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  while (n > 0) {
    putc_(digits[--n]);
  }
}

static void dv_status(uint32_t word) {
  eg2_dev_write32(kEg2DevRvCoreIbexCfg, RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET, word);
}

void eg2_result_in_test(void) { dv_status(kEg2DvInTest); }

#ifdef EG2_RESULT_GPIO_PIN
static void gpio_verdict(bool passed) {
  const uint32_t pin = 1u << EG2_RESULT_GPIO_PIN;
  uint32_t out = eg2_dev_read32(kEg2DevGpio, GPIO_DIRECT_OUT_REG_OFFSET);
  eg2_dev_write32(kEg2DevGpio, GPIO_DIRECT_OE_REG_OFFSET,
                  eg2_dev_read32(kEg2DevGpio, GPIO_DIRECT_OE_REG_OFFSET) | pin);
  if (passed) {
    eg2_dev_write32(kEg2DevGpio, GPIO_DIRECT_OUT_REG_OFFSET, out | pin);
    return;
  }
  // A failure toggles the pin, so a stuck-high pin is never taken for a pass.
  for (int i = 0; i < 8; ++i) {
    out ^= pin;
    eg2_dev_write32(kEg2DevGpio, GPIO_DIRECT_OUT_REG_OFFSET, out);
  }
}
#else
static void gpio_verdict(bool passed) { (void)passed; }
#endif

bool eg2_test_end(bool passed) {
  passed = eg2_status_check() && passed;
  gpio_verdict(passed);
  return passed;
}

void eg2_result_report(bool passed) {
  eg2_puts(passed ? "PASS!\r\n" : "FAIL!\r\n");
  // The UART line must be out before the bench ends the run on the status word.
  while (!(eg2_dev_read32(kEg2DevUart0, UART_STATUS_REG_OFFSET) &
           (1u << UART_STATUS_TXIDLE_BIT))) {
  }
  dv_status(passed ? kEg2DvPassed : kEg2DvFailed);
  for (;;) {
    __asm__ volatile("wfi");
  }
}
