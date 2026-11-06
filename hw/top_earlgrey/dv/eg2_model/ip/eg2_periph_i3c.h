// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// i3c0 and i3c1 of the EG2 SystemC model (hw/ip/i3c: data/i3c.hjson, README.md,
// doc/theory_of_operation.md, doc/programmers_guide.md, doc/tti.md, doc/registers.md).
//
// The block is an I3C controller, driven through the HCI in PIO mode, and a target, driven through
// the Target Transaction Interface (TTI). eg2sim has no other device on either I3C bus, so the
// model gives the queues, levels and interrupts software sees, accepts what software queues, and
// does not invent what a target or a controller on the bus would answer:
//
//   Queues      each queue and data buffer is a circular buffer of DWORDs inside the message
//               buffer, between the MIN_ADDR and MAX_ADDR of its *_CONFIG register (both
//               inclusive; programmers_guide.md "Buffer allocation"). Its *_STATE register shows
//               FULL, PRE (data available), and the read and write pointers, which step from
//               MIN_ADDR and wrap after MAX_ADDR. The data is kept by the model, not written into
//               the BUFFER window.
//   Controller  the HCI ports (HCI_PORTS): COMMAND_QUEUE_PORT takes command descriptors (two
//               DWORDs each), XFER_DATA_PORT takes TX data and gives RX data, RESPONSE_QUEUE_PORT
//               and IBI_PORT give responses and IBI data. QUEUE_STATUS_LEVEL (CMD_QUEUE_FREE_LVL in
//               commands, RESPONSE_BUFFER_LVL) and DATA_BUFFER_STATUS_LEVEL (TX_BUF_FREE_LVL,
//               RX_BUF_LVL in DWORDs) follow them. While HC_CONTROL.BUS_ENABLE, PIO_CONTROL.ENABLE
//               and PIO_CONTROL.RS are set, the controller takes the next command from the queue
//               and starts it on the bus (PRESENT_STATE.AC_CURRENT_OWN follows BUS_ENABLE). With no
//               target on the bus the command never completes in the model: no response, no RX
//               data, and its TX data stays in the buffer.
//   Target      the TTI ports (TTI_PORTS, in tti.md's order: Rx Descriptor, Rx Data, IBI Status
//               Descriptor, IBI Data, Asynchronous Event, Target 0-3 Tx Descriptor, Target 0-3 Tx
//               Data): software's Tx descriptors and data, and IBI requests, wait for a controller
//               that never comes; the Rx and asynchronous-event queues stay empty. The target logic
//               is active while TARG_CONTROL.EN is set: TARG_STATUS.ACTIVE and CONNECTED,
//               TARG_TX_QUEUE_STATUS_LEVEL_n and TARG_BUF_STATUS_LEVEL show the free space then,
//               and read 0 (their reset value) while it is off.
//   Errors      BUFFER_STATUS.HCIQ_ERR and TTIQ_ERR: "a write to a full or read-only queue, or a
//               read from an empty or write-only queue" (the access is dropped, a read gives 0).
//   Resets      RESET_CONTROL's bits empty their queue (SOFT_RST all of the controller's, and the
//               command it runs); BUFFER_CTRL.CLEAR empties every queue; TARG_CONTROL.SW_RESET
//               empties the target's queues (the target's registers are not reset by the model).
//   Interrupts  PIO_INTR_STATUS, INTR_STATUS and TARG_INTR_STATUS: a level status bit
//               (CMD_QUEUE_READY, RESP_READY, TX_THLD, RX_THLD, IBI_STATUS_THLD; the target's
//               TXn_DESC_READY, TXn_THLD, RX_DESC_READY, IBI_STATUS_THLD, ASYNC_EVT_READY) follows
//               its condition against its threshold (QUEUE_THLD_CTRL, DATA_BUFFER_THLD_CTRL: 2^(N+1)
//               DWORDs; TARG_QUEUE_THLD_CTRL, TARG_TX_THLD_CTRL_n: N, 0 never) while its
//               *_STATUS_ENABLE bit is set (the status enable records it, which matches the reset
//               values of 0); a *_FORCE write sets the bits it names (a level bit stays forced
//               until the next write of that *_FORCE register). INTR_STATE.hci (status) is any
//               PIO_INTR_STATUS or INTR_STATUS bit whose *_SIGNAL_ENABLE bit is set;
//               INTR_STATE.targ the same for TARG_INTR_STATUS.
//   Waiting     software that reads one of the registers the model keeps, or a port, kPollLimit
//               times with no write to the block waits for the bus: the run stops, saying what is
//               outstanding (a command started, commands queued with the controller stopped, the
//               target waiting for a controller).
//
// The command descriptors are printed by the MIPI I3C HCI 1.2 layout the documents name (bits 2:0
// the attribute, 6:3 the TID; for a regular or immediate transfer 14:7 the CCC, 15 CP, 20:16 the DAT
// index, 29 RnW, 31 TOC, and a regular transfer's length in the second DWORD's 31:16), together
// with the raw DWORDs: the repository's documents do not give the layout.
// Items in i3c<N>.model.log (the chip bench has no I3C log to follow): "start <DWORD0> <DWORD1>"
// for each command the controller starts on the bus. Debug lines: HC_CONTROL, PIO_CONTROL,
// TARG_CONTROL, TARG_ENABLE and the queue configuration as written, each command queued (decoded)
// and started, TX data and target Tx descriptors queued, IBI requests, queue errors, resets, DAT
// entries, interrupts raised; at --periph-debug 2 every port access.
// Not modelled: the I3C bus and any device on it (so no transfer completes, no response, IBI,
// Hot-Join or dynamic address assignment), the HDR-DDR mode, the timers and timing registers, the
// Standby Controller, dead bus recovery, the reset detector, direct drive, the message buffer as
// memory (the BUFFER window is plain memory), HC_CONTROL.ABORT and RESUME, PIO_CONTROL.ABORT.

