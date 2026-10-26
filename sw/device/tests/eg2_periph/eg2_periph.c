// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_periph/eg2_periph.h"

#include "hw/top/gpio_regs.h"      // Generated.
#include "hw/top/i2c_regs.h"       // Generated.
#include "hw/top/pinmux_regs.h"    // Generated.
#include "hw/top/spi_host_regs.h"  // Generated.
#include "hw/top/uart_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/lib/arch/device.h"
#include "sw/device/lib/base/bitfield.h"
#include "sw/device/lib/dif/dif_i2c.h"

enum {
  // Register polls before a wait gives up. A UART character at 1 Mbaud is 10 us, an I2C byte at
  // 1 MHz 9 us; one poll is at least a few core cycles at 100 MHz.
  kPolls = 1000000,
};

// The first data check of the run is made against a broken expected value (EG2_PERIPH_INJECT_FAIL).
static bool injected;
static uint32_t checks;
static uint32_t fails;

eg2_dev_id_t eg2_periph_uart_dev(uint32_t i) {
  static const eg2_dev_id_t kDevs[kEg2PeriphUarts] = {kEg2DevUart1, kEg2DevUart2, kEg2DevUart3};
  return kDevs[i];
}

eg2_dev_id_t eg2_periph_spi_dev(uint32_t i) {
  return i == 0 ? kEg2DevSpiHost0 : kEg2DevSpiHost1;
}

eg2_dev_id_t eg2_periph_i2c_dev(uint32_t i) {
  static const eg2_dev_id_t kDevs[kEg2PeriphI2cs] = {kEg2DevI2c0, kEg2DevI2c1, kEg2DevI2c2};
  return kDevs[i];
}

void eg2_periph_step(const char *inst, const char *step, bool with_value, uint32_t value) {
  eg2_puts("PERIPH ");
  eg2_puts(inst);
  eg2_puts(" ");
  eg2_puts(step);
  if (with_value) {
    eg2_puts(" 0x");
    eg2_puthex(value);
  }
  eg2_puts("\r\n");
}

bool eg2_periph_check(const char *inst, const char *what, uint32_t exp, uint32_t got) {
  if (kEg2PeriphInjectFail && !injected) {
    injected = true;
    exp ^= 1;
  }
  ++checks;
  if (exp == got) {
    return true;
  }
  ++fails;
  eg2_puts("PERIPH FAIL ");
  eg2_puts(inst);
  eg2_puts(" ");
  eg2_puts(what);
  eg2_puts(" exp 0x");
  eg2_puthex(exp);
  eg2_puts(" got 0x");
  eg2_puthex(got);
  eg2_puts("\r\n");
  return false;
}

bool eg2_periph_expect(const char *inst, const char *what, bool cond) {
  ++checks;
  if (cond) {
    return true;
  }
  ++fails;
  eg2_puts("PERIPH FAIL ");
  eg2_puts(inst);
  eg2_puts(" ");
  eg2_puts(what);
  eg2_puts("\r\n");
  return false;
}

bool eg2_periph_summary(void) {
  eg2_puts("PERIPH summary checks ");
  eg2_putdec(checks);
  eg2_puts(" failed ");
  eg2_putdec(fails);
  eg2_puts("\r\n");
  return fails == 0 && checks != 0;
}

bool eg2_periph_poll(eg2_dev_id_t dev, uint32_t offset, uint32_t mask, uint32_t want,
                     uint32_t max_polls) {
  for (uint32_t i = 0; i < max_polls; ++i) {
    if ((eg2_dev_read32(dev, offset) & mask) == want) {
      return true;
    }
  }
  return false;
}

uint32_t eg2_periph_len(uint32_t len_class) {
  if (len_class > 2) {
    len_class = eg2_rand_range(0, 2);
  }
  switch (len_class) {
    case 0:
      return 1;
    case 1:
      return eg2_rand_range(2, 16);
    default:
      return eg2_rand_range(17, kEg2PeriphMaxLen);
  }
}

// ---- Pin multiplexer ----

void eg2_periph_pin_in(uint32_t periph_in, uint32_t mio) {
  eg2_dev_write32(kEg2DevPinmux, PINMUX_MIO_PERIPH_INSEL_0_REG_OFFSET + 4 * periph_in,
                  kTopEarlgreyPinmuxInselIoa0 + mio);
}

