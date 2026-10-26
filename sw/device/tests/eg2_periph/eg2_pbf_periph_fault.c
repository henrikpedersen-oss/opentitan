// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

/**
 * chip_sw_eg2_pbf_periph_fault (EG2 verification spec REQ_PBF_13, "Post-boot faults"): a transfer
 * on UART, SPI host or I2C, and a reset of the device in the middle of it. Check: the transfer is
 * aborted (the device idle, its FIFOs empty, no error raised by the reset; after an rstmgr reset
 * its registers at their reset values), and one short transfer works again afterwards. Whether a
 * partial frame left the pins is the chip bench's check (its agents and the per-peripheral
 * scoreboards).
 *
 * Knobs:
 *   device       0 uart1, 1 spi_host0, 2 i2c0, 3 drawn from the seed
 *   fault        0 the IP's own reset: uart FIFO_CTRL.TXRST, then CTRL.TX cleared once the
 *                  character in flight has left (STATUS.TXIDLE); spi_host CONTROL.SW_RST until
 *                  the IP is idle and empty, then cleared; i2c FIFO_CTRL.FMTRST | RXRST with
 *                  CTRL.ENABLEHOST cleared;
 *                1 rstmgr's software reset of the device (SW_RST_CTRL_N; spi_host0 and i2c0 have
 *                  one, uart1 has none: the test then uses fault 0 and says so)
 *   fault_point  the reset after the first byte (0), in the middle (1), before the last byte (2)
 *                of the transfer (spi_host: in 4-byte words)
 *   len_class    the transfer's length: 0: 16, 1: 17-32, 2: 33-64, 3: drawn from 16-64
 *   clk_class    as chip_sw_eg2_pbf_periph_sb: UART baud, SPI clock divider
 *   i2c_addr     the I2C target's 7-bit address
 *   model_gaps   skips what the EG2 SystemC model does not model (eg2sim runs only):
 *
 * | Skipped with model_gaps = 1                        | Why (the model's documentation)          |
 * |----------------------------------------------------|------------------------------------------|
 * | fault 1: registers at reset values, device idle    | rstmgr SW_RST_CTRL_N is storage only in  |
 * |   and empty after the rstmgr reset (the IP's own   |   eg2sim (status/dv_todo.md G2): the     |
 * |   reset is applied in its place, so the recovery   |   device is not reset                    |
 * |   transfer starts from an idle device)             |                                          |
 *
 * An escalation as the fault (spec row "device software reset, escalation") is not here: it needs
 * a second boot that the framework does not give in CHERIoT mode (the start-up code ends the test
 * on any trap, and the status module fails any reset reason but power-on);
 * eg2_alert_escalation_smoketest covers escalation in RISC-V mode.
 *
 * Built with EG2_PERIPH_INJECT_FAIL the first check after the fault is made against a wrong
 * expected value: the run must fail with `PERIPH FAIL`.
 */

#include "hw/top/i2c_regs.h"       // Generated.
#include "hw/top/rstmgr_regs.h"    // Generated.
#include "hw/top/spi_host_regs.h"  // Generated.
#include "hw/top/uart_regs.h"      // Generated.
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"
#include "sw/device/tests/eg2_periph/eg2_periph.h"
#include "sw/device/tests/eg2_regmem/eg2_regmem.h"

EG2_TEST_CONFIG();
EG2_PERIPH_INJECT_FAIL_DEFINE;

#define PERIPH_FAULT_PARAMS(X) \
  X(device, 3, 0, 3)           \
  X(fault, 0, 0, 1)            \
  X(fault_point, 1, 0, 2)      \
  X(len_class, 3, 0, 3)        \
  X(clk_class, 0, 0, 1)        \
  X(i2c_addr, 51, 8, 119)      \
  X(model_gaps, 0, 0, 1)
EG2_RM_PARAMS(PERIPH_FAULT_PARAMS)

enum {
  kPolls = 1000000,
  kRecoveryLen = 4,
};

/** The transfer's length: 16-64 bytes by `len_class`. */
static uint32_t fault_len(void) {
  uint32_t c = EG2_PARAM(len_class);
  if (c > 2) {
    return eg2_rand_range(16, 64);
  }
  return c == 0 ? 16 : c == 1 ? eg2_rand_range(17, 32) : eg2_rand_range(33, 64);
}

/** Where the fault comes, in units of `unit` bytes, for a transfer of `n` bytes. */
static uint32_t fault_at(uint32_t n, uint32_t unit) {
  switch (EG2_PARAM(fault_point)) {
    case 0:
      return unit;
    case 1:
      return ((n / 2) / unit) * unit < unit ? unit : ((n / 2) / unit) * unit;
    default:
      return n - unit;
  }
}

