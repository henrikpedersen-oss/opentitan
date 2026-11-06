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
// Items in i2c<N>.model.log, in the format of the chip bench's i2c<N>.chip.log (eg2_i2c_sb, the chip
// as host): one line per transfer (a START to the next repeated START or STOP), "S <addr> w <data
// bytes>" or "S <addr> r <n> bytes", "S <addr> <r|w> NACK" when the address is not acknowledged;
// and one line "< %02h" per byte the core reads from RDATA.
// Debug lines: CTRL, TIMING0 and TIMING1 (the SCL frequency) as written, each transfer, FIFO
// resets, a FDATA write to a full FMT FIFO (dropped), a NACK and the halt it causes, interrupts
// raised; at --periph-debug 2 every FDATA entry and RDATA read.
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
    debug_irqs();
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    txn_close("cut by a chip reset (no STOP)", false);
    fmt_.clear();
    rx_.clear();
    echo_.clear();
    busy_ = false;
    started_ = false;
    update();
  }
  ~Eg2I2c() override { txn_close("still open at the end of the run (no STOP)", false); }
  void on_time(uint64_t now) override {
    if (busy_ && now >= end_) finish_entry();
    start_next(now);
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == fdata_) {
      if (fmt_.size() < kFifoDepth) {
        fmt_.push_back(w & 0x1fff);
        dbgf(2, "FDATA entry %s (%zu in the FMT FIFO)", entry_text(w & 0x1fff).c_str(),
             fmt_.size());
      } else {
        dbgf(1, "FMT FIFO overflow: FDATA entry %s written with %u entries waiting, dropped",
             entry_text(w & 0x1fff).c_str(), kFifoDepth);
      }
    } else if (r == fifo_ctrl_) {
      if (w & 1) dbgf(1, "FIFO_CTRL.RXRST: RX FIFO emptied (%zu bytes dropped)", rx_.size());
      if (w & 2) dbgf(1, "FIFO_CTRL.FMTRST: FMT FIFO emptied (%zu entries dropped)", fmt_.size());
      if (w & 1) rx_.clear();
      if (w & 2) fmt_.clear();
    } else if (r == ctrl_ && value(ctrl_) != old) {
      uint32_t c = value(ctrl_);
      dbgf(1, "CTRL: host %s, target %s%s%s", (c & 1) ? "on" : "off", (c & 2) ? "on" : "off",
           (c & 4) ? ", line loopback (not modelled)" : "",
           (c & 2) ? " (target mode is not modelled: no transfers reach it)" : "");
    } else if ((r == timing0_ || r == timing1_) && value(r) != old) {
      uint32_t t0 = value(timing0_), t1 = value(timing1_);
      uint64_t cyc = (t0 & 0x1fff) + ((t0 >> 16) & 0x1fff) + (t1 & 0x3ff) + ((t1 >> 16) & 0x1ff);
      dbgf(1, "%s: SCL period %llu cycles (THIGH %u, TLOW %u, T_R %u, T_F %u): %.1f kHz",
           r == timing0_ ? "TIMING0" : "TIMING1", (unsigned long long)cyc, t0 & 0x1fff,
           (t0 >> 16) & 0x1fff, t1 & 0x3ff, (t1 >> 16) & 0x1ff,
           cyc ? double(sys_.clock_hz(Eg2System::Clk::kIoDiv4)) / double(cyc) / 1e3 : 0.0);
    } else if (r == events_ && (old & 0xf) && !halted()) {
      dbg(1, "CONTROLLER_EVENTS cleared: the controller resumes");
    }
    start_next(sys_.now_ps());
    update();
  }
  void reg_before_read(uint16_t r) override {
    if (r == rdata_) set_value(rdata_, rx_.empty() ? 0 : rx_.front());
  }
  void reg_after_read(uint16_t r) override {
    if (r == rdata_ && !rx_.empty()) {
      char item[8];
      std::snprintf(item, sizeof item, "< %02x", rx_.front());
      sys_.emit_in(desc().window, item);
      dbgf(2, "RDATA read 0x%02x (%zu bytes left in the RX FIFO)", rx_.front(), rx_.size() - 1);
      rx_.pop_front();
      if (busy_ && waiting_room_) finish_entry();
      start_next(sys_.now_ps());
      update();
    } else if (r == rdata_ && ++rx_underflows_ <= kIrqLines) {
      dbg(1, "RDATA read with the RX FIFO empty: reads 0");
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
  // An FDATA entry in words: "START 0xa0 (address 0x50, write)", "write 0x12", "read 4 bytes".
  static std::string entry_text(uint32_t e) {
    char b[96];
    uint8_t fbyte = uint8_t(e);
    bool start = (e >> 8) & 1, stop = (e >> 9) & 1, readb = (e >> 10) & 1, rcont = (e >> 11) & 1,
         nakok = (e >> 12) & 1;
    if (start)
      std::snprintf(b, sizeof b, "START 0x%02x (address 0x%02x, %s)", fbyte, fbyte >> 1,
                    (fbyte & 1) ? "read" : "write");
    else if (readb)
      std::snprintf(b, sizeof b, "read %u bytes%s", fbyte ? fbyte : 256u, rcont ? " (RCONT)" : "");
    else std::snprintf(b, sizeof b, "write 0x%02x", fbyte);
    std::string s = b;
    if (stop) s += " then STOP";
    if (nakok) s += " (NAKOK)";
    return s;
  }
  // The transfer in progress (START to repeated START or STOP): its item and debug line.
  void txn_close(const char *how, bool item_out = true) {
    if (!txn_open_) return;
    std::string item = txn_head_;
    std::string text = "transfer to address 0x" + txn_head_.substr(2, 2);
    char b[16];
    if (txn_read_) {
      item += " " + std::to_string(txn_nread_) + " bytes";
      text += ": read " + std::to_string(txn_nread_) + " bytes";
      for (size_t i = 0; i < txn_data_.size() && i < 16; ++i) {
        std::snprintf(b, sizeof b, " %02x", txn_data_[i]);
        text += b;
      }
    } else {
      text += ": wrote " + std::to_string(txn_data_.size()) + " bytes";
      for (size_t i = 0; i < txn_data_.size(); ++i) {
        std::snprintf(b, sizeof b, " %02x", txn_data_[i]);
        item += b;
        if (i < 16) text += b;
      }
    }
    if (txn_data_.size() > 16) text += " ...";
    if (item_out) sys_.emit(desc().window, item);
    dbg(1, text + ", " + how);
    txn_open_ = false;
    txn_data_.clear();
    txn_nread_ = 0;
  }
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
        if (!waiting_room_)
          dbgf(1, "read of %u bytes waits for room in the RX FIFO (%zu of %u bytes used): SCL "
                  "stretched", n, rx_.size(), kFifoDepth);
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
        txn_data_.push_back(v);
        dbgf(2, "read byte 0x%02x from the target", v);
      }
      txn_nread_ += n;
    } else if (start) {
      txn_close("then a repeated START");
      txn_data_.clear();  // bytes of entries with no START before them
      txn_nread_ = 0;
      std::snprintf(b, sizeof b, "S %02x %c", fbyte >> 1, (fbyte & 1) ? 'r' : 'w');
      nack = (fbyte >> 1) == nack_addr_;
      if (nack) {
        sys_.emit(desc().window, std::string(b) + " NACK");
        dbgf(1, "transfer to address 0x%02x (%s): address not acknowledged%s", fbyte >> 1,
             (fbyte & 1) ? "read" : "write",
             nakok ? " (NAKOK: the controller goes on)"
                   : ": CONTROLLER_EVENTS.NACK set, the controller halts until it is cleared");
      } else {
        txn_open_ = true;
        txn_head_ = b;
        txn_read_ = fbyte & 1;
      }
    } else {
      txn_data_.push_back(fbyte);
      echo_.push_back(fbyte);
      dbgf(2, "wrote byte 0x%02x to the target", fbyte);
    }
    if (nack && !nakok) {
      set_value(events_, value(events_) | 1);  // NACK
    }
    if (stop) {
      txn_close("STOP");
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
  bool txn_open_ = false, txn_read_ = false;
  std::string txn_head_;  // "S <addr> <r|w>"
  std::vector<uint8_t> txn_data_;
  unsigned txn_nread_ = 0, rx_underflows_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_I2C_H_