void eg2_periph_pin_out(uint32_t mio, uint32_t outsel) {
  eg2_dev_write32(kEg2DevPinmux, PINMUX_MIO_OUTSEL_0_REG_OFFSET + 4 * mio, outsel);
}

void eg2_periph_route_uart(uint32_t i) {
  static const uint8_t kTx[kEg2PeriphUarts] = {
      kTopEarlgreyPinmuxMioOutIob5, kTopEarlgreyPinmuxMioOutIoa5, kTopEarlgreyPinmuxMioOutIoa1};
  static const uint8_t kRx[kEg2PeriphUarts] = {
      kTopEarlgreyPinmuxMioOutIob4, kTopEarlgreyPinmuxMioOutIoa4, kTopEarlgreyPinmuxMioOutIoa0};
  eg2_periph_pin_in(kTopEarlgreyPinmuxPeripheralInUart1Rx + i, kRx[i]);
  eg2_periph_pin_out(kTx[i], kTopEarlgreyPinmuxOutselUart1Tx + i);
}

void eg2_periph_route_spi_host1(void) {
  eg2_periph_pin_out(kTopEarlgreyPinmuxMioOutIob0, kTopEarlgreyPinmuxOutselSpiHost1Sck);
  eg2_periph_pin_out(kTopEarlgreyPinmuxMioOutIob1, kTopEarlgreyPinmuxOutselSpiHost1Csb);
  for (uint32_t k = 0; k < 4; ++k) {
    eg2_periph_pin_out(kTopEarlgreyPinmuxMioOutIob3 + k, kTopEarlgreyPinmuxOutselSpiHost1Sd0 + k);
    eg2_periph_pin_in(kTopEarlgreyPinmuxPeripheralInSpiHost1Sd0 + k,
                      kTopEarlgreyPinmuxMioOutIob3 + k);
  }
}

void eg2_periph_route_i2c(uint32_t i) {
  static const uint8_t kScl[kEg2PeriphI2cs] = {
      kTopEarlgreyPinmuxMioOutIoa8, kTopEarlgreyPinmuxMioOutIob9, kTopEarlgreyPinmuxMioOutIob11};
  static const uint8_t kSda[kEg2PeriphI2cs] = {
      kTopEarlgreyPinmuxMioOutIoa7, kTopEarlgreyPinmuxMioOutIob10, kTopEarlgreyPinmuxMioOutIob12};
  // Per instance the peripheral inputs and outputs are SDA then SCL.
  eg2_periph_pin_in(kTopEarlgreyPinmuxPeripheralInI2c0Sda + 2 * i, kSda[i]);
  eg2_periph_pin_in(kTopEarlgreyPinmuxPeripheralInI2c0Scl + 2 * i, kScl[i]);
  eg2_periph_pin_out(kSda[i], kTopEarlgreyPinmuxOutselI2c0Sda + 2 * i);
  eg2_periph_pin_out(kScl[i], kTopEarlgreyPinmuxOutselI2c0Scl + 2 * i);
}

uint32_t eg2_periph_gpio_pad(uint32_t gpio) {
  // pinmux_testutils.c kPinmuxTestutilsGpioPads, as MIO indices.
  static const uint8_t kPads[32] = {
      kTopEarlgreyPinmuxMioOutIoa0,  kTopEarlgreyPinmuxMioOutIoa1,  kTopEarlgreyPinmuxMioOutIoa2,
      kTopEarlgreyPinmuxMioOutIoa3,  kTopEarlgreyPinmuxMioOutIoa4,  kTopEarlgreyPinmuxMioOutIoa5,
      kTopEarlgreyPinmuxMioOutIoa6,  kTopEarlgreyPinmuxMioOutIoa7,  kTopEarlgreyPinmuxMioOutIoa8,
      kTopEarlgreyPinmuxMioOutIob6,  kTopEarlgreyPinmuxMioOutIob7,  kTopEarlgreyPinmuxMioOutIob8,
      kTopEarlgreyPinmuxMioOutIob9,  kTopEarlgreyPinmuxMioOutIob10, kTopEarlgreyPinmuxMioOutIob11,
      kTopEarlgreyPinmuxMioOutIob12, kTopEarlgreyPinmuxMioOutIoc9,  kTopEarlgreyPinmuxMioOutIoc10,
      kTopEarlgreyPinmuxMioOutIoc11, kTopEarlgreyPinmuxMioOutIoc12, kTopEarlgreyPinmuxMioOutIor0,
      kTopEarlgreyPinmuxMioOutIor1,  kTopEarlgreyPinmuxMioOutIor2,  kTopEarlgreyPinmuxMioOutIor3,
      kTopEarlgreyPinmuxMioOutIor4,  kTopEarlgreyPinmuxMioOutIor5,  kTopEarlgreyPinmuxMioOutIor6,
      kTopEarlgreyPinmuxMioOutIor7,  kTopEarlgreyPinmuxMioOutIor10, kTopEarlgreyPinmuxMioOutIor11,
      kTopEarlgreyPinmuxMioOutIor12, kTopEarlgreyPinmuxMioOutIor13,
  };
  return kPads[gpio & 31];
}

