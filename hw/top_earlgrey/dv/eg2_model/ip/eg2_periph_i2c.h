// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// i2c of the EG2 SystemC model (hw/ip/i2c: data/i2c.hjson, doc/theory_of_operation.md,
// doc/programmers_guide.md). Host mode is modelled; target mode is registers only.
//
// Host mode (CTRL.ENABLEHOST): a write of FDATA queues a format entry (FBYTE and the flags START,
// STOP, READB, RCONT, NAKOK) in the 64-entry FMT FIFO; the controller runs one entry at a time:
//   START          a (repeated) START, then FBYTE as the address byte
//   READB          read FBYTE bytes (0: 256) into the 64-entry RX FIFO, acknowledging all but the
//                  last unless RCONT; it waits while the RX FIFO lacks room (the bus is stretched)
//   neither        write FBYTE
//   STOP           a STOP after the entry
// An entry takes 9 SCL periods per byte, SCL period = THIGH + TLOW + T_R + T_F (TIMING0, TIMING1)
// cycles of io_div4 (Eg2System's clock); the START and STOP setup and hold times are not modelled.
// The peer is a target at every address that acknowledges every byte; it answers reads with
// --stim i2c<N>.rx=<hex bytes>, or by default with the data bytes it was written, in order (the
// chip bench's I2C agent in its target mode returns what the host wrote), 0xff when it has none. A
// NACK (--stim i2c<N>.nack=<7-bit address> makes the peer NACK that address) that NAKOK does not
// allow halts the controller: CONTROLLER_EVENTS.NACK and controller_halt (status, "released when the
// bits in CONTROLLER_EVENTS are cleared"); the controller runs no entry while halted.
// Status: FMTFULL, FMTEMPTY, HOSTIDLE (FMT empty and no entry running), RXFULL, RXEMPTY; target side
// TARGETIDLE, TXEMPTY, ACQEMPTY. HOST_FIFO_STATUS: FMTLVL, RXLVL. RDATA pops the RX FIFO.
// Interrupts: fmt_threshold (status, "FMT FIFO level is below the low threshold" FMT_THRESH),
// rx_threshold (status, "RX FIFO level is above the high threshold" RX_THRESH), cmd_complete ("the
// host issues a repeated START or terminates the transaction by issuing STOP"), rx_overflow is not
// raised (the host stretches instead of overflowing), controller_halt.
// FIFO_CTRL: RXRST, FMTRST, ACQRST, TXRST empty their FIFOs.
// Output items, one line per bus event in i2c<N>.model.log: "S <addr> <r|w>" (START with its
// address byte), "W <byte>", "R <byte>", "N" (a NACK received), "P" (STOP).
// Not modelled: target mode (ACQ and TX FIFOs stay empty), clock stretching by the peer, bus
// errors, timeouts, multi-controller arbitration, the line loopback (CTRL.LLPBK), OVRD and VAL.

#ifndef EG2_MODEL_IP_EG2_PERIPH_I2C_H_
#define EG2_MODEL_IP_EG2_PERIPH_I2C_H_

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

class Eg2I2c : public Eg2IpModel {
 public:
  static constexpr unsigned kFifoDepth = 64;

