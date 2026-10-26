// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// spi_host of the EG2 SystemC model (hw/ip/spi_host: data/spi_host.hjson,
// doc/theory_of_operation.md, doc/programmers_guide.md).
//
// Commands: a write of COMMAND queues a segment of LEN + 1 bytes ("equal to COMMAND.LEN + 1") in
// the 4-deep command FIFO, to the chip select CSID, with CONFIGOPTS's clock, at SPEED (standard,
// dual, quad), in DIRECTION (dummy, RX, TX, bidirectional). The IP runs one segment at a time while
// CONTROL.SPIEN is set. A segment takes 8 * (LEN + 1) / lanes SCK periods, SCK = f_clk / (2 *
// (CLKDIV + 1)), f_clk = io (Eg2System's clock; both instances); CS lead, trail and idle times are not
// modelled. A TX or bidirectional segment takes its bytes from the TX FIFO (72 words): if they are
// not there when the segment ends it stalls (STATUS.TXSTALL) until software writes them. An RX or
// bidirectional segment puts its bytes into the RX FIFO (64 words); without room it stalls
// (STATUS.RXSTALL) until software reads. The peer's bytes come from --stim spi_host<N>.rx=<hex
// bytes>, then 0xff (an idle bus); with --stim spi_host<N>.peer=echo the peer instead answers with
// the bytes it was sent, in order (the chip bench's SPI device agent in its echo mode), 0xff while
// it has none.
// Windows: TXDATA takes the bytes of a write by its byte enables. The RX FIFO holds words: a
// segment's received bytes are packed into words, its last word padded with zeros (the RTL's packer:
// the documentation does not say what a partial word holds); RXDATA's read pops one word.
// ByteOrder is 1 (little-endian): the first byte on the bus is the word's least significant byte.
// Status: READY (room in the command FIFO), ACTIVE (a segment runs), the FIFO levels in words,
// TXWM (TX words below CONTROL.TX_WATERMARK), RXWM (RX words at least CONTROL.RX_WATERMARK).
// Interrupts: spi_event (status), the OR of the EVENT_ENABLE'd events RXFULL, TXEMPTY, RXWM, TXWM,
// READY, IDLE; error, an ERROR_STATUS bit set by an event whose ERROR_ENABLE bit is set
// (ACCESSINVAL always). "All of these programming violations ... will also halt the IP until the
// corresponding bit is cleared": the model checks CMDBUSY (COMMAND while not READY), OVERFLOW
// (TXDATA write to a full FIFO), UNDERFLOW (RXDATA read of an empty FIFO), CMDINVAL (SPEED 3, or a
// dual or quad bidirectional segment) and CSIDINVAL (CSID >= NumCS = 1); the halt is modelled as no
// new segment starting while an enabled error is set.
// CONTROL.SW_RST empties the FIFOs and the command queue and stops the segment.
// Output items, one line per segment in spi_host<N>.model.log: "cs<CSID> <dir> x<lanes> <csaat>
// <bytes on the bus, hex>", the bytes sent (TX, bidirectional) or received (RX), none for dummy.

#ifndef EG2_MODEL_IP_EG2_PERIPH_SPI_H_
#define EG2_MODEL_IP_EG2_PERIPH_SPI_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

class Eg2SpiHost : public Eg2IpModel {
 public:
  static constexpr unsigned kTxWords = 72, kRxWords = 64, kCmdDepth = 4, kNumCs = 1;