// ---- UART ----

uint32_t eg2_periph_uart_nco(uint32_t clk_class) {
  return clk_class == 0 ? kUartNCOValue : kUartBaud1M50;
}

void eg2_periph_uart_setup(eg2_dev_id_t dev, uint32_t nco, uint32_t ctrl_bits) {
  eg2_dev_write32(dev, UART_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(dev, UART_FIFO_CTRL_REG_OFFSET,
                  (1u << UART_FIFO_CTRL_RXRST_BIT) | (1u << UART_FIFO_CTRL_TXRST_BIT));
  eg2_dev_write32(dev, UART_CTRL_REG_OFFSET,
                  ctrl_bits | ((nco & UART_CTRL_NCO_MASK) << UART_CTRL_NCO_OFFSET));
}

bool eg2_periph_uart_put(eg2_dev_id_t dev, uint8_t byte) {
  if (!eg2_periph_poll(dev, UART_STATUS_REG_OFFSET, 1u << UART_STATUS_TXFULL_BIT, 0, kPolls)) {
    return false;
  }
  eg2_dev_write32(dev, UART_WDATA_REG_OFFSET, byte);
  return true;
}

bool eg2_periph_uart_wait_idle(eg2_dev_id_t dev) {
  uint32_t idle = 1u << UART_STATUS_TXIDLE_BIT;
  return eg2_periph_poll(dev, UART_STATUS_REG_OFFSET, idle, idle, kPolls);
}

uint32_t eg2_periph_uart_rx_level(eg2_dev_id_t dev) {
  return bitfield_field32_read(eg2_dev_read32(dev, UART_FIFO_STATUS_REG_OFFSET),
                               UART_FIFO_STATUS_RXLVL_FIELD);
}

uint32_t eg2_periph_uart_tx_level(eg2_dev_id_t dev) {
  return bitfield_field32_read(eg2_dev_read32(dev, UART_FIFO_STATUS_REG_OFFSET),
                               UART_FIFO_STATUS_TXLVL_FIELD);
}

bool eg2_periph_uart_wait_rx(eg2_dev_id_t dev, uint32_t n) {
  for (uint32_t i = 0; i < kPolls; ++i) {
    if (eg2_periph_uart_rx_level(dev) >= n) {
      return true;
    }
  }
  return false;
}

// ---- SPI host ----

uint32_t eg2_periph_spi_status(eg2_dev_id_t dev) {
  return eg2_dev_read32(dev, SPI_HOST_STATUS_REG_OFFSET);
}

uint32_t eg2_periph_spi_txqd(uint32_t status) {
  return bitfield_field32_read(status, SPI_HOST_STATUS_TXQD_FIELD);
}

uint32_t eg2_periph_spi_rxqd(uint32_t status) {
  return bitfield_field32_read(status, SPI_HOST_STATUS_RXQD_FIELD);
}

void eg2_periph_spi_setup(eg2_dev_id_t dev, uint32_t clkdiv) {
  uint32_t opts = 0;
  opts = bitfield_field32_write(opts, SPI_HOST_CONFIGOPTS_CLKDIV_FIELD, clkdiv);
  opts = bitfield_field32_write(opts, SPI_HOST_CONFIGOPTS_CSNIDLE_FIELD, 1);
  opts = bitfield_field32_write(opts, SPI_HOST_CONFIGOPTS_CSNLEAD_FIELD, 1);
  opts = bitfield_field32_write(opts, SPI_HOST_CONFIGOPTS_CSNTRAIL_FIELD, 1);
  eg2_dev_write32(dev, SPI_HOST_CONFIGOPTS_REG_OFFSET, opts);
  eg2_dev_write32(dev, SPI_HOST_CSID_REG_OFFSET, 0);
  eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET,
                  (1u << SPI_HOST_CONTROL_SPIEN_BIT) | (1u << SPI_HOST_CONTROL_OUTPUT_EN_BIT));
}