static void rstmgr_pulse(uint32_t sw_reset) {
  uint32_t off = RSTMGR_SW_RST_CTRL_N_0_REG_OFFSET + 4 * sw_reset;
  eg2_dev_write32(kEg2DevRstmgr, off, 0);
  (void)eg2_dev_read32(kEg2DevRstmgr, off);
  eg2_dev_write32(kEg2DevRstmgr, off, 1);
}

static bool uart_fault(void) {
  const char *name = "uart1";
  eg2_dev_id_t dev = kEg2DevUart1;
  uint32_t nco = eg2_periph_uart_nco(EG2_PARAM(clk_class));
  uint32_t n = fault_len();
  uint32_t k = fault_at(n, 1);
  bool ok = true;
  eg2_periph_route_uart(0);
  eg2_periph_uart_setup(dev, nco, 1u << UART_CTRL_TX_BIT);
  eg2_periph_step(name, "transfer bytes", true, n);
  for (uint32_t i = 0; i < k && ok; ++i) {
    ok = eg2_periph_expect(name, "tx fifo room", eg2_periph_uart_put(dev, (uint8_t)eg2_rand()));
  }
  if (EG2_PARAM(fault) == 1) {
    eg2_periph_step(name, "fault rstmgr: uart has no rstmgr software reset, the IP's reset instead",
                    false, 0);
  }
  eg2_periph_step(name, "fault ip_reset after bytes", true, k);
  eg2_dev_write32(dev, UART_FIFO_CTRL_REG_OFFSET, 1u << UART_FIFO_CTRL_TXRST_BIT);
  ok = eg2_periph_expect(name, "tx idle after the reset", eg2_periph_uart_wait_idle(dev)) && ok;
  eg2_dev_write32(dev, UART_CTRL_REG_OFFSET, (nco & UART_CTRL_NCO_MASK) << UART_CTRL_NCO_OFFSET);
  ok = eg2_periph_check(name, "tx fifo level after the reset", 0,
                        eg2_periph_uart_tx_level(dev)) &&
       ok;
  uint32_t st = eg2_dev_read32(dev, UART_STATUS_REG_OFFSET);
  uint32_t empty_idle = (1u << UART_STATUS_TXEMPTY_BIT) | (1u << UART_STATUS_TXIDLE_BIT);
  ok = eg2_periph_check(name, "status txempty txidle", empty_idle, st & empty_idle) && ok;

  // Recovery: a short transfer through the system loopback, checked, then one on the pin.
  uint8_t exp[kRecoveryLen];
  eg2_periph_uart_setup(dev, nco,
                        (1u << UART_CTRL_TX_BIT) | (1u << UART_CTRL_RX_BIT) |
                            (1u << UART_CTRL_SLPBK_BIT));
  eg2_periph_step(name, "recovery loopback bytes", true, kRecoveryLen);
  for (uint32_t i = 0; i < kRecoveryLen; ++i) {
    exp[i] = (uint8_t)eg2_rand();
    ok = eg2_periph_expect(name, "tx fifo room", eg2_periph_uart_put(dev, exp[i])) && ok;
  }
  bool rx = eg2_periph_expect(name, "rx bytes arrived",
                              eg2_periph_uart_wait_rx(dev, kRecoveryLen));
  for (uint32_t i = 0; i < kRecoveryLen && rx; ++i) {
    rx = eg2_periph_check(name, "recovery rx byte", exp[i],
                          eg2_dev_read32(dev, UART_RDATA_REG_OFFSET) & 0xff);
  }
  ok = rx && ok;
  eg2_periph_uart_setup(dev, nco, 1u << UART_CTRL_TX_BIT);
  eg2_periph_step(name, "recovery tx bytes", true, kRecoveryLen);
  for (uint32_t i = 0; i < kRecoveryLen; ++i) {
    ok = eg2_periph_expect(name, "tx fifo room", eg2_periph_uart_put(dev, (uint8_t)eg2_rand())) &&
         ok;
  }
  ok = eg2_periph_expect(name, "tx idle", eg2_periph_uart_wait_idle(dev)) && ok;
  eg2_dev_write32(dev, UART_CTRL_REG_OFFSET, 0);
  return ok;
}