  Eg2SpiHost(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    control_ = reg("CONTROL");
    status_ = reg("STATUS");
    configopts_ = reg("CONFIGOPTS");
    csid_ = reg("CSID");
    command_ = reg("COMMAND");
    err_en_ = reg("ERROR_ENABLE");
    err_st_ = reg("ERROR_STATUS");
    ev_en_ = reg("EVENT_ENABLE");
    set_modelled(status_);
    set_modelled(err_st_);
    rx_win_ = uint16_t(win_index("RXDATA"));
    tx_win_ = uint16_t(win_index("TXDATA"));
    peer_ = Eg2PeriphStim::hex_bytes(std::string(d.window) + ".rx");
    auto it = Eg2PeriphStim::all().find(std::string(d.window) + ".peer");
    echo_mode_ = it != Eg2PeriphStim::all().end() && it->second == "echo";
    update();
  }

  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    sw_reset();
  }

  void on_time(uint64_t now) override {
    if (busy_ && now >= seg_end_) finish_segment();
    start_next(now);
    update();
  }

 protected:
  // RXDATA pops a word per read (window-read hook).
  bool win_read(uint16_t win, uint32_t off, uint8_t *data, unsigned len) override {
    if (win != rx_win_) return false;
    uint32_t word = 0;
    if (rx_.empty()) {
      error(2);  // UNDERFLOW
    } else {
      word = rx_.front();
      rx_.pop_front();
    }
    for (unsigned i = 0; i < len && i < 4; ++i) data[i] = uint8_t(word >> (8 * ((off + i) & 3)));
    retry_stalled();
    update();
    return true;
  }
  void win_write(uint16_t win, uint32_t off, unsigned len) override {
    if (win != tx_win_) return;
    const std::vector<uint8_t> &m = win_mem(win);
    if (len != 1 && len != 2 && len != 4) error(5);  // ACCESSINVAL
    for (unsigned i = 0; i < len; ++i) {
      if (tx_.size() >= 4 * kTxWords) {
        error(1);  // OVERFLOW
        break;
      }
      tx_.push_back(m[off + i]);
    }
    retry_stalled();
    update();
  }
  void reg_write(uint16_t r, uint32_t, uint32_t wdata, uint32_t wmask) override {
    uint32_t w = wdata & wmask;
    if (r == control_ && (value(control_) >> 30 & 1)) {
      sw_reset();
    } else if (r == command_) {
      queue_command(w);
    }
    start_next(sys_.now_ps());
    update();
  }

 private:
  struct Seg {
    uint32_t csid;
    unsigned bytes, dir, speed;
    bool csaat;
    uint32_t configopts;
  };
  void sw_reset() {
    tx_.clear();
    rx_.clear();
    cmds_.clear();
    busy_ = stalled_ = false;
    sys_.unschedule(this);
    update();
  }
  void error(unsigned bit) {
    set_value(err_st_, value(err_st_) | (1u << bit));
    if (bit == 5 || ((value(err_en_) >> bit) & 1)) set_intr("error");
  }
  bool halted() const {
    return (value(err_st_) & (value(err_en_) | (1u << 5))) != 0;
  }
  void queue_command(uint32_t w) {
    if (cmds_.size() >= kCmdDepth) {
      error(0);  // CMDBUSY
      return;
    }
    Seg s{value(csid_), ((w >> 5) & 0xfffff) + 1, (w >> 3) & 3, (w >> 1) & 3, (w & 1) != 0,
          value(configopts_)};
    if (s.speed == 3 || (s.dir == 3 && s.speed != 0)) {
      error(3);  // CMDINVAL
      return;
    }
    if (s.csid >= kNumCs) {
      error(4);  // CSIDINVAL
      return;
    }
    cmds_.push_back(s);
  }
  uint64_t segment_time(const Seg &s) const {
    uint64_t clkdiv = s.configopts & 0xffff;
    uint64_t sck_ps = 2 * (clkdiv + 1) * 1000000000000ull / sys_.clock_hz(Eg2System::Clk::kIo);
    unsigned lanes = s.speed == 0 ? 1 : s.speed == 1 ? 2 : 4;
    return uint64_t(s.bytes) * 8 / lanes * sck_ps;
  }
  void start_next(uint64_t now) {
    if (busy_ || cmds_.empty() || !(value(control_) >> 31 & 1) || halted()) return;
    cur_ = cmds_.front();
    cmds_.pop_front();
    busy_ = true;
    stalled_ = false;
    seg_end_ = now + segment_time(cur_);
    sys_.schedule(this, seg_end_);
  }
  void retry_stalled() {
    if (busy_ && stalled_) finish_segment();
    start_next(sys_.now_ps());
  }
  // The segment's data moves at its end; it waits (stalls) for TX bytes or RX room.
  void finish_segment() {
    bool tx = cur_.dir == 2 || cur_.dir == 3, rx = cur_.dir == 1 || cur_.dir == 3;
    if ((tx && tx_.size() < cur_.bytes) || (rx && rx_.size() + (cur_.bytes + 3) / 4 > kRxWords)) {
      stalled_ = true;
      return;
    }
    static const char *kDir[4] = {"dummy", "rx", "tx", "bidir"};
    std::string item = "cs" + std::to_string(cur_.csid) + " " + kDir[cur_.dir] + " x" +
                       std::to_string(cur_.speed == 0 ? 1 : cur_.speed == 1 ? 2 : 4) +
                       (cur_.csaat ? " csaat" : " end");
    uint32_t word = 0;
    for (unsigned i = 0; i < cur_.bytes; ++i) {
      uint8_t out = 0;
      if (tx) {
        out = tx_.front();
        tx_.pop_front();
      }
      if (rx) {
        uint8_t in = 0xff;
        if (echo_mode_) {
          if (!echo_.empty()) {
            in = echo_.front();
            echo_.pop_front();
          }
        } else if (!peer_.empty()) {
          in = peer_.front();
          peer_.erase(peer_.begin());
        }
        word |= uint32_t(in) << (8 * (i & 3));
        if ((i & 3) == 3 || i + 1 == cur_.bytes) {
          rx_.push_back(word);
          word = 0;
        }
        if (!tx) out = in;
      }
      if (tx && echo_mode_) echo_.push_back(out);
      if (cur_.dir != 0) {
        char b[4];
        std::snprintf(b, sizeof b, " %02x", out);
        item += b;
      }
    }
    sys_.emit(desc().window, item);
    busy_ = stalled_ = false;
  }
  void update() {
    uint32_t ctl = value(control_);
    unsigned txw = unsigned((tx_.size() + 3) / 4), rxw = unsigned(rx_.size());
    bool ready = cmds_.size() < kCmdDepth, active = busy_;
    bool txfull = txw >= kTxWords, txempty = tx_.empty(), rxfull = rxw >= kRxWords,
         rxempty = rx_.empty();
    bool txwm = txw < ((ctl >> 8) & 0xff), rxwm = rxw >= (ctl & 0xff);
    uint32_t st = (uint32_t(ready) << 31) | (uint32_t(active) << 30) | (uint32_t(txfull) << 29) |
                  (uint32_t(txempty) << 28) | (uint32_t(stalled_ && (cur_.dir & 2)) << 27) |
                  (uint32_t(txwm) << 26) | (uint32_t(rxfull) << 25) | (uint32_t(rxempty) << 24) |
                  (uint32_t(stalled_ && (cur_.dir & 1)) << 23) | (1u << 22) |
                  (uint32_t(rxwm) << 20) | (uint32_t(cmds_.size()) << 16) |
                  (std::min(rxw, 255u) << 8) | std::min(txw, 255u);
    set_value(status_, st);
    uint32_t ev = (uint32_t(rxfull) << 0) | (uint32_t(txempty) << 1) | (uint32_t(rxwm) << 2) |
                  (uint32_t(txwm) << 3) | (uint32_t(ready) << 4) | (uint32_t(!active) << 5);
    set_intr_level("spi_event", (ev & value(ev_en_)) != 0);
  }

  uint16_t control_, status_, configopts_, csid_, command_, err_en_, err_st_, ev_en_;
  uint16_t rx_win_, tx_win_;
  std::deque<uint8_t> tx_, echo_;
  std::deque<uint32_t> rx_;
  bool echo_mode_ = false;
  std::deque<Seg> cmds_;
  std::vector<uint8_t> peer_;
  Seg cur_{};
  bool busy_ = false, stalled_ = false;
  uint64_t seg_end_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_SPI_H_
