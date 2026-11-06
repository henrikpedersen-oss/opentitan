// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Peripherals of the EG2 SystemC model (Phase B group: peripherals). Registered through
// EG2_MODELS_PERIPH, at the end of this file.

#ifndef EG2_MODEL_IP_EG2_PERIPH_H_
#define EG2_MODEL_IP_EG2_PERIPH_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_periph_gpio.h"
#include "eg2_periph_i2c.h"
#include "eg2_periph_i3c.h"
#include "eg2_periph_spi.h"
#include "eg2_periph_spi_device.h"
#include "eg2_periph_sys.h"
#include "eg2_periph_usb.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

// uart (hw/ip/uart: data/uart.hjson, README.md, doc/theory_of_operation.md, doc/programmers_guide.md).
//
// Transmit: a byte written to WDATA enters the 32-byte TX FIFO; while CTRL.TX is set the
// transmitter takes the next byte from the FIFO and sends it in one character time (below), so the
// FIFO drains at the baud rate. "If TX is not enabled, written data into the FIFO will be stacked up
// and sent out when TX is enabled": STATUS.TXFULL is then set once 32 bytes wait, and software that
// waits for room waits for ever; the model stops such a run, saying so. With NCO 0 there is no baud
// clock and nothing is sent. Every byte sent is an output item, one line in <instance>.model.log in the
// format of the chip bench's <instance>.chip.log ("%02h %c", eg2_scoreboard.sv); whole lines also
// go to stdout. tx_watermark: "If the FIFO depth is less than the setting [TXILVL]" (status);
// tx_empty: the FIFO is empty (status); tx_done: the last byte has left the transmitter and the FIFO
// is empty (event). STATUS.TXIDLE: the FIFO is empty and nothing is being sent.
//
// Receive: the peer's bytes (--stim uart<N>.rx=<text>, ip/eg2_periph_stim.h) arrive one character
// time apart once CTRL.RX is set: a character is 10 bit times (11 with CTRL.PARITY_EN) at
// f_baud = NCO * f_pclk / 2^20 (programmer's guide), f_pclk the uart's clock, io_div4 (Eg2System's clock). A
// byte that finds the 64-byte RX FIFO full is lost and raises rx_overflow. rx_watermark: the FIFO
// holds at least RXILVL's level (1, 2, 4, 8, 16, 32, 62; status). rx_timeout:
// TIMEOUT_CTRL.EN set and the FIFO not empty for TIMEOUT_CTRL.VAL bit times without a new byte.
//
// Loopbacks: CTRL.SLPBK, "any outgoing bits to TX are received through RX" and the TX line stays
// 1: a byte sent goes into the RX FIFO (while CTRL.RX is set) instead of out. CTRL.LLPBK,
// "incoming bits are forwarded to TX": each byte received from the peer also goes out as an output
// item.
//
// Log: each byte the core reads from RDATA is also a line "< %02h %c" in <instance>.model.log, as
// the bench's chip.log writes the RX side. Debug lines (<instance>.debug.log): CTRL, FIFO_CTRL and
// TIMEOUT_CTRL as written (enables, baud rate, parity, loopbacks, watermarks, FIFO resets), each
// burst sent (the bytes from the first to the transmitter going idle), a byte dropped on a full
// FIFO, every interrupt that rises while enabled; at --periph-debug 2 every byte written, sent,
// received and read.
//
// Not modelled: parity and frame errors, break detection (the peer sends
// valid characters only), the noise filter, OVRD and VAL (the TX pin override and the oversampled
// RX value).
class Eg2Uart : public Eg2IpModel {
 public:
  static constexpr unsigned kTxFifoDepth = 32;  // uart.hjson TxFifoDepth
  static constexpr unsigned kRxFifoDepth = 64;  // uart.hjson RxFifoDepth
  static constexpr uint64_t kStuckPolls = 100000;

  Eg2Uart(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    wdata_ = reg("WDATA");
    status_ = reg("STATUS");
    fifo_status_ = reg("FIFO_STATUS");
    fifo_ctrl_ = reg("FIFO_CTRL");
    rdata_ = reg("RDATA");
    timeout_ctrl_ = reg("TIMEOUT_CTRL");
    set_modelled(rdata_);
    set_modelled(status_);
    set_modelled(fifo_status_);
    peer_all_ = Eg2PeriphStim::text(std::string(d.window) + ".rx");
    peer_ = peer_all_;
    sys_.inputs[std::string(d.window) + ".rx"] = [this](uint32_t b) { peer_byte(uint8_t(b)); };
    debug_irqs();
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    burst_end("then the chip reset");
    fifo_.clear();
    rx_.clear();
    next_char_ = timeout_at_ = tx_end_ = Eg2System::kNever;
    update();
  }
  ~Eg2Uart() override {
    if (!line_.empty()) std::printf("%s| %s\n", desc().window, line_.c_str());
    burst_end("by the end of the run");
  }