#ifndef EG2_MODEL_IP_EG2_PERIPH_I3C_H_
#define EG2_MODEL_IP_EG2_PERIPH_I3C_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"

namespace eg2_model {

class Eg2I3c : public Eg2IpModel {
 public:
  Eg2I3c(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    // The controller's queues (HCI queues: MAX_ADDR at 21:12) and data buffers (25:16).
    cmd_ = add_ring("COMMAND_QUEUE", 12, "command queue");
    resp_ = add_ring("RESPONSE_QUEUE", 12, "response queue");
    ctx_ = add_ring("CTRL_TXBUF", 16, "controller TX data buffer");
    crx_ = add_ring("CTRL_RXBUF", 16, "controller RX data buffer");
    ibi_ = add_ring("IBI", 12, "IBI queue");
    ibi_stat_ = add_ring("IBI_STAT", 12, "IBI status queue");
    // The target's.
    for (unsigned t = 0; t < kTargets; ++t) {
      ttxbuf_[t] = add_ring("TARG_TXBUF", 16, "target " + std::to_string(t) + " Tx data buffer", t);
      ttxdesc_[t] =
          add_ring("TARG_TXDESC", 16, "target " + std::to_string(t) + " Tx descriptor queue", t);
    }
    trxbuf_ = add_ring("TARG_RXBUF", 16, "target Rx data buffer");
    trxdesc_ = add_ring("TARG_RXDESC", 16, "target Rx descriptor queue");
    tibi_ = add_ring("TARG_IBI", 16, "target IBI data buffer");
    tibidesc_ = add_ring("TARG_IBIDESC", 16, "target IBI status descriptor queue");
    tasync_ = add_ring("TARG_ASYNC", 16, "target asynchronous event queue");
    for (unsigned t = 0; t < kTargets; ++t)
      tx_level_[t] = reg(("TARG_TX_QUEUE_STATUS_LEVEL_" + std::to_string(t)).c_str());
    for (unsigned t = 0; t < kTargets; ++t)
      tx_thld_[t] = reg(("TARG_TX_THLD_CTRL_" + std::to_string(t)).c_str());
    hc_control_ = reg("HC_CONTROL");
    pio_control_ = reg("PIO_CONTROL");
    reset_control_ = reg("RESET_CONTROL");
    present_ = reg("PRESENT_STATE");
    buffer_ctrl_ = reg("BUFFER_CTRL");
    buffer_status_ = reg("BUFFER_STATUS");
    queue_level_ = reg("QUEUE_STATUS_LEVEL");
    data_level_ = reg("DATA_BUFFER_STATUS_LEVEL");
    queue_thld_ = reg("QUEUE_THLD_CTRL");
    data_thld_ = reg("DATA_BUFFER_THLD_CTRL");
    pio_st_ = reg("PIO_INTR_STATUS");
    pio_st_en_ = reg("PIO_INTR_STATUS_ENABLE");
    pio_sig_en_ = reg("PIO_INTR_SIGNAL_ENABLE");
    pio_force_ = reg("PIO_INTR_FORCE");
    hc_st_ = reg("INTR_STATUS");
    hc_sig_en_ = reg("INTR_SIGNAL_ENABLE");
    hc_force_ = reg("INTR_FORCE");
    targ_control_ = reg("TARG_CONTROL");
    targ_status_ = reg("TARG_STATUS");
    targ_enable_ = reg("TARG_ENABLE");
    targ_st_ = reg("TARG_INTR_STATUS");
    targ_st_en_ = reg("TARG_INTR_STATUS_ENABLE");
    targ_sig_en_ = reg("TARG_INTR_SIGNAL_ENABLE");
    targ_force_ = reg("TARG_INTR_FORCE");
    targ_q_thld_ = reg("TARG_QUEUE_THLD_CTRL");
    targ_q_level_ = reg("TARG_QUEUE_STATUS_LEVEL");
    targ_buf_level_ = reg("TARG_BUF_STATUS_LEVEL");
    hci_win_ = win_index("HCI_PORTS");
    tti_win_ = win_index("TTI_PORTS");
    dat_win_ = win_index("DAT");
    for (uint16_t r : {present_, buffer_status_, queue_level_, data_level_, pio_st_, hc_st_,
                       targ_status_, targ_st_, targ_q_level_, targ_buf_level_, targ_control_}) {
      set_modelled(r);
      watched_.push_back(r);
    }
    for (unsigned t = 0; t < kTargets; ++t) {
      set_modelled(tx_level_[t]);
      watched_.push_back(tx_level_[t]);
    }
    for (Ring &q : rings_) {
      set_modelled(q.state);
      watched_.push_back(q.state);
    }
    watched_.push_back(uint16_t(intr_state_));
    debug_irqs();
    update();
  }
  void on_reset(Eg2ResetCause c) override {
    Eg2IpModel::on_reset(c);
    for (Ring &q : rings_) q.configure(value(q.cfg));  // the *_CONFIG reset values, empty
    running_ = false;
    pio_forced_ = targ_forced_ = 0;
    update();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t v = value(r), w = wdata & wmask;
    if (r == reset_control_) {
      static const char *kRst[6] = {"SOFT_RST", "CMD_QUEUE_RST", "RESP_QUEUE_RST", "TX_FIFO_RST",
                                    "RX_FIFO_RST", "IBI_QUEUE_RST"};
      for (unsigned b = 0; b < 6; ++b) {
        if ((w >> b) & 1) dbgf(1, "RESET_CONTROL.%s", kRst[b]);
      }
      if (w & 1) {
        for (Ring *q : {&cmd_q(), &resp_q(), &ctx_q(), &crx_q(), &ibi_q(), &ibi_stat_q()}) q->clear();
        stop_running("SOFT_RST");
      }
      if (w & 2) {
        cmd_q().clear();
        stop_running("CMD_QUEUE_RST");
      }
      if (w & 4) resp_q().clear();
      if (w & 8) ctx_q().clear();
      if (w & 16) crx_q().clear();
      if (w & 32) {
        ibi_q().clear();
        ibi_stat_q().clear();
      }
    } else if (r == buffer_ctrl_ && (w >> 31)) {
      dbg(1, "BUFFER_CTRL.CLEAR: every queue and buffer emptied");
      for (Ring &q : rings_) q.clear();
      stop_running("BUFFER_CTRL.CLEAR");
    } else if (r == hc_control_ && v != old) {
      dbgf(1, "HC_CONTROL: BUS_ENABLE %u%s%s, I2C_DEV_PRESENT %u, HOT_JOIN_CTRL %u, IBA_INCLUDE %u",
           v >> 31, ((v >> 30) & 1) ? ", RESUME (not modelled)" : "",
           ((v >> 29) & 1) ? ", ABORT (not modelled)" : "", (v >> 7) & 1, (v >> 8) & 1, v & 1);
    } else if (r == pio_control_ && v != old) {
      dbgf(1, "PIO_CONTROL: ENABLE %u, RS %u%s", v & 1, (v >> 1) & 1,
           ((v >> 2) & 1) ? ", ABORT (not modelled)" : "");
    } else if (r == targ_control_) {
      if (w >> 31) {
        dbg(1, "TARG_CONTROL.SW_RESET: the target's queues emptied");
        for (Ring &q : rings_) {
          if (q.target_side) q.clear();
        }
      }
      if (((v ^ old) & 0x7fffffffu) != 0) {
        dbgf(1, "TARG_CONTROL: target %s, standby controller support %u%s", (v & 1) ? "on" : "off",
             (v >> 1) & 1, ((v >> 30) & 1) ? ", Hot-Join request (needs a controller on the bus: "
                                             "never sent here)"
                                           : "");
      }
    } else if (r == targ_enable_ && v != old) {
      dbgf(1, "TARG_ENABLE: virtual targets 0x%x", v & 0xf);
    } else if (r == pio_force_) {
      pio_forced_ = w & 0x1f;  // the level bits stay forced until the next write
      set_value(pio_st_, value(pio_st_) | (w & 0x220));
      if (w) dbgf(1, "PIO_INTR_FORCE 0x%03x", w);
    } else if (r == hc_force_) {
      set_value(hc_st_, value(hc_st_) | (w & 0x7c00));
      if (w) dbgf(1, "INTR_FORCE 0x%04x", w);
    } else if (r == targ_force_) {
      targ_forced_ = w;
      if (w) dbgf(1, "TARG_INTR_FORCE 0x%08x", w);
    } else if (v != old) {
      for (Ring &q : rings_) {
        if (r == q.cfg) {
          q.configure(value(r));
          if (dbg_count("queue configuration writes", 32))
            dbgf(1, "%s: DWORDs 0x%03x to 0x%03x (%u)", desc().regs[r].name, q.min, q.max, q.size());
        }
      }
    }
    start_next();
    update();
  }
  bool win_read(uint16_t win, uint32_t off, uint8_t *data, unsigned len) override {
    if (int(win) != hci_win_ && int(win) != tti_win_) return false;
    unsigned port = off / 4;
    bool hci = int(win) == hci_win_;
    Ring *q = nullptr;
    const char *name = "";
    if (hci) {
      static const char *kHci[4] = {"COMMAND_QUEUE_PORT", "RESPONSE_QUEUE_PORT", "XFER_DATA_PORT",
                                    "IBI_PORT"};
      name = kHci[port & 3];
      if (port == 1) q = &resp_q();
      else if (port == 2) q = &crx_q();
      else if (port == 3) q = &ibi_q();
    } else {
      name = tti_port_name(port);
      if (port == 0) q = &trxdesc_q();
      else if (port == 1) q = &trxbuf_q();
      else if (port == 4) q = &tasync_q();
    }
    uint32_t word = 0;
    if (!q) {
      queue_error(hci, std::string("read of the write-only ") + name);
    } else if (q->data.empty()) {
      queue_error(hci, std::string("read of ") + name + " with the " + q->name + " empty");
    } else {
      word = q->pop();
      char item[32];
      std::snprintf(item, sizeof item, "< %s %08x", name, word);
      sys_.emit_in(inst(), item);
      dbgf(2, "%s read 0x%08x", name, word);
    }
    for (unsigned i = 0; i < len && i < 4; ++i) data[i] = uint8_t(word >> (8 * ((off + i) & 3)));
    poll_watch(0x10000u + (hci ? 0 : 0x100) + port, name, [this] { return outstanding(); });
    update();
    return true;
  }
  void win_write(uint16_t win, uint32_t off, unsigned len) override {
    polls_reset();
    if (int(win) == dat_win_) {
      if (dbg_count("DAT writes", 32))
        dbgf(1, "DAT word %u (entry %u) written", off / 4, off / 8);
      return;
    }
    if (int(win) != hci_win_ && int(win) != tti_win_) return;
    bool hci = int(win) == hci_win_;
    unsigned port = off / 4;
    const std::vector<uint8_t> &m = win_mem(win);
    if (len != 4 || (off & 3)) {
      dbgf(1, "%s: a %u-byte write to a port, dropped (byte writes are not supported)",
           hci ? "HCI_PORTS" : "TTI_PORTS", len);
      return;
    }
    uint32_t word = uint32_t(m[off]) | (uint32_t(m[off + 1]) << 8) | (uint32_t(m[off + 2]) << 16) |
                    (uint32_t(m[off + 3]) << 24);
    if (hci) {
      if (port == 0) {
        if (!push(cmd_q(), word, true, "COMMAND_QUEUE_PORT")) return;
        if (cmd_q().data.size() % 2 == 0) debug_command("queued", cmd_q().data.end()[-2], word);
      } else if (port == 2) {
        if (push(ctx_q(), word, true, "XFER_DATA_PORT"))
          dbgf(2, "XFER_DATA_PORT: TX data 0x%08x (%zu DWORDs waiting)", word,
               ctx_q().data.size());
      } else {
        queue_error(true, std::string("write of the read-only ") +
                              (port == 1 ? "RESPONSE_QUEUE_PORT" : "IBI_PORT"));
      }
    } else {
      const char *name = tti_port_name(port);
      Ring *q = port == 2 ? &tibidesc_q()
                : port == 3 ? &tibi_q()
                : port >= 5 && port <= 8 ? &rings_[ttxdesc_[port - 5]]
                : port >= 9 && port <= 12 ? &rings_[ttxbuf_[port - 9]]
                                          : nullptr;
      if (!q) {
        queue_error(false, std::string("write of the read-only ") + name);
      } else if (push(*q, word, false, name)) {
        if (port >= 5 && port <= 8) {
          dbgf(1, "target %u Tx descriptor 0x%08x: %s, TID %u, %s%s; it waits for a controller's "
                  "read (none on the bus here)", port - 5, word,
               (word >> 31) ? "pending read notification" : "private read",
               (word >> 16) & 0xf,
               (word >> 31) ? (std::to_string((word >> 20) & 3) + " extra bytes").c_str()
                            : (std::to_string(word & 0xffff) + " bytes").c_str(),
               ((word >> 30) & 1) ? ", WROC" : "");
        } else if (port == 2) {
          dbgf(1, "IBI status descriptor 0x%08x: target %u, MDB 0x%02x, TID %u, %u payload bytes; "
                  "it waits for a controller to accept the IBI (none on the bus here)", word,
               (word >> 20) & 3, (word >> 12) & 0xff, (word >> 8) & 0xf, word & 0xff);
        } else {
          dbgf(2, "%s: 0x%08x (%zu DWORDs waiting)", name, word, q->data.size());
        }
      }
    }
    start_next();
    update();
  }
  void reg_after_read(uint16_t r) override {
    for (uint16_t w : watched_) {
      if (w == r) poll_watch(r, [this] { return outstanding(); });
    }
  }