static bool spi_fault(void) {
  const char *name = "spi_host0";
  eg2_dev_id_t dev = kEg2DevSpiHost0;
  uint32_t clkdiv = EG2_PARAM(clk_class) == 0 ? 1 : 0;
  uint32_t n = (fault_len() + 3) & ~3u;
  uint32_t k = fault_at(n, 4);
  uint8_t buf[kEg2PeriphMaxLen];
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = true;
  for (uint32_t i = 0; i < n; ++i) {
    buf[i] = (uint8_t)eg2_rand();
  }
  eg2_periph_spi_setup(dev, clkdiv);
  eg2_periph_step(name, "transfer bytes", true, n);
  // Only the first k bytes are given: the segment stalls there (STATUS.TXSTALL).
  ok = eg2_periph_expect(name, "tx data written", eg2_periph_spi_push(dev, buf, k)) && ok;
  ok = eg2_periph_expect(name, "tx command", eg2_periph_spi_command(dev, 2, n, false)) && ok;
  uint32_t stall = 1u << SPI_HOST_STATUS_TXSTALL_BIT;
  ok = eg2_periph_expect(name, "stalled for data",
                         eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, stall, stall, kPolls)) &&
       ok;
  uint32_t idle_mask = (1u << SPI_HOST_STATUS_ACTIVE_BIT) |
                       (SPI_HOST_STATUS_TXQD_MASK << SPI_HOST_STATUS_TXQD_OFFSET) |
                       (SPI_HOST_STATUS_RXQD_MASK << SPI_HOST_STATUS_RXQD_OFFSET) |
                       (SPI_HOST_STATUS_CMDQD_MASK << SPI_HOST_STATUS_CMDQD_OFFSET);
  uint32_t en = (1u << SPI_HOST_CONTROL_SPIEN_BIT) | (1u << SPI_HOST_CONTROL_OUTPUT_EN_BIT);
  if (EG2_PARAM(fault) == 1) {
    eg2_periph_step(name, "fault rstmgr after bytes", true, k);
    rstmgr_pulse(kTopEarlgreyResetManagerSwResetsSpiHost0);
    if (gaps) {
      eg2_periph_step(name, "model_gaps: rstmgr reset not modelled, IP reset instead", false, 0);
      eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, en | (1u << SPI_HOST_CONTROL_SW_RST_BIT));
      eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, idle_mask, 0, kPolls);
      eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, 0);
    } else {
      ok = eg2_periph_check(name, "control after the reset", SPI_HOST_CONTROL_REG_RESVAL,
                            eg2_dev_read32(dev, SPI_HOST_CONTROL_REG_OFFSET)) &&
           ok;
      ok = eg2_periph_check(name, "configopts after the reset", SPI_HOST_CONFIGOPTS_REG_RESVAL,
                            eg2_dev_read32(dev, SPI_HOST_CONFIGOPTS_REG_OFFSET)) &&
           ok;
    }
  } else {
    eg2_periph_step(name, "fault ip_reset after bytes", true, k);
    eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, en | (1u << SPI_HOST_CONTROL_SW_RST_BIT));
    ok = eg2_periph_expect(
             name, "idle and empty in reset",
             eg2_periph_poll(dev, SPI_HOST_STATUS_REG_OFFSET, idle_mask, 0, kPolls)) &&
         ok;
    eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, 0);
  }
  uint32_t st = eg2_periph_spi_status(dev);
  ok = eg2_periph_check(name, "tx words after the reset", 0, eg2_periph_spi_txqd(st)) && ok;
  ok = eg2_periph_check(name, "rx words after the reset", 0, eg2_periph_spi_rxqd(st)) && ok;
  ok = eg2_periph_check(name, "active after the reset", 0,
                        (st >> SPI_HOST_STATUS_ACTIVE_BIT) & 1) &&
       ok;
  ok = eg2_periph_check(name, "error status after the reset", 0,
                        eg2_dev_read32(dev, SPI_HOST_ERROR_STATUS_REG_OFFSET)) &&
       ok;

  // Recovery: a TX and an RX segment of one word.
  eg2_periph_spi_setup(dev, clkdiv);
  eg2_periph_step(name, "recovery bytes", true, kRecoveryLen);
  ok = eg2_periph_expect(name, "tx data written", eg2_periph_spi_push(dev, buf, kRecoveryLen)) &&
       ok;
  ok = eg2_periph_expect(name, "tx command",
                         eg2_periph_spi_command(dev, 2, kRecoveryLen, false)) &&
       ok;
  ok = eg2_periph_expect(name, "rx command",
                         eg2_periph_spi_command(dev, 1, kRecoveryLen, false)) &&
       ok;
  ok = eg2_periph_expect(name, "rx word arrived", eg2_periph_spi_pull(dev, 1)) && ok;
  ok = eg2_periph_expect(name, "idle", eg2_periph_spi_wait_idle(dev)) && ok;
  ok = eg2_periph_check(name, "error status", 0,
                        eg2_dev_read32(dev, SPI_HOST_ERROR_STATUS_REG_OFFSET)) &&
       ok;
  eg2_dev_write32(dev, SPI_HOST_CONTROL_REG_OFFSET, 0);
  return ok;
}