  Eg2I2c(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    ctrl_ = reg("CTRL");
    status_ = reg("STATUS");
    rdata_ = reg("RDATA");
    fdata_ = reg("FDATA");
    fifo_ctrl_ = reg("FIFO_CTRL");
    host_cfg_ = reg("HOST_FIFO_CONFIG");
    host_st_ = reg("HOST_FIFO_STATUS");
    tgt_st_ = reg("TARGET_FIFO_STATUS");
    timing0_ = reg("TIMING0");
    timing1_ = reg("TIMING1");
    events_ = reg("CONTROLLER_EVENTS");
    set_modelled(status_);
    set_modelled(rdata_);
    set_modelled(host_st_);
    set_modelled(tgt_st_);
    set_modelled(events_);
    std::string w = d.window;
    peer_rx_ = Eg2PeriphStim::hex_bytes(w + ".rx");
    nack_addr_ = int(Eg2PeriphStim::u32(w + ".nack", 0xff));
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    fmt_.clear();
    rx_.clear();
    echo_.clear();
    busy_ = false;
    update();
  }
  void on_time(uint64_t now) override {
    if (busy_ && now >= end_) finish_entry();
    start_next(now);
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == fdata_) {
      if (fmt_.size() < kFifoDepth) fmt_.push_back(w & 0x1fff);
    } else if (r == fifo_ctrl_) {
      if (w & 1) rx_.clear();
      if (w & 2) fmt_.clear();
    }
    start_next(sys_.now_ps());
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == rdata_) set_value(rdata_, rx_.empty() ? 0 : rx_.front());
  }
  void reg_after_read(uint16_t r) override {
    if (r == rdata_ && !rx_.empty()) {
      rx_.pop_front();
      if (busy_ && waiting_room_) finish_entry();
      start_next(sys_.now_ps());
      update();
    }
  }

 private:
  bool host_on() const { return value(ctrl_) & 1; }
  bool halted() const { return (value(events_) & 0xf) != 0; }
  uint64_t byte_time() const {
    uint32_t t0 = value(timing0_), t1 = value(timing1_);
    uint64_t cyc = (t0 & 0x1fff) + ((t0 >> 16) & 0x1fff) + (t1 & 0x3ff) + ((t1 >> 16) & 0x1ff);
    if (cyc == 0) cyc = 1;
    return 9 * cyc * 1000000000000ull / sys_.clock_hz(Eg2System::Clk::kIoDiv4);
  }
  void start_next(uint64_t now) {
    if (busy_ || fmt_.empty() || !host_on() || halted()) return;
    cur_ = fmt_.front();
    fmt_.pop_front();
    busy_ = true;
    waiting_room_ = false;
    bool readb = (cur_ >> 10) & 1;
    unsigned n = readb ? ((cur_ & 0xff) ? (cur_ & 0xff) : 256) : 1;
    end_ = now + byte_time() * n;
    sys_.schedule(this, end_);
  }
  void emit(const std::string &s) { sys_.emit(desc().window, s); }
  // The entry's bus activity happens at its end.
  void finish_entry() {
    uint8_t fbyte = uint8_t(cur_);
    bool start = (cur_ >> 8) & 1, stop = (cur_ >> 9) & 1, readb = (cur_ >> 10) & 1,
         nakok = (cur_ >> 12) & 1;
    char b[24];
    bool nack = false;
    if (readb && !start) {
      unsigned n = fbyte ? fbyte : 256;
      if (rx_.size() + n > kFifoDepth) {
        waiting_room_ = true;
        return;
      }
      for (unsigned i = 0; i < n; ++i) {
        uint8_t v = 0xff;
        if (!peer_rx_.empty()) {
          v = peer_rx_.front();
          peer_rx_.erase(peer_rx_.begin());
        } else if (!echo_.empty()) {
          v = echo_.front();
          echo_.pop_front();
        }
        rx_.push_back(v);
        std::snprintf(b, sizeof b, "R %02x", v);
        emit(b);
      }
    } else if (start) {
      std::snprintf(b, sizeof b, "S %02x %c", fbyte >> 1, (fbyte & 1) ? 'r' : 'w');
      emit(b);
      nack = (fbyte >> 1) == nack_addr_;
    } else {
      std::snprintf(b, sizeof b, "W %02x", fbyte);
      emit(b);
      echo_.push_back(fbyte);
    }
    if (nack) {
      emit("N");
      if (!nakok) {
        set_value(events_, value(events_) | 1);  // NACK
      }
    }
    if (stop) {
      emit("P");
      set_intr("cmd_complete");
    } else if (start && started_) {
      set_intr("cmd_complete");  // a repeated START
    }
    started_ = start ? true : (stop ? false : started_);
    busy_ = waiting_room_ = false;
  }
  void update() {
    uint32_t hc = value(host_cfg_);
    bool fmt_empty = fmt_.empty(), idle = fmt_empty && !busy_;
    uint32_t st = (uint32_t(fmt_.size() >= kFifoDepth) << 0) | (uint32_t(rx_.size() >= kFifoDepth) << 1) |
                  (uint32_t(fmt_empty) << 2) | (uint32_t(idle) << 3) | (1u << 4) |
                  (uint32_t(rx_.empty()) << 5) | (1u << 8) | (1u << 9);
    set_value(status_, st);
    set_value(host_st_, uint32_t(fmt_.size()) | (uint32_t(rx_.size()) << 16));
    set_value(tgt_st_, 0);
    set_intr_level("fmt_threshold", fmt_.size() < (hc >> 16 & 0xfff));
    set_intr_level("rx_threshold", rx_.size() > (hc & 0xfff));
    set_intr_level("controller_halt", halted());
  }

  uint16_t ctrl_, status_, rdata_, fdata_, fifo_ctrl_, host_cfg_, host_st_, tgt_st_, timing0_,
      timing1_, events_;
  std::deque<uint32_t> fmt_;
  std::deque<uint8_t> rx_, echo_;
  std::vector<uint8_t> peer_rx_;
  int nack_addr_ = 0xff;
  uint32_t cur_ = 0;
  bool busy_ = false, waiting_room_ = false, started_ = false;
  uint64_t end_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_I2C_H_