 private:
  static constexpr unsigned kTargets = 4;
  // One queue or data buffer: a circular buffer of DWORDs in the message buffer.
  struct Ring {
    std::string name;
    uint16_t cfg, state;
    unsigned max_lsb;  // MAX_ADDR's position in the *_CONFIG register
    bool target_side;
    uint32_t min = 0, max = 0;
    uint64_t writes = 0, reads = 0;
    std::deque<uint32_t> data;
    unsigned size() const { return max >= min ? max - min + 1 : 0; }
    unsigned free() const { return size() > data.size() ? unsigned(size() - data.size()) : 0; }
    void configure(uint32_t c) {
      min = c & 0x3ff;
      max = (c >> max_lsb) & 0x3ff;
      clear();
    }
    void clear() {
      data.clear();
      writes = reads = 0;
    }
    uint32_t pop() {
      uint32_t w = data.front();
      data.pop_front();
      ++reads;
      return w;
    }
    // FULL, PRE, RPTR, WPTR.
    uint32_t state_value() const {
      unsigned n = size() ? size() : 1;
      uint32_t rptr = min + uint32_t(reads % n), wptr = min + uint32_t(writes % n);
      return (uint32_t(data.size() >= size()) << 31) | (uint32_t(!data.empty()) << 30) |
             ((rptr & 0x3ff) << 16) | (wptr & 0x3ff);
    }
  };
  size_t add_ring(const std::string &prefix, unsigned max_lsb, const std::string &name,
                  int index = -1) {
    std::string sfx = index >= 0 ? "_" + std::to_string(index) : "";
    Ring q;
    q.name = name;
    q.cfg = reg((prefix + "_CONFIG" + sfx).c_str());
    q.state = reg((prefix + "_STATE" + sfx).c_str());
    q.max_lsb = max_lsb;
    q.target_side = prefix.rfind("TARG_", 0) == 0;
    q.configure(value(q.cfg));
    rings_.push_back(q);
    return rings_.size() - 1;
  }
  Ring &cmd_q() { return rings_[cmd_]; }
  Ring &resp_q() { return rings_[resp_]; }
  Ring &ctx_q() { return rings_[ctx_]; }
  Ring &crx_q() { return rings_[crx_]; }
  Ring &ibi_q() { return rings_[ibi_]; }
  Ring &ibi_stat_q() { return rings_[ibi_stat_]; }
  Ring &trxbuf_q() { return rings_[trxbuf_]; }
  Ring &trxdesc_q() { return rings_[trxdesc_]; }
  Ring &tibi_q() { return rings_[tibi_]; }
  Ring &tibidesc_q() { return rings_[tibidesc_]; }
  Ring &tasync_q() { return rings_[tasync_]; }

