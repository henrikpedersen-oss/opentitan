// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * OpenTitan's DIFs and `abs_mmio`, unchanged, in either execution mode (EG2 framework step 3).
 *
 * Every hardware access goes through the devicetable, a DIF or `abs_mmio`, never through the
 * framework's own device helpers: in CHERIoT mode that is the path where `mmio_region_from_addr`
 * and `abs_mmio` derive a bounded capability (sw/device/lib/base/cheriot_mmio.h).
 *
 *  1. UART1 through its DIF, in system loopback: `num_bytes` bytes (a parameter) drawn from the
 *     software seed are sent and must come back in order.
 *  2. The RISC-V timer through its DIF: configured, enabled, and its counter must advance.
 *  3. `abs_mmio`: a timer compare register written and read back.
 *  4. CHERIoT mode only: the UART handle's base is a tagged capability whose bounds are exactly
 *     UART1's register window, and an address in no bus window gives an untagged pointer.
 *
 * Built with `EG2_DIF_INJECT_FAIL`, one looped-back byte is expected one higher than sent: the
 * run must fail with `step FAILED: UART1 loopback` (the check is live).
 */

#include "hw/top/dt/api.h"       // Generated.
#include "hw/top/dt/rv_timer.h"  // Generated.
#include "hw/top/dt/uart.h"      // Generated.
#include "hw/top/rv_timer_regs.h"  // Generated.
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/abs_mmio.h"
#include "sw/device/lib/dif/dif_rv_timer.h"
#include "sw/device/lib/dif/dif_uart.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"

EG2_TEST_CONFIG();

#define DIF_SMOKETEST_PARAMS(X) X(num_bytes, 8, 1, 32)
EG2_TEST_PARAMS(DIF_SMOKETEST_PARAMS);

enum {
  // Polls of the UART receive FIFO per byte; at the loopback a byte takes ten bit times.
  kRxPolls = 100000,
};

static bool step(const char *name, bool ok) {
  eg2_puts(ok ? "step ok: " : "step FAILED: ");
  eg2_puts(name);
  eg2_puts("\r\n");
  return ok;
}

static bool uart_loopback(void) {
  dif_uart_t uart;
  if (dif_uart_init_from_dt(kDtUart1, &uart) != kDifOk) {
    return false;
  }
  dif_uart_config_t config = {
      .baudrate = (uint32_t)kUartBaudrate,
      .clk_freq_hz = (uint32_t)kClockFreqPeripheralHz,
      .parity_enable = kDifToggleDisabled,
      .parity = kDifUartParityEven,
      .tx_enable = kDifToggleEnabled,
      .rx_enable = kDifToggleDisabled,
  };
  // The receiver is enabled only once loopback is on: before that it samples UART1's RX pad,
  // which nothing drives in this test, and an undriven pad fills the receive FIFO with X.
  if (dif_uart_configure(&uart, config) != kDifOk ||
      dif_uart_loopback_set(&uart, kDifUartLoopbackSystem, kDifToggleEnabled) != kDifOk ||
      dif_uart_set_enable(&uart, kDifUartDatapathRx, kDifToggleEnabled) != kDifOk) {
    return false;
  }

#ifdef __CHERIOT__
  // The DIF's base is the capability cheriot_mmio.h derived: tagged, exactly UART1's window.
  volatile void *base = uart.base_addr.base;
  if (!__builtin_cheri_tag_get(base) ||
      __builtin_cheri_base_get(base) != dt_uart_primary_reg_block(kDtUart1) ||
      __builtin_cheri_length_get(base) != TOP_EARLGREY_UART1_SIZE_BYTES) {
    eg2_puts("UART1 handle is not a capability bounded to its window\r\n");
    return false;
  }
#endif

  bool ok = true;
  for (uint32_t i = 0; i < EG2_PARAM(num_bytes); ++i) {
    uint8_t sent = (uint8_t)eg2_rand();
    uint8_t expected = sent;
#ifdef EG2_DIF_INJECT_FAIL
    if (i == 0) {
      expected = (uint8_t)(sent + 1);
    }
#endif
    if (dif_uart_byte_send_polled(&uart, sent) != kDifOk) {
      return false;
    }
    size_t avail = 0;
    for (uint32_t poll = 0; poll < kRxPolls && avail == 0; ++poll) {
      if (dif_uart_rx_bytes_available(&uart, &avail) != kDifOk) {
        return false;
      }
    }
    uint8_t got = 0;
    if (avail == 0 || dif_uart_bytes_receive(&uart, 1, &got, NULL) != kDifOk) {
      eg2_puts("UART1 loopback: nothing received\r\n");
      return false;
    }
    if (got != expected) {
      eg2_puts("UART1 loopback: sent 0x");
      eg2_puthex(sent);
      eg2_puts(", received 0x");
      eg2_puthex(got);
      eg2_puts("\r\n");
      ok = false;
    }
  }
  return dif_uart_loopback_set(&uart, kDifUartLoopbackSystem, kDifToggleDisabled) == kDifOk &&
         ok;
}

static bool timer_advances(void) {
  dif_rv_timer_t timer;
  dif_rv_timer_tick_params_t tick;
  uint64_t before = 0, after = 0;
  if (dif_rv_timer_init_from_dt((dt_rv_timer_t)0, &timer) != kDifOk ||
      dif_rv_timer_reset(&timer) != kDifOk ||
      dif_rv_timer_approximate_tick_params(kClockFreqPeripheralHz, 1000000, &tick) != kDifOk ||
      dif_rv_timer_set_tick_params(&timer, 0, tick) != kDifOk ||
      dif_rv_timer_counter_set_enabled(&timer, 0, kDifToggleEnabled) != kDifOk ||
      dif_rv_timer_counter_read(&timer, 0, &before) != kDifOk) {
    return false;
  }
  for (volatile uint32_t spin = 0; spin < 2000; ++spin) {
  }
  if (dif_rv_timer_counter_read(&timer, 0, &after) != kDifOk) {
    return false;
  }
  return after > before &&
         dif_rv_timer_counter_set_enabled(&timer, 0, kDifToggleDisabled) == kDifOk;
}

static bool abs_mmio_readback(void) {
  const uint32_t reg = dt_rv_timer_primary_reg_block((dt_rv_timer_t)0) +
                       RV_TIMER_COMPARE_LOWER0_0_REG_OFFSET;
  const uint32_t value = eg2_rand();
  uint32_t old = abs_mmio_read32(reg);
  abs_mmio_write32(reg, value);
  bool ok = abs_mmio_read32(reg) == value;
  abs_mmio_write32(reg, old);
  return ok;
}

#ifdef __CHERIOT__
static bool outside_windows_untagged(void) {
  // No bus window contains this address: the pointer must carry no tag.
  mmio_region_t region = mmio_region_from_addr(0x50000000u);
  return !__builtin_cheri_tag_get(region.base);
}
#endif

bool eg2_test_main(void) {
  bool ok = step("UART1 loopback", uart_loopback());
  ok = step("timer advances", timer_advances()) && ok;
  ok = step("abs_mmio read-back", abs_mmio_readback()) && ok;
#ifdef __CHERIOT__
  ok = step("address outside every window is untagged", outside_windows_untagged()) && ok;
#endif
  return ok;
}
