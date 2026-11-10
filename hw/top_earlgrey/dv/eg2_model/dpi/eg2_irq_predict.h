// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Which interrupts the EG2 system model predicts, for the chip bench's interrupt check
// (eg2_sysmodel_irq_class, eg2_sysmodel.sv watch_irqs; 2026-11-07).
//
// Every IP model takes INTR_STATE, INTR_ENABLE and INTR_TEST from Eg2IpModel, so an interrupt
// software raises through INTR_TEST, and every line software has disabled, is a prediction for every
// block. What an IP model must model itself is the hardware event that sets an interrupt: the
// entries below are the interrupts an IP model raises (set_intr / set_intr_level / drive_irq), by
// the key make_ip_model takes (the register window, else the IP name), from a scan of
// eg2_model/ip/*.h on 2026-11-07. "*" is every interrupt of the IP (gpio's by-bit raise,
// alert_handler's class raise, rv_timer's drive_irq).
//
// An interrupt listed here is checked both ways: every edge of the chip's line must have the
// model's within the window, and the other way round. An interrupt not listed (its hardware event
// is not modelled) is checked one way: every edge of the model's line (INTR_TEST, INTR_ENABLE,
// INTR_STATE writes) must have the chip's, and the chip's line must not rise while the model's
// INTR_ENABLE bit for it is 0; the chip's other edges are counted as not predicted.
//
// When an IP model starts raising another interrupt, add it here.

#ifndef EG2_MODEL_DPI_EG2_IRQ_PREDICT_H_
#define EG2_MODEL_DPI_EG2_IRQ_PREDICT_H_

#include <cstring>

namespace eg2_irq_predict {

struct Modelled {
  const char *key;   // make_ip_model's key: a register window or an IP name
  const char *intr;  // an interrupt name, or "*"
};

inline constexpr Modelled kModelled[] = {
    {"adc_ctrl", "match_pending"},
    {"alert_handler", "*"},
    {"aon_timer", "wkup_timer_expired"},
    {"aon_timer", "wdog_timer_bark"},
    {"cheriot_mem_sys", "tbre_done"},
    {"csrng", "cs_cmd_req_done"},
    {"csrng", "cs_entropy_req"},
    {"edn", "edn_cmd_req_done"},
    {"entropy_src", "es_entropy_valid"},
    {"entropy_src", "es_observe_fifo_ready"},
    {"gpio", "*"},
    {"hmac", "hmac_done"},
    {"hmac", "hmac_err"},
    {"i2c", "cmd_complete"},
    {"i2c", "controller_halt"},
    {"i2c", "fmt_threshold"},
    {"i2c", "rx_threshold"},
    {"i3c", "hci"},
    {"i3c", "targ"},
    {"keymgr_dpe", "op_done"},
    {"kmac", "fifo_empty"},
    {"kmac", "kmac_done"},
    {"kmac", "kmac_err"},
    {"otbn", "done"},
    {"otp_ctrl", "otp_error"},
    {"otp_ctrl", "otp_operation_done"},
    {"pwrmgr", "wakeup"},
    {"rram_ctrl.core", "op_done"},
    {"rram_ctrl.core", "rd_full"},
    {"rram_ctrl.core", "rd_lvl"},
    {"rram_ctrl.core", "wr_empty"},
    {"rram_ctrl.core", "wr_lvl"},
    {"rv_timer", "*"},
    {"sensor_ctrl", "io_status_change"},
    {"spi_device", "tpm_header_not_empty"},
    {"spi_device", "tpm_rdfifo_cmd_end"},
    {"spi_device", "tpm_rdfifo_drop"},
    {"spi_host", "error"},
    {"spi_host", "spi_event"},
    {"sysrst_ctrl", "event_detected"},
    {"uart", "rx_overflow"},
    {"uart", "rx_timeout"},
    {"uart", "rx_watermark"},
    {"uart", "tx_done"},
    {"uart", "tx_empty"},
    {"uart", "tx_watermark"},
    {"usbdev", "av_out_empty"},
    {"usbdev", "av_overflow"},
    {"usbdev", "av_setup_empty"},
    {"usbdev", "disconnected"},
    {"usbdev", "frame"},
    {"usbdev", "host_lost"},
    {"usbdev", "link_reset"},
    {"usbdev", "link_resume"},
    {"usbdev", "link_suspend"},
    {"usbdev", "pkt_received"},
    {"usbdev", "pkt_sent"},
    {"usbdev", "powered"},
    {"usbdev", "rx_full"},
};

// Whether the IP model of the window (`window`, IP `ip`) raises interrupt `intr` itself.
inline bool modelled(const char *window, const char *ip, const char *intr) {
  bool by_window = false;
  for (const Modelled &e : kModelled) {
    if (std::strcmp(e.key, window) == 0) by_window = true;
  }
  const char *key = by_window ? window : ip;
  for (const Modelled &e : kModelled) {
    if (std::strcmp(e.key, key) == 0 && (std::strcmp(e.intr, "*") == 0 ||
                                         std::strcmp(e.intr, intr) == 0)) {
      return true;
    }
  }
  return false;
}

}  // namespace eg2_irq_predict

#endif  // EG2_MODEL_DPI_EG2_IRQ_PREDICT_H_
