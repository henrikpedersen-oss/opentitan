// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef OPENTITAN_SW_DEVICE_TESTS_EG2_PERIPH_EG2_PERIPH_H_
#define OPENTITAN_SW_DEVICE_TESTS_EG2_PERIPH_EG2_PERIPH_H_

/**
 * @file
 * @brief What the peripheral tests share (eg2_periph; EG2 verification spec REQ_PBF_13,
 * `chip_sw_eg2_pbf_periph_sb` and `chip_sw_eg2_pbf_periph_fault`).
 *
 * Every register access goes through `eg2_dev()` (eg2_dev_read32 / eg2_dev_write32), so the same
 * source runs in RISC-V mode and, through exact-bounds device capabilities, in CHERIoT mode.
 *
 * The pads are the chip bench's (hw/top_earlgrey/dv/env/chip_if.sv): the bench's agents sit on
 * them, so a test that routes a peripheral elsewhere is not seen.
 *
 *   uart1  TX Iob5  RX Iob4        i2c0  SCL Ioa8   SDA Ioa7
 *   uart2  TX Ioa5  RX Ioa4        i2c1  SCL Iob9   SDA Iob10
 *   uart3  TX Ioa1  RX Ioa0        i2c2  SCL Iob11  SDA Iob12
 *   spi_host0: its dedicated pads  spi_host1: SCK Iob0, CSB Iob1, SD0-3 Iob3-Iob6
 *   gpio i: kPinmuxTestutilsGpioPads[i] (pinmux_testutils.c), the bench's gpios_if
 *
 * Checks print `PERIPH FAIL <instance> <what> exp <x> got <y>`. Built with
 * `EG2_PERIPH_INJECT_FAIL`, the first data check of the run is made against an expected value
 * with bit 0 flipped, so the run must fail with that line (REQ_STE_03).
 */

#include <stdbool.h>
#include <stdint.h>

#include "sw/device/lib/testing/eg2/eg2_test.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Defined by each test source: `EG2_PERIPH_INJECT_FAIL_DEFINE;` sets it from the build's
 * EG2_PERIPH_INJECT_FAIL, so the shared library breaks the first check of the `_inject_fail` builds.
 */
extern const bool kEg2PeriphInjectFail;
#ifdef EG2_PERIPH_INJECT_FAIL
#define EG2_PERIPH_INJECT_FAIL_DEFINE const bool kEg2PeriphInjectFail = true
#else
#define EG2_PERIPH_INJECT_FAIL_DEFINE const bool kEg2PeriphInjectFail = false
#endif

/** The UARTs, SPI hosts and I2Cs the tests drive, by index. */
enum {
  kEg2PeriphUarts = 3,  // uart1-uart3 (uart0 is the console)
  kEg2PeriphSpiHosts = 2,
  kEg2PeriphI2cs = 3,
  kEg2PeriphMaxLen = 64,
};

/** uart1-uart3, spi_host0-1, i2c0-2 as framework devices. */
eg2_dev_id_t eg2_periph_uart_dev(uint32_t i);
eg2_dev_id_t eg2_periph_spi_dev(uint32_t i);
eg2_dev_id_t eg2_periph_i2c_dev(uint32_t i);

/** A step on the console: `PERIPH <inst> <step>`, with a value if `with_value`. */
void eg2_periph_step(const char *inst, const char *step, bool with_value, uint32_t value);

/**
 * A data check: true if `got == exp`; otherwise `PERIPH FAIL <inst> <what> exp <x> got <y>`.
 * With EG2_PERIPH_INJECT_FAIL the first call of the run compares against `exp ^ 1`.
 */
bool eg2_periph_check(const char *inst, const char *what, uint32_t exp, uint32_t got);

/** A condition check (no injection): `PERIPH FAIL <inst> <what>` when false. */
bool eg2_periph_expect(const char *inst, const char *what, bool cond);

/** `PERIPH summary checks <n> failed <m>`; returns true if none failed. */
bool eg2_periph_summary(void);

/** Polls the register until (value & mask) == want; false after `max_polls` reads. */
bool eg2_periph_poll(eg2_dev_id_t dev, uint32_t offset, uint32_t mask, uint32_t want,
                     uint32_t max_polls);

/** A transfer length for a length class: 0: 1, 1: 2-16, 2: 17-64, 3: a class drawn here. */
uint32_t eg2_periph_len(uint32_t len_class);

// ---- Pin multiplexer ----