  void on_time(uint64_t now) override {
    if (now >= tx_end_) {
      tx_end_ = Eg2System::kNever;
      send(tx_cur_);
      kick(now);
      if (tx_end_ == Eg2System::kNever && fifo_.empty()) {
        set_intr("tx_done");
        burst_end();
      }
    }
    if (now >= next_char_) {
      next_char_ = Eg2System::kNever;
      if (!peer_.empty() && rx_enabled()) {
        uint8_t b = peer_.front();
        peer_.erase(peer_.begin());
        dbgf(2, "received 0x%02x '%c' from the peer", b, printable(b));
        receive(b);
        if (value(ctrl_) & (1u << 5)) out(b);  // LLPBK
        timeout_at_ = timeout_enabled() ? now + bit_time() * timeout_bits() : Eg2System::kNever;
      }
    }
    if (now >= timeout_at_) {
      timeout_at_ = Eg2System::kNever;
      if (!rx_.empty()) set_intr("rx_timeout");
    }
    plan(now);
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    if (r == wdata_ && (wmask & 0xff)) {
      if (fifo_.size() < kTxFifoDepth) {
        fifo_.push_back(uint8_t(wdata));
        dbgf(2, "WDATA 0x%02x '%c' into the TX FIFO (%zu bytes waiting)", unsigned(wdata & 0xff),
             printable(uint8_t(wdata)), fifo_.size());
      } else {
        // Dropped, as the RTL drops it. Software that polls STATUS.txfull first never does this,
        // so in the chip bench it means the model drains slower than the chip (its clock).
        char b[96];
        std::snprintf(b, sizeof b, "%s: byte 0x%02x written to a full TX FIFO, dropped",
                      desc().window, unsigned(wdata & 0xff));
        sys_.note(b);
        dbgf(1, "TX FIFO overflow: WDATA 0x%02x written with %u bytes waiting, dropped",
             unsigned(wdata & 0xff), kTxFifoDepth);
      }
    } else if (r == fifo_ctrl_) {
      uint32_t w = wdata & wmask;
      if (w & 1) {  // RXRST
        dbgf(1, "FIFO_CTRL.RXRST: RX FIFO emptied (%zu bytes dropped)", rx_.size());
        rx_.clear();
      }
      if (w & 2) {  // TXRST
        dbgf(1, "FIFO_CTRL.TXRST: TX FIFO emptied (%zu bytes dropped)", fifo_.size());
        fifo_.clear();
      }
      if ((value(fifo_ctrl_) ^ old) & 0xfc) {
        dbgf(1, "FIFO_CTRL: RX watermark %u bytes, TX watermark below %u bytes",
             kRxLvl[(value(fifo_ctrl_) >> 2) & 7], kTxLvl[(value(fifo_ctrl_) >> 5) & 7]);
      }
    } else if (r == ctrl_ && value(ctrl_) != old) {
      debug_ctrl();
    } else if (r == timeout_ctrl_ && value(timeout_ctrl_) != old) {
      if (timeout_enabled()) dbgf(1, "TIMEOUT_CTRL: RX timeout after %llu bit times",
                                  (unsigned long long)timeout_bits());
      else dbg(1, "TIMEOUT_CTRL: RX timeout off");
    }
    kick(sys_.now_ps());
    update();
    plan(sys_.now_ps());
  }
  void reg_before_read(uint16_t r) override {
    if (r == rdata_) set_value(rdata_, rx_.empty() ? 0 : rx_.front());
  }
  void reg_after_read(uint16_t r) override {
    if (r == rdata_) {
      if (!rx_.empty()) {
        uint8_t b = rx_.front();
        rx_.erase(rx_.begin());
        char item[16];
        std::snprintf(item, sizeof item, "< %02x %c", b, printable(b));
        sys_.emit_in(desc().window, item);
        dbgf(2, "RDATA read 0x%02x '%c' (%zu bytes left in the RX FIFO)", b, printable(b),
             rx_.size());
      } else if (++rx_underflows_ <= kIrqLines) {
        dbg(1, "RDATA read with the RX FIFO empty: reads 0");
      }
      update();
      return;
    }
    if (r != status_) return;
    bool stuck = (value(status_) & 1) && !(value(ctrl_) & 1);
    stuck_polls_ = stuck ? stuck_polls_ + 1 : 0;
    if (stuck_polls_ == kStuckPolls) {
      Eg2ModelStop::request(std::string(desc().window) +
                            ": software waits for room in a full TX FIFO with CTRL.TX clear: "
                            "nothing enabled transmission, so the FIFO never drains");
    }
  }