static bool i2c_fault(void) {
  const char *name = "i2c0";
  eg2_dev_id_t dev = kEg2DevI2c0;
  uint32_t addr = EG2_PARAM(i2c_addr);
  uint32_t n = fault_len();
  uint32_t k = fault_at(n, 1);
  bool gaps = EG2_PARAM(model_gaps) != 0;
  bool ok = true;
  eg2_periph_route_i2c(0);
  if (!eg2_periph_expect(name, "timing", eg2_periph_i2c_setup(dev))) {
    return false;
  }
  eg2_periph_step(name, "transfer bytes", true, n);
  // Only the address and the first k bytes, no STOP: the controller waits for more.
  ok = eg2_periph_expect(name, "fmt room",
                         eg2_periph_i2c_fmt(dev, addr << 1, eg2_periph_i2c_flag_start())) &&
       ok;
  for (uint32_t i = 0; i < k && ok; ++i) {
    ok = eg2_periph_expect(name, "fmt room", eg2_periph_i2c_fmt(dev, eg2_rand() & 0xff, 0));
  }
  uint32_t fmtempty = 1u << I2C_STATUS_FMTEMPTY_BIT;
  ok = eg2_periph_expect(name, "bytes taken",
                         eg2_periph_poll(dev, I2C_STATUS_REG_OFFSET, fmtempty, fmtempty, kPolls)) &&
       ok;
  uint32_t rst = (1u << I2C_FIFO_CTRL_FMTRST_BIT) | (1u << I2C_FIFO_CTRL_RXRST_BIT);
  if (EG2_PARAM(fault) == 1) {
    eg2_periph_step(name, "fault rstmgr after bytes", true, k);
    rstmgr_pulse(kTopEarlgreyResetManagerSwResetsI2c0);
    if (gaps) {
      eg2_periph_step(name, "model_gaps: rstmgr reset not modelled, IP reset instead", false, 0);
      eg2_dev_write32(dev, I2C_FIFO_CTRL_REG_OFFSET, rst);
      eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 0);
    } else {
      ok = eg2_periph_check(name, "ctrl after the reset", I2C_CTRL_REG_RESVAL,
                            eg2_dev_read32(dev, I2C_CTRL_REG_OFFSET)) &&
           ok;
      ok = eg2_periph_check(name, "timing0 after the reset", I2C_TIMING0_REG_RESVAL,
                            eg2_dev_read32(dev, I2C_TIMING0_REG_OFFSET)) &&
           ok;
    }
  } else {
    eg2_periph_step(name, "fault ip_reset after bytes", true, k);
    eg2_dev_write32(dev, I2C_FIFO_CTRL_REG_OFFSET, rst);
    eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 0);
  }
  uint32_t hostidle = 1u << I2C_STATUS_HOSTIDLE_BIT;
  bool idle = eg2_periph_poll(dev, I2C_STATUS_REG_OFFSET, hostidle, hostidle, kPolls);
  ok = eg2_periph_check(name, "fmt level after the reset", 0, eg2_periph_i2c_fmt_level(dev)) && ok;
  ok = eg2_periph_check(name, "rx level after the reset", 0, eg2_periph_i2c_rx_level(dev)) && ok;
  ok = eg2_periph_expect(name, "host idle after the reset", idle) && ok;

  // Recovery: a short write and read. The data read is not checked: the peer may still hold the
  // aborted transfer's bytes (the bench's agent and eg2sim's peer return what was written).
  uint8_t wr[kRecoveryLen];
  uint8_t rd[kRecoveryLen];
  for (uint32_t i = 0; i < kRecoveryLen; ++i) {
    wr[i] = (uint8_t)eg2_rand();
  }
  ok = eg2_periph_expect(name, "timing", eg2_periph_i2c_setup(dev)) && ok;
  eg2_periph_step(name, "recovery bytes", true, kRecoveryLen);
  ok = eg2_periph_expect(name, "recovery write done",
                         eg2_periph_i2c_write(dev, addr, wr, kRecoveryLen)) &&
       ok;
  ok = eg2_periph_expect(name, "recovery read done",
                         eg2_periph_i2c_read(dev, addr, rd, kRecoveryLen)) &&
       ok;
  ok = eg2_periph_check(name, "controller events", 0, eg2_periph_i2c_events(dev)) && ok;
  eg2_dev_write32(dev, I2C_CTRL_REG_OFFSET, 0);
  return ok;
}

bool eg2_test_main(void) {
  if (!eg2_rm_params_begin()) {
    return false;
  }
  uint32_t device = EG2_PARAM(device);
  if (device > 2) {
    device = eg2_rand_range(0, 2);
  }
  eg2_periph_step("fault", "device", true, device);
  bool ok;
  switch (device) {
    case 0:
      ok = uart_fault();
      break;
    case 1:
      ok = spi_fault();
      break;
    default:
      ok = i2c_fault();
      break;
  }
  return eg2_periph_summary() && ok;
}