/** Routes MIO pad `mio` (kTopEarlgreyPinmuxMioOut*) to peripheral input `periph_in`. */
void eg2_periph_pin_in(uint32_t periph_in, uint32_t mio);
/** Drives MIO pad `mio` from peripheral output select `outsel` (kTopEarlgreyPinmuxOutsel*). */
void eg2_periph_pin_out(uint32_t mio, uint32_t outsel);
/** Routes uart<i+1>, spi_host1 or i2c<i> to the bench's pads (table above). */
void eg2_periph_route_uart(uint32_t i);
void eg2_periph_route_spi_host1(void);
void eg2_periph_route_i2c(uint32_t i);
/** The GPIO's pad (kPinmuxTestutilsGpioPads) as a MIO index. */
uint32_t eg2_periph_gpio_pad(uint32_t gpio);

// ---- UART ----

/** NCO for a baud class: 0 the console's (kUartNCOValue), 1 1.5 Mbaud (kUartBaud1M50). */
uint32_t eg2_periph_uart_nco(uint32_t clk_class);
/** Resets both FIFOs and sets CTRL (NCO and the enables in `ctrl_bits`). */
void eg2_periph_uart_setup(eg2_dev_id_t dev, uint32_t nco, uint32_t ctrl_bits);
/** Writes a byte once the TX FIFO has room; false if it never had. */
bool eg2_periph_uart_put(eg2_dev_id_t dev, uint8_t byte);
/** Waits for STATUS.TXIDLE. */
bool eg2_periph_uart_wait_idle(eg2_dev_id_t dev);
/** Waits until the RX FIFO holds `n` bytes. */
bool eg2_periph_uart_wait_rx(eg2_dev_id_t dev, uint32_t n);
uint32_t eg2_periph_uart_rx_level(eg2_dev_id_t dev);
uint32_t eg2_periph_uart_tx_level(eg2_dev_id_t dev);

// ---- SPI host ----

/** CONFIGOPTS for CS 0 (CLKDIV, CPOL = CPHA = 0, short CS timings), CSID 0, SPIEN | OUTPUT_EN. */
void eg2_periph_spi_setup(eg2_dev_id_t dev, uint32_t clkdiv);
/** Queues a standard-speed segment: `dir` 1 RX, 2 TX; `len` bytes; `csaat` keeps CS asserted. */
bool eg2_periph_spi_command(eg2_dev_id_t dev, uint32_t dir, uint32_t len, bool csaat);
/** Writes `len` bytes (a multiple of 4) to TXDATA, little-endian. */
bool eg2_periph_spi_push(eg2_dev_id_t dev, const uint8_t *data, uint32_t len);
/** Waits for `words` words in the RX FIFO, then reads them. */
bool eg2_periph_spi_pull(eg2_dev_id_t dev, uint32_t words);
/** Waits until no segment runs and none is queued. */
bool eg2_periph_spi_wait_idle(eg2_dev_id_t dev);
uint32_t eg2_periph_spi_status(eg2_dev_id_t dev);
uint32_t eg2_periph_spi_txqd(uint32_t status);
uint32_t eg2_periph_spi_rxqd(uint32_t status);

// ---- I2C host ----

/** Host timing for a 41 ns clock, FastPlus, 1000 ns SCL period, 10 ns rise and fall (as the
 * legacy i2c_host_tx_rx test with the values chip_sw_i2c_tx_rx_vseq gives it); ENABLEHOST. */
bool eg2_periph_i2c_setup(eg2_dev_id_t dev);
/** Queues a format entry once the FMT FIFO has room. */
bool eg2_periph_i2c_fmt(eg2_dev_id_t dev, uint32_t fbyte, uint32_t flags);
/** FDATA flags. */
uint32_t eg2_periph_i2c_flag_start(void);
uint32_t eg2_periph_i2c_flag_stop(void);
uint32_t eg2_periph_i2c_flag_readb(void);
/** Writes `len` bytes to `addr` (START, bytes, STOP on the last). */
bool eg2_periph_i2c_write(eg2_dev_id_t dev, uint32_t addr, const uint8_t *data, uint32_t len);
/** Reads `len` bytes from `addr` (START, READB len, STOP) into `out`. */
bool eg2_periph_i2c_read(eg2_dev_id_t dev, uint32_t addr, uint8_t *out, uint32_t len);
/** Waits for STATUS.HOSTIDLE. */
bool eg2_periph_i2c_wait_idle(eg2_dev_id_t dev);
/** CONTROLLER_EVENTS (a NACK, a timeout): 0 when nothing went wrong. */
uint32_t eg2_periph_i2c_events(eg2_dev_id_t dev);
uint32_t eg2_periph_i2c_fmt_level(eg2_dev_id_t dev);
uint32_t eg2_periph_i2c_rx_level(eg2_dev_id_t dev);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // OPENTITAN_SW_DEVICE_TESTS_EG2_PERIPH_EG2_PERIPH_H_