 private:
  // FIFO_CTRL's RXILVL and TXILVL levels.
  static constexpr unsigned kRxLvl[8] = {1, 2, 4, 8, 16, 32, 62, 62};
  static constexpr unsigned kTxLvl[8] = {1, 2, 4, 8, 16, 16, 16, 16};
  static char printable(uint8_t b) { return (b >= 0x20 && b <= 0x7e) ? char(b) : '.'; }
  void debug_ctrl() {
    uint32_t c = value(ctrl_);
    uint64_t bt = bit_time();
    char baud[48];
    if (bt == 0) std::snprintf(baud, sizeof baud, "NCO 0: no baud clock");
    else std::snprintf(baud, sizeof baud, "NCO 0x%04x: %.0f baud", c >> 16, 1e12 / double(bt));
    dbgf(1, "CTRL: TX %s, RX %s, %s, parity %s, system loopback %s, line loopback %s",
         (c & 1) ? "on" : "off", (c & 2) ? "on" : "off", baud,
         !((c >> 6) & 1) ? "off" : ((c >> 7) & 1) ? "odd" : "even", ((c >> 4) & 1) ? "on" : "off",
         ((c >> 5) & 1) ? "on" : "off");
  }
  // The bytes sent since the transmitter was last idle, as one debug line when it goes idle.
  void burst_add(uint8_t b) {
    if (burst_n_++ < 64) {
      if (b == '\n') burst_ += "\\n";
      else if (b == '\r') burst_ += "\\r";
      else burst_ += printable(b);
    }
  }
  void burst_end(const char *why = "TX idle") {
    if (burst_n_ == 0) return;
    dbgf(1, "sent %llu bytes, %s: \"%s\"%s", (unsigned long long)burst_n_, why, burst_.c_str(),
         burst_n_ > 64 ? " ..." : "");
    burst_.clear();
    burst_n_ = 0;
  }
  bool rx_enabled() const { return (value(ctrl_) >> 1) & 1; }
  bool timeout_enabled() const { return (value(timeout_ctrl_) >> 31) & 1; }
  uint64_t timeout_bits() const { return value(timeout_ctrl_) & 0xffffff; }
  // One bit time in ps at the NCO's baud rate; 0 while the NCO is 0 (no baud clock).
  uint64_t bit_time() const {
    uint64_t nco = value(ctrl_) >> 16;
    if (nco == 0) return 0;
    return uint64_t((__uint128_t(1) << 20) * 1000000000000ull /
                    (__uint128_t(nco) * sys_.clock_hz(Eg2System::Clk::kIoDiv4)));
  }
  uint64_t char_time() const {
    return bit_time() * (((value(ctrl_) >> 6) & 1) ? 11 : 10);
  }
  // The next peer character, and the RX timeout, as the next time to act.
  void plan(uint64_t now) {
    if (next_char_ == Eg2System::kNever && !peer_.empty() && rx_enabled() && char_time() != 0) {
      next_char_ = now + char_time();
    }
    uint64_t t = std::min(std::min(next_char_, timeout_at_), tx_end_);
    if (t == Eg2System::kNever) sys_.unschedule(this);
    else sys_.schedule(this, t);
  }
  void receive(uint8_t b) {
    if (rx_.size() < kRxFifoDepth) {
      rx_.push_back(b);
    } else {
      dbgf(1, "RX FIFO overflow: byte 0x%02x '%c' lost (%u bytes waiting)", b, printable(b),
           kRxFifoDepth);
      set_intr("rx_overflow");
    }
  }
  // A byte from the peer at run time (the chip bench's UART agent, Eg2System::inputs), received
  // now, as the chip received it.
  void peer_byte(uint8_t b) {
    if (!rx_enabled()) {
      dbgf(2, "byte 0x%02x '%c' from the peer ignored: CTRL.RX is off", b, printable(b));
      return;
    }
    uint64_t now = sys_.now_ps();
    dbgf(2, "received 0x%02x '%c' from the peer", b, printable(b));
    receive(b);
    if (value(ctrl_) & (1u << 5)) out(b);  // LLPBK
    timeout_at_ = timeout_enabled() ? now + bit_time() * timeout_bits() : Eg2System::kNever;
    plan(now);
    update();
  }
  // The transmitter takes the next byte from the FIFO, if it is free, enabled and has a baud clock.
  void kick(uint64_t now) {
    if (tx_end_ != Eg2System::kNever || !(value(ctrl_) & 1) || fifo_.empty() || char_time() == 0)
      return;
    tx_cur_ = fifo_.front();
    fifo_.erase(fifo_.begin());
    tx_end_ = now + char_time();
  }
  void send(uint8_t b) {
    burst_add(b);
    if (value(ctrl_) & (1u << 4)) {  // SLPBK: into the receiver, the TX line stays 1
      dbgf(2, "sent 0x%02x '%c' into the receiver (system loopback)", b, printable(b));
      if (rx_enabled()) receive(b);
      return;
    }
    out(b);
  }
  // A byte on the TX line: an output item, and the console line on stdout.
  void out(uint8_t b) {
    char item[16];
    std::snprintf(item, sizeof item, "%02x %c", b, printable(b));
    dbgf(2, "sent 0x%02x '%c'", b, printable(b));
    sys_.emit(desc().window, item);
    if (b == '\n' || b == '\r') {
      if (!line_.empty()) std::printf("%s| %s\n", desc().window, line_.c_str());
      line_.clear();
    } else {
      line_ += char(b);
    }
  }
  // STATUS: 0 txfull, 1 rxfull, 2 txempty, 3 txidle, 4 rxidle, 5 rxempty.
  // FIFO_STATUS: txlvl 7:0, rxlvl 23:16.
  void update() {
    uint32_t fc = value(fifo_ctrl_);
    bool empty = fifo_.empty();
    uint32_t st = 1u << 4;
    if (fifo_.size() == kTxFifoDepth) st |= 1u << 0;
    if (rx_.size() == kRxFifoDepth) st |= 1u << 1;
    if (empty) st |= 1u << 2;
    if (empty && tx_end_ == Eg2System::kNever) st |= 1u << 3;
    if (rx_.empty()) st |= 1u << 5;
    set_value(status_, st);
    set_value(fifo_status_, uint32_t(fifo_.size()) | (uint32_t(rx_.size()) << 16));
    set_intr_level("tx_watermark", fifo_.size() < kTxLvl[(fc >> 5) & 7]);
    set_intr_level("tx_empty", empty);
    set_intr_level("rx_watermark", rx_.size() >= kRxLvl[(fc >> 2) & 7]);
  }
  uint16_t ctrl_, wdata_, status_, fifo_status_, fifo_ctrl_, rdata_, timeout_ctrl_;
  std::vector<uint8_t> fifo_, rx_, peer_, peer_all_;
  uint64_t stuck_polls_ = 0;
  uint64_t next_char_ = Eg2System::kNever, timeout_at_ = Eg2System::kNever;
  uint64_t tx_end_ = Eg2System::kNever;  // when the byte being sent has left
  uint8_t tx_cur_ = 0;
  std::string line_;
  std::string burst_;
  uint64_t burst_n_ = 0;
  unsigned rx_underflows_ = 0;
};

}  // namespace eg2_model

// This group's entries of kModels (eg2_ip_models.cc): {window name or IP type, factory}.
#define EG2_MODELS_PERIPH \
  {"uart", make<eg2_model::Eg2Uart>}, \
  {"gpio", make<eg2_model::Eg2Gpio>}, \
  {"spi_host", make<eg2_model::Eg2SpiHost>}, \
  {"i2c", make<eg2_model::Eg2I2c>}, \
  {"i3c", make<eg2_model::Eg2I3c>}, \
  {"spi_device", make<eg2_model::Eg2SpiDevice>}, \
  {"sysrst_ctrl", make<eg2_model::Eg2SysrstCtrl>}, \
  {"adc_ctrl", make<eg2_model::Eg2AdcCtrl>}, \
  {"pinmux", make<eg2_model::Eg2Pinmux>}, \
  {"usbdev", make<eg2_model::Eg2Usbdev>},

#endif  // EG2_MODEL_IP_EG2_PERIPH_H_
