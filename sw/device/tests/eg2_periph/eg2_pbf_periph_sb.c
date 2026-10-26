// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_periph_sb (EG2 verification spec REQ_PBF_13, "Post-boot features in CHERIoT
 * mode"): traffic on UART, SPI host, I2C and GPIO, behind secure boot or the test ROM, in RISC-V
 * mode and (the `_cheriot` build) in CHERIoT mode through exact-bounds device capabilities. The
 * pins are checked by the chip bench (its agents and the per-peripheral scoreboards); the software
 * checks what comes back to it.
 *
 * Phases, in order, each selected by a bit of `devices`:
 *   bit 0  uart1-uart3: N random bytes out on TX (the bench's UART agents see them); then the
 *          receive path: with `rx_bench` 0 by system loopback (CTRL.SLPBK: `rx_count` random
 *          bytes sent and read back); with `rx_bench` 1 the test sends the marker byte 0xA5 on TX
 *          and the bench answers with `rx_count` bytes (`rx_base` + i) & 0xff on RX, which the
 *          test reads and checks;
 *   bit 1  spi_host0 (and spi_host1 when bit 0 is clear: its pads overlap uart1's): a TX segment
 *          of N random bytes, then an RX segment of M bytes (N, M multiples of 4: the bench's SPI
 *          device agent plays back in 4-byte units); the RX words arrive, no error is raised;
 *          the received data is the bench's peer's and not checked here;
 *   bit 2  i2c0-i2c2 as hosts: N random bytes written to `i2c_addr`, then N bytes read back from
 *          it; the bench's I2C agent and eg2sim's I2C peer both return what was written;
 *   bit 3  gpio: every GPIO whose pad no enabled device uses (never 20-24, the JTAG and strap
 *          pads) driven on its pad, 4-8 random DIRECT_OUT / MASKED_OUT_* writes, each read back
 *          through the pad and DATA_IN.
 * First, every peripheral of the run gets one load and one store at an offset inside its register
 * window that holds no register (uart and spi_host 0x3c, gpio 0x3fc; i2c's 0x80-byte window has a
 * register at every word, so it has none): the register block answers with a TL-UL error
 * (reggen: an address no register decodes; the TL-UL specification's d_error), which the core takes
 * as a load or store access fault (mcause 5, 7), in CHERIoT mode too (the offset is inside the
 * device capability's bounds, so the capability check passes). The chip bench's per-peripheral
 * scoreboards classify the same accesses as expected TL-UL errors.
 * N is drawn per transfer from `len_class` (0: 1, 1: 2-16, 2: 17-64, 3: a class per transfer);
 * `clk_class` selects the UART baud (0: the console's, 1 Mbaud on sim_dv; 1: 1.5 Mbaud) and the
 * SPI clock divider (0: 1, 1: 0).
 *
 * Built with EG2_PERIPH_INJECT_FAIL the first data check (the first unmapped load's cause) is
 * made against a wrong expected value: the run must fail with `PERIPH FAIL`.
 *
 * The I2C phase needs the bench's I2C agent as the peer; the Verilator top has none, so each
 * transfer waited out kPolls (regr-vlt 2026-10-26: TIMEOUT at `i2c0 write bytes`). So `devices`
 * defaults to every device but I2C (11): Verilator, which sets no knob, runs UART, SPI and GPIO;
 * the vseq sets `devices` on Xcelium, I2C included (review 2026-10-26: not excluded whole).
 */

#include "hw/top/gpio_regs.h"      // Generated.
#include "hw/top/i2c_regs.h"       // Generated.
#include "hw/top/spi_host_regs.h"  // Generated.
#include "hw/top/uart_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_periph/eg2_periph.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();
EG2_PERIPH_INJECT_FAIL_DEFINE;

#define PERIPH_SB_PARAMS(X) \
  X(devices, 11, 1, 15)     \
  X(len_class, 3, 0, 3)     \
  X(clk_class, 0, 0, 1)     \
  X(rx_bench, 0, 0, 1)      \
  X(rx_count, 8, 1, 32)     \
  X(rx_base, 48, 0, 255)    \
  X(i2c_addr, 51, 8, 119)
EG2_RM_PARAMS(PERIPH_SB_PARAMS)

enum {
  kDevUart = 1,
  kDevSpi = 2,
  kDevI2c = 4,
  kDevGpio = 8,
  kRxMarker = 0xa5,
  kGpioPolls = 1000,
};

static const char *uart_name(uint32_t i) {
  return i == 0 ? "uart1" : i == 1 ? "uart2" : "uart3";
}

static const char *i2c_name(uint32_t i) { return i == 0 ? "i2c0" : i == 1 ? "i2c1" : "i2c2"; }

static void fill_random(uint8_t *buf, uint32_t len) {
  for (uint32_t i = 0; i < len; ++i) {
    buf[i] = (uint8_t)eg2_rand();
  }
}

/** The receive path of one UART: system loopback, or the bench's bytes after the marker. */
static bool uart_rx(uint32_t i, uint32_t nco) {
  eg2_dev_id_t dev = eg2_periph_uart_dev(i);
  const char *name = uart_name(i);
  uint32_t m = EG2_PARAM(rx_count);
  uint8_t exp[32];
  bool ok = true;
  if (EG2_PARAM(rx_bench) == 0) {
    eg2_periph_uart_setup(dev, nco,
                          (1u << UART_CTRL_TX_BIT) | (1u << UART_CTRL_RX_BIT) |
                              (1u << UART_CTRL_SLPBK_BIT));
    fill_random(exp, m);
    eg2_periph_step(name, "rx loopback bytes", true, m);
    for (uint32_t k = 0; k < m && ok; ++k) {
      ok = eg2_periph_expect(name, "tx fifo room", eg2_periph_uart_put(dev, exp[k]));
    }
  } else {
    eg2_periph_uart_setup(dev, nco, (1u << UART_CTRL_TX_BIT) | (1u << UART_CTRL_RX_BIT));
    for (uint32_t k = 0; k < m; ++k) {
      exp[k] = (uint8_t)(EG2_PARAM(rx_base) + k);
    }
    eg2_periph_step(name, "rx from the bench, bytes", true, m);
    ok = eg2_periph_expect(name, "tx fifo room", eg2_periph_uart_put(dev, kRxMarker));
  }
  ok = ok && eg2_periph_expect(name, "rx bytes arrived", eg2_periph_uart_wait_rx(dev, m));
  for (uint32_t k = 0; k < m && ok; ++k) {
    ok = eg2_periph_check(name, "rx byte", exp[k],
                          eg2_dev_read32(dev, UART_RDATA_REG_OFFSET) & 0xff);
  }
  ok = eg2_periph_expect(name, "tx idle", eg2_periph_uart_wait_idle(dev)) && ok;
  eg2_dev_write32(dev, UART_CTRL_REG_OFFSET, 0);
  return ok;
}

static bool uart_phase(void) {
  uint32_t nco = eg2_periph_uart_nco(EG2_PARAM(clk_class));
  uint8_t buf[kEg2PeriphMaxLen];
  bool ok = true;
  for (uint32_t i = 0; i < kEg2PeriphUarts; ++i) {
    eg2_dev_id_t dev = eg2_periph_uart_dev(i);
    const char *name = uart_name(i);
    uint32_t n = eg2_periph_len(EG2_PARAM(len_class));
    eg2_periph_route_uart(i);
    eg2_periph_uart_setup(dev, nco, 1u << UART_CTRL_TX_BIT);
    fill_random(buf, n);
    eg2_periph_step(name, "tx bytes", true, n);
    bool sent = true;
    for (uint32_t k = 0; k < n && sent; ++k) {
      sent = eg2_periph_uart_put(dev, buf[k]);
    }
    ok = eg2_periph_expect(name, "tx fifo room", sent) && ok;
    ok = eg2_periph_expect(name, "tx idle", eg2_periph_uart_wait_idle(dev)) && ok;
    ok = uart_rx(i, nco) && ok;
  }
  return ok;
}

static bool spi_phase(bool with_host1) {
  uint8_t buf[kEg2PeriphMaxLen];
  bool ok = true;
  for (uint32_t h = 0; h < (with_host1 ? 2u : 1u); ++h) {
    eg2_dev_id_t dev = eg2_periph_spi_dev(h);
    const char *name = h == 0 ? "spi_host0" : "spi_host1";
    uint32_t n = (eg2_periph_len(EG2_PARAM(len_class)) + 3) & ~3u;
    uint32_t m = (eg2_periph_len(EG2_PARAM(len_class)) + 3) & ~3u;
    if (h == 1) {
      eg2_periph_route_spi_host1();
    }
    eg2_periph_spi_setup(dev, EG2_PARAM(clk_class) == 0 ? 1 : 0);
    fill_random(buf, n);
    eg2_periph_step(name, "tx bytes", true, n);
    ok = eg2_periph_expect(name, "tx data written", eg2_periph_spi_push(dev, buf, n)) && ok;
    ok = eg2_periph_expect(name, "tx command", eg2_periph_spi_command(dev, 2, n, false)) && ok;
    eg2_periph_step(name, "rx bytes", true, m);
    ok = eg2_periph_expect(name, "rx command", eg2_periph_spi_command(dev, 1, m, false)) && ok;
    ok = eg2_periph_expect(name, "rx words arrived", eg2_periph_spi_pull(dev, m / 4)) && ok;
    ok = eg2_periph_expect(name, "idle", eg2_periph_spi_wait_idle(dev)) && ok;
    uint32_t st = eg2_periph_spi_status(dev);
    ok = eg2_periph_check(name, "tx words left", 0, eg2_periph_spi_txqd(st)) && ok;
    ok = eg2_periph_check(name, "rx words left", 0, eg2_periph_spi_rxqd(st)) && ok;
    ok = eg2_periph_check(name, "error status", 0,
                          eg2_dev_read32(dev, SPI_HOST_ERROR_STATUS_REG_OFFSET)) &&
         ok;
    eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, 0);
  }
  return ok;
}

static bool i2c_phase(void) {
  uint8_t wr[kEg2PeriphMaxLen];
  uint8_t rd[kEg2PeriphMaxLen];
  uint32_t addr = EG2_PARAM(i2c_addr);
  bool ok = true;
  for (uint32_t i = 0; i < kEg2PeriphI2cs; ++i) {
    eg2_dev_id_t dev = eg2_periph_i2c_dev(i);
    const char *name = i2c_name(i);
    uint32_t n = eg2_periph_len(EG2_PARAM(len_class));
    eg2_periph_route_i2c(i);
    if (!eg2_periph_expect(name, "timing", eg2_periph_i2c_setup(dev))) {
      ok = false;
      continue;
    }
    fill_random(wr, n);
    eg2_periph_step(name, "write bytes", true, n);
    bool done = eg2_periph_expect(name, "write done", eg2_periph_i2c_write(dev, addr, wr, n));
    eg2_periph_step(name, "read bytes", true, n);
    done = done && eg2_periph_expect(name, "read done", eg2_periph_i2c_read(dev, addr, rd, n));
    for (uint32_t k = 0; k < n && done; ++k) {
      done = eg2_periph_check(name, "read byte", wr[k], rd[k]);
    }
    ok = done && ok;
    ok = eg2_periph_check(name, "controller events", 0, eg2_periph_i2c_events(dev)) && ok;
    eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 0);
  }
  return ok;
}

static uint32_t gpio_mask(uint32_t devices, bool with_host1) {
  uint32_t mask = ~(0x1fu << 20);  // GPIO 20-24: the JTAG and strap pads
  if (devices & kDevUart) {
    mask &= ~((1u << 0) | (1u << 1) | (1u << 4) | (1u << 5));
  }
  if (devices & kDevI2c) {
    mask &= ~((1u << 7) | (1u << 8) | (0xfu << 12));
  }
  if (with_host1) {
    mask &= ~(1u << 9);
  }
  return mask;
}

static bool gpio_phase(uint32_t mask) {
  eg2_dev_id_t dev = kEg2DevGpio;
  bool ok = true;
  for (uint32_t g = 0; g < 32; ++g) {
    if (mask & (1u << g)) {
      eg2_periph_pin_out(eg2_periph_gpio_pad(g), kTopEarlgreyPinmuxOutselGpioGpio0 + g);
      eg2_periph_pin_in(kTopEarlgreyPinmuxPeripheralInGpioGpio0 + g, eg2_periph_gpio_pad(g));
    }
  }
  eg2_dev_write32(dev, GPIO_DIRECT_OUT_REG_OFFSET, 0);
  eg2_dev_write32(dev, GPIO_DIRECT_OE_REG_OFFSET, mask);
  eg2_periph_step("gpio", "output enable", true, mask);
  uint32_t out = 0;
  uint32_t writes = eg2_rand_range(4, 8);
  for (uint32_t w = 0; w < writes && ok; ++w) {
    uint32_t v = eg2_rand();
    switch (eg2_rand_range(0, 2)) {
      case 0:
        eg2_dev_write32(dev, GPIO_DIRECT_OUT_REG_OFFSET, v);
        out = v;
        break;
      case 1: {
        uint32_t m = v >> 16;
        eg2_dev_write32(dev, GPIO_MASKED_OUT_LOWER_REG_OFFSET, v);
        out = (out & ~m) | (v & m);
        break;
      }
      default: {
        uint32_t m = v & 0xffff0000u;
        eg2_dev_write32(dev, GPIO_MASKED_OUT_UPPER_REG_OFFSET, v);
        out = (out & ~m) | ((v << 16) & m);
        break;
      }
    }
    eg2_periph_step("gpio", "out", true, out & mask);
    eg2_periph_poll(dev, GPIO_DATA_IN_REG_OFFSET, mask, out & mask, kGpioPolls);
    ok = eg2_periph_check("gpio", "data_in", out & mask,
                          eg2_dev_read32(dev, GPIO_DATA_IN_REG_OFFSET) & mask);
  }
  eg2_dev_write32(dev, GPIO_DIRECT_OE_REG_OFFSET, 0);
  return ok;
}

/** One load and one store at an unmapped offset of a register window: each must fault. */
static bool unmapped_access(const char *inst, uint32_t base, uint32_t offset) {
  eg2_rm_out_t out;
  bool ok;
  uint32_t cause = eg2_rm_probe(eg2_rm_ptr(base + offset), kEg2RmLw, 0, &out);
  ok = eg2_periph_check(inst, "unmapped load mcause", kEg2RmCauseLoadFault, cause);
  cause = eg2_rm_probe(eg2_rm_ptr(base + offset), kEg2RmSw, 0xa5a5a5a5, &out);
  ok = eg2_periph_check(inst, "unmapped store mcause", kEg2RmCauseStoreFault, cause) && ok;
  eg2_periph_step(inst, "unmapped offset faulted", true, offset);
  return ok;
}

/** The unmapped offsets of the run's peripherals (see the file comment). */
static bool unmapped_phase(uint32_t devices, bool with_host1) {
  static const uint32_t kUartBase[kEg2PeriphUarts] = {
      TOP_EARLGREY_UART1_BASE_ADDR, TOP_EARLGREY_UART2_BASE_ADDR, TOP_EARLGREY_UART3_BASE_ADDR};
  enum {
    kUartUnmapped = 0x3c,     // registers up to TIMEOUT_CTRL at 0x30, window 0x40
    kSpiHostUnmapped = 0x3c,  // registers up to EVENT_ENABLE at 0x34, window 0x40
    kGpioUnmapped = 0x3fc,    // registers up to PER_PIN_INTR_CTRL_31 at 0x2fc, window 0x400
  };
  bool ok = true;
  if (devices & kDevUart) {
    for (uint32_t i = 0; i < kEg2PeriphUarts; ++i) {
      ok = unmapped_access(uart_name(i), kUartBase[i], kUartUnmapped) && ok;
    }
  }
  if (devices & kDevSpi) {
    ok = unmapped_access("spi_host0", TOP_EARLGREY_SPI_HOST0_BASE_ADDR, kSpiHostUnmapped) && ok;
    if (with_host1) {
      ok = unmapped_access("spi_host1", TOP_EARLGREY_SPI_HOST1_BASE_ADDR, kSpiHostUnmapped) && ok;
    }
  }
  if (devices & kDevGpio) {
    ok = unmapped_access("gpio", TOP_EARLGREY_GPIO_BASE_ADDR, kGpioUnmapped) && ok;
  }
  return ok;
}

bool eg2_test_main(void) {
  if (!eg2_rm_params_begin()) {
    return false;
  }
  uint32_t devices = EG2_PARAM(devices);
  bool with_host1 = (devices & kDevSpi) && !(devices & kDevUart);
  bool ok = unmapped_phase(devices, with_host1);
  if (devices & kDevUart) {
    ok = uart_phase() && ok;
  }
  if (devices & kDevSpi) {
    ok = spi_phase(with_host1) && ok;
  }
  if (devices & kDevI2c) {
    ok = i2c_phase() && ok;
  }
  if (devices & kDevGpio) {
    ok = gpio_phase(gpio_mask(devices, with_host1)) && ok;
  }
  return eg2_periph_summary() && ok;
}