bool eg2_periph_spi_command(eg2_dev_id_t dev, uint32_t dir, uint32_t len, bool csaat) {
  uint32_t ready = 1u << SPI_HOST_STATUS_READY_BIT;
  if (!eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, ready, ready, kPolls)) {
    return false;
  }
  uint32_t cmd = 0;
  cmd = bitfield_bit32_write(cmd, SPI_HOST_COMMAND_CSAAT_BIT, csaat);
  cmd = bitfield_field32_write(cmd, SPI_HOST_COMMAND_SPEED_FIELD, 0);
  cmd = bitfield_field32_write(cmd, SPI_HOST_COMMAND_DIRECTION_FIELD, dir);
  cmd = bitfield_field32_write(cmd, SPI_HOST_COMMAND_LEN_FIELD, len - 1);
  eg2_dev_write32(dev, SPI_HOST_COMMAND_REG_OFFSET, cmd);
  return true;
}

bool eg2_periph_spi_push(eg2_dev_id_t dev, const uint8_t *data, uint32_t len) {
  for (uint32_t i = 0; i < len; i += 4) {
    uint32_t full = 1u << SPI_HOST_STATUS_TXFULL_BIT;
    if (!eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, full, 0, kPolls)) {
      return false;
    }
    uint32_t w = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                 ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    eg2_dev_write32(dev, SPI_HOST_TXDATA_REG_OFFSET, w);
  }
  return true;
}

bool eg2_periph_spi_pull(eg2_dev_id_t dev, uint32_t words) {
  for (uint32_t n = 0; n < words; ++n) {
    uint32_t i = 0;
    while (eg2_periph_spi_rxqd(eg2_periph_spi_status(dev)) == 0) {
      if (++i == kPolls) {
        return false;
      }
    }
    (void)eg2_dev_read32(dev, SPI_HOST_RXDATA_REG_OFFSET);
  }
  return true;
}

bool eg2_periph_spi_wait_idle(eg2_dev_id_t dev) {
  uint32_t mask = (1u << SPI_HOST_STATUS_ACTIVE_BIT) |
                  (SPI_HOST_STATUS_CMDQD_MASK << SPI_HOST_STATUS_CMDQD_OFFSET);
  return eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, mask, 0, kPolls);
}

// ---- I2C host ----

uint32_t eg2_periph_i2c_flag_start(void) { return 1u << I2C_FDATA_START_BIT; }
uint32_t eg2_periph_i2c_flag_stop(void) { return 1u << I2C_FDATA_STOP_BIT; }
uint32_t eg2_periph_i2c_flag_readb(void) { return 1u << I2C_FDATA_READB_BIT; }

bool eg2_periph_i2c_setup(eg2_dev_id_t dev) {
  // The values chip_sw_i2c_tx_rx_vseq writes into the legacy test (kClockPeriodNanos 41,
  // kI2cRiseFallNanos 10, kI2cClockPeriodNanos 1000), with its FastPlus target speed: the chip
  // bench's I2C agent is timed for these.
  dif_i2c_timing_config_t t = {
      .lowest_target_device_speed = kDifI2cSpeedFastPlus,
      .clock_period_nanos = 41,
      .sda_rise_nanos = 10,
      .sda_fall_nanos = 10,
      .scl_period_nanos = 1000,
  };
  dif_i2c_config_t c;
  if (dif_i2c_compute_timing(t, &c) != kDifOk) {
    return false;
  }
  eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 0);
  eg2_dev_write32(dev, I2C_TIMING0_REG_OFFSET,
                  bitfield_field32_write(
                      bitfield_field32_write(0, I2C_TIMING0_THIGH_FIELD, c.scl_time_high_cycles),
                      I2C_TIMING0_TLOW_FIELD, c.scl_time_low_cycles));
  eg2_dev_write32(dev, I2C_TIMING1_REG_OFFSET,
                  bitfield_field32_write(
                      bitfield_field32_write(0, I2C_TIMING1_T_R_FIELD, c.rise_cycles),
                      I2C_TIMING1_T_F_FIELD, c.fall_cycles));
  eg2_dev_write32(dev, I2C_TIMING2_REG_OFFSET,
                  bitfield_field32_write(bitfield_field32_write(0, I2C_TIMING2_TSU_STA_FIELD,
                                                                c.start_signal_setup_cycles),
                                         I2C_TIMING2_THD_STA_FIELD, c.start_signal_hold_cycles));
  eg2_dev_write32(dev, I2C_TIMING3_REG_OFFSET,
                  bitfield_field32_write(bitfield_field32_write(0, I2C_TIMING3_TSU_DAT_FIELD,
                                                                c.data_signal_setup_cycles),
                                         I2C_TIMING3_THD_DAT_FIELD, c.data_signal_hold_cycles));
  eg2_dev_write32(dev, I2C_TIMING4_REG_OFFSET,
                  bitfield_field32_write(bitfield_field32_write(0, I2C_TIMING4_TSU_STO_FIELD,
                                                                c.stop_signal_setup_cycles),
                                         I2C_TIMING4_T_BUF_FIELD, c.stop_signal_hold_cycles));
  eg2_dev_write32(dev, I2C_FIFO_CTRL_REG_OFFSET,
                  (1u << I2C_FIFO_CTRL_RXRST_BIT) | (1u << I2C_FIFO_CTRL_FMTRST_BIT));
  eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 1u << I2C_CTRL_ENABLEHOST_BIT);
  return true;
}

