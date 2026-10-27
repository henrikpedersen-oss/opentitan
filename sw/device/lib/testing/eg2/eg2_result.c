// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/lib/testing/eg2/eg2_result.h"

#include "hw/top/gpio_regs.h"         // Generated.
#include "hw/top/rv_core_ibex_regs.h"  // Generated.
#include "hw/top/uart_regs.h"          // Generated.
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/macros.h"
#include "sw/device/lib/testing/eg2/eg2_dev.h"
#include "sw/device/lib/testing/eg2/eg2_status.h"

// The DV status window's words, as OTTF's `test_status_set` writes them.
enum {
  kEg2DvInTest = 0x4354,
  kEg2DvPassed = 0x900d,
  kEg2DvFailed = 0xbaad,
};

// The console's channel (eg2_result.h). Written by chip_sw_eg2_base_vseq before the CPU starts;
// the image's 0 keeps the console on UART0. A constant in .rodata, as kEg2Seed (eg2_params.c):
// the start-up code would overwrite a writable global.
OT_USED OT_SECTION(".rodata") volatile const uint32_t kEg2ConsoleDv = 0;

enum {
  // The DV window's console word: SW_DV_EG2_CONSOLE_ADDR in the chip bench's chip_common_pkg.sv
  // (after OTTF's status word at +0 and log word at +4).
  kEg2DvConsoleOffset = RV_CORE_IBEX_DV_SIM_WINDOW_REG_OFFSET + 8,
  // The first failing line kept for the verdict, cut to this length. Small on purpose: several
  // CHERIoT images are within a few bytes of their 4 KiB of globals (REQ_STE_12).
  kFailureSize = 40,
};

static uint8_t console_verbosity = kEg2VerbosityLow;

// The first console line that reported a failure, repeated in the UART verdict. Each line is built
// here until one matches; that one is kept.
static char failure[kFailureSize];
static uint8_t failure_len;
static bool failure_kept;

void eg2_console_init(void) {
  eg2_dev_write32(kEg2DevUart0, UART_CTRL_REG_OFFSET,
                  (1u << UART_CTRL_TX_BIT) |
                      ((kUartNCOValue & UART_CTRL_NCO_MASK) << UART_CTRL_NCO_OFFSET));
}

void eg2_console_verbosity(eg2_verbosity_t verbosity) {
  console_verbosity = (uint8_t)verbosity;
}

static bool failure_has(const char *word) {
  for (uint32_t i = 0; i < failure_len; ++i) {
    uint32_t j = 0;
    while (word[j] != '\0' && i + j < failure_len && failure[i + j] == word[j]) {
      ++j;
    }
    if (word[j] == '\0') {
      return true;
    }
  }
  return false;
}

static void note_failure(char c) {
  if (failure_kept) {
    return;
  }
  if (c == '\n' || c == '\r') {
    if (failure_len != 0 && (failure_has("FAIL") || failure_has("MISMATCH") ||
                             failure_has("STATUS ") || failure_has("TRAP"))) {
      failure_kept = true;
      return;
    }
    failure_len = 0;
    return;
  }
  if (failure_len < kFailureSize) {
    failure[failure_len++] = c;
  }
}

static void uart_putc(char c) {
  while (eg2_dev_read32(kEg2DevUart0, UART_STATUS_REG_OFFSET) &
         (1u << UART_STATUS_TXFULL_BIT)) {
  }
  eg2_dev_write32(kEg2DevUart0, UART_WDATA_REG_OFFSET, (uint8_t)c);
}

static void uart_puts(const char *s) {
  for (; *s != '\0'; ++s) {
    uart_putc(*s);
  }
}

static void putc_(char c) {
  note_failure(c);
  if (kEg2ConsoleDv != 0) {
    eg2_dev_write32(kEg2DevRvCoreIbexCfg, kEg2DvConsoleOffset,
                    ((uint32_t)console_verbosity << 8) | (uint8_t)c);
    return;
  }
  uart_putc(c);
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

static void uart0_drain(void) {
  while (!(eg2_dev_read32(kEg2DevUart0, UART_STATUS_REG_OFFSET) &
           (1u << UART_STATUS_TXIDLE_BIT))) {
  }
  // TXIDLE comes before the receiver has the last character: three more character times (10 bits
  // each) before the status word ends the run (2026-10-27: on Verilator the final "\r\n" of
  // "PASS!\r\n" was lost, so the console never matched; about 2000 cycles there, a few on DV).
  uint32_t cycles = 3u * 10u * ((uint32_t)kClockFreqCpuHz / (uint32_t)kUartBaudrate);
  for (uint32_t i = 0; i < cycles / 2u; ++i) {
    __asm__ volatile("nop");
  }
}

// The verdict on UART0, whichever channel the console uses: `PASS!`, or `FAIL! ` and the first
// console line that reported a failure, so the UART alone says why.
static void uart_verdict(bool passed) {
  if (passed) {
    uart_puts("PASS!\r\n");
  } else {
    uart_puts("FAIL! ");
    if (failure_kept) {
      for (uint32_t i = 0; i < failure_len; ++i) {
        uart_putc(failure[i]);
      }
    } else {
      uart_puts("(no failing console line: see the log)");
    }
    uart_puts("\r\n");
  }
  // The UART line must be out before the run ends on the status word.
  uart0_drain();
}

bool eg2_test_end(bool passed) {
  passed = eg2_status_check() && passed;
  gpio_verdict(passed);
#ifndef __CHERIOT__
  // In RISC-V mode OTTF reports the verdict on its own channel (the DV log on sim_dv), without
  // waiting for UART0: the UART's verdict goes out first.
  uart_verdict(passed);
#endif
  return passed;
}

void eg2_result_report(bool passed) {
  uart_verdict(passed);
  dv_status(passed ? kEg2DvPassed : kEg2DvFailed);
  for (;;) {
    __asm__ volatile("wfi");
  }
}