  static const char *tti_port_name(unsigned port) {
    static const char *kTti[13] = {
        "TTI Rx Descriptor Queue",   "TTI Rx Data Buffer",        "TTI IBI Status Descriptor",
        "TTI IBI Data Buffer",       "TTI Async Event Queue",     "TTI Target 0 Tx Descriptor",
        "TTI Target 1 Tx Descriptor", "TTI Target 2 Tx Descriptor", "TTI Target 3 Tx Descriptor",
        "TTI Target 0 Tx Data",      "TTI Target 1 Tx Data",      "TTI Target 2 Tx Data",
        "TTI Target 3 Tx Data"};
    return port < 13 ? kTti[port] : "TTI port";
  }
  bool push(Ring &q, uint32_t word, bool hci, const char *port) {
    if (q.data.size() >= q.size()) {
      queue_error(hci, std::string("write of ") + port + " with the " + q.name + " full");
      return false;
    }
    q.data.push_back(word);
    ++q.writes;
    return true;
  }
  // "The hardware sets this bit in the event of a write to a full or read-only queue, or a read
  // from an empty or write-only queue" (BUFFER_STATUS.HCIQ_ERR, TTIQ_ERR).
  void queue_error(bool hci, const std::string &what) {
    set_value(buffer_status_, value(buffer_status_) | (1u << (hci ? 31 : 30)));
    if (dbg_count(hci ? "HCI queue errors" : "TTI queue errors", 8))
      dbgf(1, "%s: BUFFER_STATUS.%s set", what.c_str(), hci ? "HCIQ_ERR" : "TTIQ_ERR");
  }
  // A command descriptor in words (MIPI I3C HCI 1.2 layout).
  void debug_command(const char *what, uint32_t d0, uint32_t d1) {
    if (!dbg_on(1)) return;
    static const char *kAttr[8] = {"regular transfer", "immediate transfer", "address assignment",
                                   "combo transfer", "attribute 4", "attribute 5", "attribute 6",
                                   "internal control"};
    unsigned attr = d0 & 7, tid = (d0 >> 3) & 0xf;
    std::string s;
    char b[160];
    if (attr <= 1) {
      std::snprintf(b, sizeof b, ", %s DAT entry %u%s", ((d0 >> 29) & 1) ? "read from" : "write to",
                    (d0 >> 16) & 0x1f, ((d0 >> 31) & 1) ? ", STOP after it (TOC)" : "");
      s += b;
      if ((d0 >> 15) & 1) {
        std::snprintf(b, sizeof b, ", CCC 0x%02x", (d0 >> 7) & 0xff);
        s += b;
      }
      if (attr == 0) {
        std::snprintf(b, sizeof b, ", %u bytes", d1 >> 16);
        s += b;
      }
    } else if (attr == 2) {
      std::snprintf(b, sizeof b, ", CCC 0x%02x from DAT entry %u", (d0 >> 7) & 0xff,
                    (d0 >> 16) & 0x1f);
      s += b;
    }
    dbgf(1, "command %s: 0x%08x 0x%08x = TID %u, %s%s", what, d0, d1, tid, kAttr[attr], s.c_str());
  }
  bool controller_runs() const {
    return (value(hc_control_) >> 31) && (value(pio_control_) & 3) == 3;
  }
  // The controller takes the next command when it runs and has none in progress.
  void start_next() {
    if (running_ || !controller_runs() || cmd_q().data.size() < 2) return;
    run_d0_ = cmd_q().pop();
    run_d1_ = cmd_q().pop();
    running_ = true;
    char item[40];
    std::snprintf(item, sizeof item, "start %08x %08x", run_d0_, run_d1_);
    sys_.emit(inst(), item);
    debug_command("started on the bus", run_d0_, run_d1_);
    dbg(1, "no I3C target is on the bus in eg2sim: the command never completes (no response)");
  }
  void stop_running(const char *why) {
    if (running_) dbgf(1, "the command in progress (TID %u) dropped by %s", (run_d0_ >> 3) & 0xf, why);
    running_ = false;
  }
  // What the software waits for, for the stop message.
  std::string outstanding() const {
    char b[256];
    if (running_) {
      std::snprintf(b, sizeof b, "command TID %u (0x%08x 0x%08x) waits for an I3C target to "
                    "answer, and eg2sim has no device on the I3C bus", (run_d0_ >> 3) & 0xf,
                    run_d0_, run_d1_);
    } else if (rings_[cmd_].data.size() >= 2) {
      std::snprintf(b, sizeof b, "%zu commands wait in the command queue and the controller does "
                    "not run (HC_CONTROL.BUS_ENABLE %u, PIO_CONTROL.ENABLE %u, RS %u)",
                    rings_[cmd_].data.size() / 2, value(hc_control_) >> 31,
                    value(pio_control_) & 1, (value(pio_control_) >> 1) & 1);
    } else if (value(targ_control_) & 1) {
      std::snprintf(b, sizeof b, "the target waits for an I3C controller on the bus (a private "
                    "read or write, an IBI accepted), and eg2sim has none");
    } else {
      std::snprintf(b, sizeof b, "nothing in eg2sim changes it: the I3C bus has no other device");
    }
    return b;
  }
  static bool at_least_pow(unsigned n, unsigned thld_field) {
    return n >= (2u << thld_field);  // 2^(N+1)
  }
  void update() {
    // The controller's levels.
    unsigned cmd_free = cmd_q().free() / 2;
    set_value(queue_level_, (std::min<unsigned>(unsigned(resp_q().data.size()), 255) << 8) |
                                std::min(cmd_free, 255u));
    set_value(data_level_, (std::min<unsigned>(unsigned(crx_q().data.size()), 255) << 8) |
                               std::min(ctx_q().free(), 255u));
    set_value(present_, uint32_t((value(hc_control_) >> 31) & 1) << 2);
    // PIO_INTR_STATUS: the level bits while their status enable is set, the events as they are.
    uint32_t qt = value(queue_thld_), dt = value(data_thld_);
    uint32_t lv = 0;
    if (at_least_pow(ctx_q().free(), dt & 7)) lv |= 1u << 0;              // TX_THLD
    if (at_least_pow(unsigned(crx_q().data.size()), (dt >> 8) & 7)) lv |= 1u << 1;  // RX_THLD
    unsigned ibi_thld = (qt >> 24) & 0xff, resp_thld = (qt >> 8) & 0xff, cmd_thld = qt & 0xff;
    if (ibi_thld && ibi_stat_q().data.size() >= ibi_thld) lv |= 1u << 2;  // IBI_STATUS_THLD
    if (cmd_free >= cmd_thld) lv |= 1u << 3;                                // CMD_QUEUE_READY
    if (resp_thld && resp_q().data.size() >= resp_thld) lv |= 1u << 4;    // RESP_READY
    lv = (lv & value(pio_st_en_)) | pio_forced_;
    uint32_t pio = (value(pio_st_) & 0x220) | (lv & 0x1f);
    set_value(pio_st_, pio);
    // The target's.
    bool active = value(targ_control_) & 1;
    uint32_t ts = value(targ_status_) & ~(3u << 29);
    set_value(targ_status_, ts | (uint32_t(active) << 30) | (uint32_t(active) << 29));
    uint32_t tl = 0;
    for (unsigned t = 0; t < kTargets; ++t) {
      const Ring &dq = rings_[ttxdesc_[t]], &bq = rings_[ttxbuf_[t]];
      set_value(tx_level_[t], active ? ((std::min(dq.free(), 255u) << 16) | std::min(bq.free(), 4095u))
                                     : 0);
      uint32_t th = value(tx_thld_[t]);
      unsigned desc_thld = (th >> 16) & 0xff, buf_thld = th & 0xfff;
      if (active && desc_thld && dq.free() >= desc_thld) tl |= 1u << (16 + t);  // TXn_DESC_READY
      if (active && buf_thld && bq.free() >= buf_thld) tl |= 1u << (8 + t);     // TXn_THLD
    }
    uint32_t tqt = value(targ_q_thld_);
    unsigned rx_desc_thld = (tqt >> 8) & 0xff, tibi_thld = (tqt >> 24) & 0xff;
    if (active && rx_desc_thld && trxdesc_q().data.size() >= rx_desc_thld) tl |= 1u << 0;
    if (active && tibi_thld && tibidesc_q().free() >= tibi_thld) tl |= 1u << 1;
    if (active && !tasync_q().data.empty()) tl |= 1u << 2;
    set_value(targ_st_, (tl & value(targ_st_en_)) | targ_forced_);
    set_value(targ_q_level_, active ? ((std::min<unsigned>(unsigned(tasync_q().data.size()), 4095) << 16) |
                                       std::min<unsigned>(unsigned(trxdesc_q().data.size()), 255))
                                    : 0);
    set_value(targ_buf_level_, active ? ((std::min(tibi_q().free(), 4095u) << 16) |
                                         std::min<unsigned>(unsigned(trxbuf_q().data.size()), 4095))
                                      : 0);
    for (Ring &q : rings_) set_value(q.state, q.state_value());
    // INTR_STATE.hci and .targ.
    bool hci = (pio & value(pio_sig_en_)) || (value(hc_st_) & value(hc_sig_en_));
    set_intr_level("hci", hci);
    set_intr_level("targ", (value(targ_st_) & value(targ_sig_en_)) != 0);
  }

  std::vector<Ring> rings_;
  size_t cmd_, resp_, ctx_, crx_, ibi_, ibi_stat_, trxbuf_, trxdesc_, tibi_, tibidesc_, tasync_;
  size_t ttxbuf_[kTargets], ttxdesc_[kTargets];
  uint16_t tx_level_[kTargets], tx_thld_[kTargets];
  uint16_t hc_control_, pio_control_, reset_control_, present_, buffer_ctrl_, buffer_status_,
      queue_level_, data_level_, queue_thld_, data_thld_, pio_st_, pio_st_en_, pio_sig_en_,
      pio_force_, hc_st_, hc_sig_en_, hc_force_, targ_control_, targ_status_, targ_enable_,
      targ_st_, targ_st_en_, targ_sig_en_, targ_force_, targ_q_thld_, targ_q_level_,
      targ_buf_level_;
  int hci_win_ = -1, tti_win_ = -1, dat_win_ = -1;
  std::vector<uint16_t> watched_;
  bool running_ = false;
  uint32_t run_d0_ = 0, run_d1_ = 0;
  uint32_t pio_forced_ = 0, targ_forced_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_I3C_H_