bool eg2_periph_i2c_fmt(eg2_dev_id_t dev, uint32_t fbyte, uint32_t flags) {
  uint32_t full = 1u << I2C_STATUS_FMTFULL_BIT;
  if (!eg2_periph_poll(dev, I2C_STATUS_REG_OFFSET, full, 0, kPolls)) {
    return false;
  }
  eg2_dev_write32(dev, I2C_FDATA_REG_OFFSET, (fbyte & 0xff) | flags);
  return true;
}

bool eg2_periph_i2c_write(eg2_dev_id_t dev, uint32_t addr, const uint8_t *data, uint32_t len) {
  bool ok = eg2_periph_i2c_fmt(dev, addr << 1, eg2_periph_i2c_flag_start());
  for (uint32_t i = 0; i < len && ok; ++i) {
    ok = eg2_periph_i2c_fmt(dev, data[i], i + 1 == len ? eg2_periph_i2c_flag_stop() : 0);
  }
  return ok && eg2_periph_i2c_wait_idle(dev);
}

bool eg2_periph_i2c_read(eg2_dev_id_t dev, uint32_t addr, uint8_t *out, uint32_t len) {
  bool ok = eg2_periph_i2c_fmt(dev, (addr << 1) | 1, eg2_periph_i2c_flag_start()) &&
            eg2_periph_i2c_fmt(dev, len & 0xff,
                               eg2_periph_i2c_flag_readb() | eg2_periph_i2c_flag_stop());
  for (uint32_t i = 0; i < len && ok; ++i) {
    uint32_t polls = 0;
    while (eg2_periph_i2c_rx_level(dev) == 0) {
      if (++polls == kPolls) {
        return false;
      }
    }
    out[i] = (uint8_t)eg2_dev_read32(dev, I2C_RDATA_REG_OFFSET);
  }
  return ok && eg2_periph_i2c_wait_idle(dev);
}

bool eg2_periph_i2c_wait_idle(eg2_dev_id_t dev) {
  uint32_t idle = (1u << I2C_STATUS_HOSTIDLE_BIT) | (1u << I2C_STATUS_FMTEMPTY_BIT);
  return eg2_periph_poll(dev, I2C_STATUS_REG_OFFSET, idle, idle, kPolls);
}

uint32_t eg2_periph_i2c_events(eg2_dev_id_t dev) {
  return eg2_dev_read32(dev, I2C_CONTROLLER_EVENTS_REG_OFFSET);
}

uint32_t eg2_periph_i2c_fmt_level(eg2_dev_id_t dev) {
  return bitfield_field32_read(eg2_dev_read32(dev, I2C_HOST_FIFO_STATUS_REG_OFFSET),
                               I2C_HOST_FIFO_STATUS_FMTLVL_FIELD);
}

uint32_t eg2_periph_i2c_rx_level(eg2_dev_id_t dev) {
  return bitfield_field32_read(eg2_dev_read32(dev, I2C_HOST_FIFO_STATUS_REG_OFFSET),
                               I2C_HOST_FIFO_STATUS_RXLVL_FIELD);
}
