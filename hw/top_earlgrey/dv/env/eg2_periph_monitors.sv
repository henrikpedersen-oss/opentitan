// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 extended peripheral monitors (status/decisions.md 2026-10-24: each peripheral's agent
// monitor takes the TL-UL traffic of that peripheral's own device port and turns it into the
// peripheral's sequence items). Compiled with EG2_PERIPH_SB only; eg2_periph_env puts them in
// place by factory override when +eg2_periph_sb=1. OpenTitan's agents are unchanged: each class
// here extends one, keeps its pin side as it is (the actual items), and adds:
//
//   tl_fifo      the TL-UL items of the peripheral's port (a passive tl_monitor on the tap of
//                tb/eg2_periph_tl.svh), each with its response
//   tl_exp_port  what the register writes tell the peripheral to send, as the peripheral's own
//                items (expected; flushes and the like as eg2_periph_marker items in the stream)
//   rx_exp_port  what the bench drove into the chip, as the chip's receive FIFO takes it: the
//                expected values of the core's later reads of the receive register
//   rx_rd_port   those reads (TL-UL read responses), as items (actual)
//
// What each conversion follows is in the documentation of the block (hw/ip/<block>/doc and
// data/<block>.hjson), not in its RTL; what it does not follow is listed at each class.

// The register offset of a TL item in a window of `size` bytes (a power of two).
function automatic bit [31:0] eg2_tl_offset(tl_seq_item t, bit [31:0] size);
  return t.a_addr & (size - 1);
endfunction

// A register written with byte enables: the new value of `old`.
function automatic bit [31:0] eg2_tl_merge(bit [31:0] old, tl_seq_item t);
  bit [31:0] m;
  for (int b = 0; b < 4; b++) if (t.a_mask[b]) m[8 * b +: 8] = 8'hff;
  return (old & ~m) | (t.a_data & m);
endfunction

//////////////////////////////////////////////////////////////////////////////////////////////////
// UART (hw/ip/uart: data/uart.hjson, doc/theory_of_operation.md, doc/programmers_guide.md)
//
// TX: a byte written to WDATA is sent once CTRL.TX is set ("written data into the FIFO will be
// stacked up and sent out when TX is enabled"); with CTRL.SLPBK it goes into the receiver instead
// (while CTRL.RX is set) and nothing leaves on the pin; with CTRL.LLPBK each byte received on the
// pin is also sent. FIFO_CTRL.TXRST drops the bytes not sent yet (the one on the wire finishes).
// RX: a byte the bench drove is taken while CTRL.RX is set and CTRL.SLPBK clear, unless the
// 64-byte RX FIFO is full (rx_overflow: the byte is lost); a read of RDATA returns the oldest. A
// read while the FIFO is empty is not compared. FIFO_CTRL.RXRST empties the FIFO. A byte the bench
// drove with a parity or framing error is announced (eg2_inject_registry: logged and covered, not
// to be detected) and optional: the core may not read it. Not followed: the TX FIFO overflowing (software waits for room), the
// break and timeout conditions, the noise filter, OVRD/VAL.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_uart_monitor extends uart_monitor;
  `uvm_component_utils(eg2_uart_monitor)

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_analysis_port #(uart_item)       tl_exp_port;
  uvm_analysis_port #(uart_item)       rx_exp_port;
  uvm_analysis_port #(uart_item)       rx_rd_port;
  protected uvm_tlm_analysis_fifo #(uart_item) pin_rx_fifo;

  virtual tl_if tl_vif;          // the tap; null: no conversion (the pin side only)
  string        inst = "uart";

  protected bit [31:0]   ctrl;
  protected uart_item    tx_hold[$];
  protected int unsigned rx_level;
  int unsigned           rx_empty_reads, rx_overflows, rx_bad_frames;
  // Functional coverage (eg2_periph_cov.sv), per instance; created by eg2_periph_env.
  eg2_uart_cfg_cg        cfg_cg;
  eg2_uart_fifo_cg       fifo_cg;

  function void create_cov(string path);
    cfg_cg  = new({path, "::eg2_uart_cfg_cg"});
    fifo_cg = new({path, "::eg2_uart_fifo_cg"});
  endfunction

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo     = new("tl_fifo", this);
    pin_rx_fifo = new("pin_rx_fifo", this);
    tl_exp_port = new("tl_exp_port", this);
    rx_exp_port = new("rx_exp_port", this);
    rx_rd_port  = new("rx_rd_port", this);
  endfunction

  function void connect_phase(uvm_phase phase);
    super.connect_phase(phase);
    rx_analysis_port.connect(pin_rx_fifo.analysis_export);
  endfunction

  task run_phase(uvm_phase phase);
    fork
      super.run_phase(phase);
      if (tl_vif != null) begin
        fork
          tl_thread();
          pin_rx_thread();
          reset_thread();
          keep_rx_monitor_on();
        join
      end
    join
  endtask

  // The RX direction needs the bench's bytes: whenever the agent watches TX, it watches RX too.
  protected task keep_rx_monitor_on();
    forever begin
      if (cfg.en_tx_monitor && !cfg.en_rx_monitor) cfg.en_rx_monitor = 1'b1;
      @(cfg.en_tx_monitor or cfg.en_rx_monitor);
    end
  endtask

  protected function uart_item byte_item(bit [7:0] data);
    uart_item it = uart_item::type_id::create("it");
    it.data = data;
    it.parity = ^data ^ ctrl[7];
    return it;
  endfunction

  protected function void send_flush(uvm_analysis_port #(uart_item) port, int keep, string why);
    uart_item m = uart_item::type_id::create("flush");
    eg2_periph_marker::mark(m, eg2_periph_marker::MarkFlush, keep, why);
    port.write(m);
  endfunction

  // A byte the transmitter takes: out on the pin, or into the receiver in system loopback.
  protected function void transmit(uart_item it);
    if (!ctrl[4]) begin
      tl_exp_port.write(it);
    end else if (ctrl[1] && rx_level < uart_reg_pkg::RxFifoDepth) begin
      rx_level++;
      rx_exp_port.write(it);
    end else if (ctrl[1]) begin
      rx_overflows++;
      if (fifo_cg != null) fifo_cg.sample(1, rx_level);
    end
  endfunction

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      // An access the port refused (d_error) or the documents refuse (eg2_tl_err) changes nothing;
      // its classification is the scoreboard's (tl_fifo).
      if (t.d_error || eg2_tl_err::refused(inst, t)) continue;
      off = eg2_tl_offset(t, 32'h40);
      if (t.is_write()) begin
        case (off)
          uart_reg_pkg::UART_CTRL_OFFSET: begin
            bit was_on = ctrl[0];
            ctrl = eg2_tl_merge(ctrl, t);
            if (cfg_cg != null && (ctrl[0] || ctrl[1])) begin
              cfg_cg.sample(ctrl[0], ctrl[1], ctrl[31:16], ctrl[6], ctrl[7], ctrl[4], ctrl[5]);
            end
            if (ctrl[0] && !was_on) begin
              while (tx_hold.size() > 0) transmit(tx_hold.pop_front());
            end
          end
          uart_reg_pkg::UART_WDATA_OFFSET: begin
            if (t.a_mask[0]) begin
              uart_item it = byte_item(t.a_data[7:0]);
              if (ctrl[0]) transmit(it);
              else tx_hold.push_back(it);
            end
          end
          uart_reg_pkg::UART_FIFO_CTRL_OFFSET: begin
            if (t.a_mask[0] && t.a_data[0]) begin
              if (fifo_cg != null) fifo_cg.sample(2, rx_level);
              rx_level = 0;
              send_flush(rx_exp_port, 0, "FIFO_CTRL.RXRST");
            end
            if (t.a_mask[0] && t.a_data[1]) begin
              if (fifo_cg != null) fifo_cg.sample(3, rx_level);
              tx_hold.delete();
              send_flush(tl_exp_port, 1, "FIFO_CTRL.TXRST");
            end
          end
          default: ;
        endcase
      end else if (off == uart_reg_pkg::UART_RDATA_OFFSET) begin
        if (rx_level > 0) begin
          uart_item it = byte_item(t.d_data[7:0]);
          rx_level--;
          rx_rd_port.write(it);
        end else begin
          rx_empty_reads++;
        end
      end
    end
  endtask

  protected task pin_rx_thread();
    forever begin
      uart_item it;
      bit       bad;
      pin_rx_fifo.get(it);
      bad = it.stop_bit !== 1'b1 || (ctrl[6] && it.parity !== (^it.data ^ ctrl[7]));
      if (bad) begin
        rx_bad_frames++;
        void'(eg2_inject_registry::announce({inst, " agent"}, inst,
                                            it.stop_bit !== 1'b1 ? "RX byte with a framing error" :
                                            "RX byte with a parity error", $realtime, $realtime, 1'b0));
      end
      if (ctrl[5]) tl_exp_port.write(byte_item(it.data));  // LLPBK: forwarded to TX
      if (!ctrl[1] || ctrl[4]) continue;                     // RX off, or loopback from TX
      if (fifo_cg != null) fifo_cg.sample(rx_level >= uart_reg_pkg::RxFifoDepth ? 1 : 0, rx_level);
      if (rx_level >= uart_reg_pkg::RxFifoDepth) begin
        rx_overflows++;                                     // lost: rx_overflow
        continue;
      end
      rx_level++;
      begin
        uart_item e = byte_item(it.data);
        if (bad) eg2_periph_marker::mark(e, eg2_periph_marker::MarkOptional, 0, "bad frame");
        rx_exp_port.write(e);
      end
    end
  endtask

  protected task reset_thread();
    forever begin
      @(negedge tl_vif.rst_n);
      ctrl = '0;
      tx_hold.delete();
      rx_level = 0;
      send_flush(tl_exp_port, 0, "uart reset");
      send_flush(rx_exp_port, 0, "uart reset");
    end
  endtask
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// SPI host (hw/ip/spi_host: data/spi_host.hjson, doc/theory_of_operation.md,
// doc/programmers_guide.md). For spi_host<N>, whose pins the agent watches in device mode.
//
// A COMMAND queues a segment of LEN + 1 bytes (standard speed) with DIRECTION 1 RX, 2 TX,
// 3 bidirectional; segments run in order while CONTROL.SPIEN is set. A TX or bidirectional
// segment sends the next bytes of TXDATA (written by byte enables, the first byte on the bus the
// least significant: ByteOrder 1); an RX segment drives nothing the documentation defines, so its
// bytes are expected as X (not compared). The bytes the bench drove on SD1 during RX and
// bidirectional segments are the expected values of the RXDATA words, a segment's last word
// holding its remaining bytes. CONTROL.SW_RST drops what was not sent. Not followed: dual and quad
// segments and dummy segments (the stream is then NOT PREDICTED from there on, said in the log),
// the CSID and CSAAT boundaries (the bytes are compared as one stream), command and FIFO errors.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_spi_monitor extends spi_monitor;
  `uvm_component_utils(eg2_spi_monitor)

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_analysis_port #(spi_item)        tl_exp_port;
  uvm_analysis_port #(spi_item)        rx_exp_port;
  uvm_analysis_port #(spi_item)        rx_rd_port;
  protected uvm_tlm_analysis_fifo #(spi_item) pin_dev_fifo;

  virtual tl_if tl_vif;          // the tap of a spi_host; null for spi_device (pin side only)
  string        inst = "spi";

  typedef struct {
    int unsigned n;      // bytes
    int unsigned left;   // bytes still to be emitted (TX)
    bit [1:0]    dir;
    bit [1:0]    speed;
  } seg_t;
  typedef struct {
    int unsigned n;
    bit          rx;
  } bus_seg_t;

  protected bit [31:0]   control;
  protected bit [31:0]   configopts;
  protected bit [7:0]    tx_bytes[$];
  eg2_spi_seg_cg         seg_cg;

  function void create_cov(string path);
    seg_cg = new({path, "::eg2_spi_seg_cg"});
  endfunction
  protected seg_t        segs[$];
  protected bus_seg_t    bus_segs[$];
  protected int unsigned rx_words[$];  // valid bytes of each RXDATA word to come
  protected bit          lost;         // a segment it cannot predict was seen

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo      = new("tl_fifo", this);
    pin_dev_fifo = new("pin_dev_fifo", this);
    tl_exp_port  = new("tl_exp_port", this);
    rx_exp_port  = new("rx_exp_port", this);
    rx_rd_port   = new("rx_rd_port", this);
  endfunction

  function void connect_phase(uvm_phase phase);
    super.connect_phase(phase);
    device_analysis_port.connect(pin_dev_fifo.analysis_export);
  endfunction

  task run_phase(uvm_phase phase);
    fork
      super.run_phase(phase);
      if (tl_vif != null) begin
        fork
          tl_thread();
          pin_dev_thread();
          reset_thread();
        join
      end else begin
        // spi_device's agent: no conversion; its SD1 bytes are not kept here.
        forever begin
          spi_item it;
          pin_dev_fifo.get(it);
        end
      end
    join
  endtask

  protected function spi_item bytes_item(logic [7:0] b[$]);
    spi_item it = spi_item::type_id::create("it");
    it.data = b;
    return it;
  endfunction

  protected function void marker(uvm_analysis_port #(spi_item) port,
                                 eg2_periph_marker::kind_e kind, int keep, string why);
    spi_item m = spi_item::type_id::create("marker");
    eg2_periph_marker::mark(m, kind, keep, why);
    port.write(m);
  endfunction

  // Emits the expected bytes of the segments as far as their TX bytes are there.
  protected function void pump();
    while (segs.size() > 0 && control[31] && !lost) begin
      logic [7:0] b[$];
      if (segs[0].speed != 0 || segs[0].dir == 0) begin
        lost = 1'b1;
        marker(tl_exp_port, eg2_periph_marker::MarkUnpredicted, 0,
               segs[0].dir == 0 ? "a dummy segment" : "a dual or quad segment");
        marker(rx_exp_port, eg2_periph_marker::MarkUnpredicted, 0,
               segs[0].dir == 0 ? "a dummy segment" : "a dual or quad segment");
        return;
      end
      if (segs[0].dir == 1) begin
        repeat (segs[0].left) b.push_back(8'hxx);
        segs[0].left = 0;
      end else begin
        while (segs[0].left > 0 && tx_bytes.size() > 0) begin
          b.push_back(tx_bytes.pop_front());
          segs[0].left--;
        end
      end
      if (b.size() > 0) tl_exp_port.write(bytes_item(b));
      if (segs[0].left > 0) return;  // waits for TXDATA (the IP stalls too)
      void'(segs.pop_front());
    end
  endfunction

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      if (t.d_error || eg2_tl_err::refused(inst, t)) continue;  // refused: no state change
      off = eg2_tl_offset(t, 32'h40);
      if (t.is_write()) begin
        case (off)
          spi_host_reg_pkg::SPI_HOST_CONTROL_OFFSET: begin
            control = eg2_tl_merge(control, t);
            if (control[30]) begin
              tx_bytes.delete();
              segs.delete();
              bus_segs.delete();
              rx_words.delete();
              marker(tl_exp_port, eg2_periph_marker::MarkFlush, 4, "CONTROL.SW_RST");
              marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, "CONTROL.SW_RST");
            end
            pump();
          end
          spi_host_reg_pkg::SPI_HOST_CONFIGOPTS_OFFSET: configopts = eg2_tl_merge(configopts, t);
          spi_host_reg_pkg::SPI_HOST_TXDATA_OFFSET: begin
            for (int i = 0; i < 4; i++) if (t.a_mask[i]) tx_bytes.push_back(t.a_data[8 * i +: 8]);
            pump();
          end
          spi_host_reg_pkg::SPI_HOST_COMMAND_OFFSET: begin
            seg_t s;
            s.n     = t.a_data[24:5] + 1;
            s.left  = s.n;
            s.dir   = t.a_data[4:3];
            s.speed = t.a_data[2:1];
            segs.push_back(s);
            if (seg_cg != null) begin
              seg_cg.sample(configopts[31], configopts[30], s.speed, t.a_data[0], s.dir, s.n);
            end
            if (s.dir != 0 && s.speed == 0) begin
              bus_seg_t bs;
              bs.n  = s.n;
              bs.rx = s.dir inside {2'd1, 2'd3};
              bus_segs.push_back(bs);
              if (bs.rx) begin
                for (int unsigned w = 0; w < (s.n + 3) / 4; w++) begin
                  rx_words.push_back((w + 1) * 4 <= s.n ? 4 : s.n % 4);
                end
              end
            end
            pump();
          end
          default: ;
        endcase
      end else if (off == spi_host_reg_pkg::SPI_HOST_RXDATA_OFFSET && rx_words.size() > 0 &&
                   !lost) begin
        logic [7:0] b[$];
        int unsigned v = rx_words.pop_front();
        for (int i = 0; i < v; i++) b.push_back(t.d_data[8 * i +: 8]);
        rx_rd_port.write(bytes_item(b));
      end
    end
  endtask

  // The bench's bytes on SD1, kept where the segment receives them.
  protected task pin_dev_thread();
    forever begin
      spi_item it;
      pin_dev_fifo.get(it);
      foreach (it.data[i]) begin
        logic [7:0] b[$];
        if (bus_segs.size() == 0 || lost) continue;
        if (bus_segs[0].rx) begin
          b.push_back(it.data[i]);
          rx_exp_port.write(bytes_item(b));
        end
        bus_segs[0].n--;
        if (bus_segs[0].n == 0) void'(bus_segs.pop_front());
      end
    end
  endtask

  protected task reset_thread();
    forever begin
      @(negedge tl_vif.rst_n);
      control = '0;
      tx_bytes.delete();
      segs.delete();
      bus_segs.delete();
      rx_words.delete();
      marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, "spi_host reset");
      marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, "spi_host reset");
    end
  endtask
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// I2C (hw/ip/i2c: data/i2c.hjson, doc/theory_of_operation.md, doc/programmers_guide.md)
//
// Host mode (the agent is the target): FDATA entries run while CTRL.ENABLEHOST is set. An entry
// with START begins a transfer to FBYTE[7:1], reading if FBYTE[0]; an entry with READB reads FBYTE
// bytes (0: 256); any other entry writes FBYTE; STOP ends the transfer, a START without a STOP
// before it ends the previous one (a repeated START). Each finished transfer is one expected
// i2c_item (address, direction, the bytes written or the number read). The bytes the bench
// returned on reads are the expected values of RDATA. FIFO_CTRL.FMTRST drops the entries not run.
// Target mode (the agent is the controller): the bytes written to TXDATA are what the chip
// returns on the agent's reads (expected, as one byte stream); what the agent wrote and the
// address bytes of its transfers are the expected ACQDATA entries (START or repeated START with
// the address byte, the data bytes, STOP). Not followed: NAKOK and the controller halt after a
// NACK (the scoreboard makes the pending items optional after a NACK), 10-bit addresses, RCONT,
// clock stretching, timeouts, TARGET_ACK_CTRL.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_i2c_monitor extends i2c_monitor;
  `uvm_component_utils(eg2_i2c_monitor)

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_analysis_port #(i2c_item)        tl_exp_port;
  uvm_analysis_port #(i2c_item)        rx_exp_port;
  uvm_analysis_port #(i2c_item)        rx_rd_port;
  // The pin items, both directions in bus order, for the scoreboard (actual) and for the RX
  // expectations.
  uvm_analysis_port #(i2c_item)        pin_port;
  protected uvm_tlm_analysis_fifo #(i2c_item) pin_fifo;

  virtual tl_if tl_vif;
  string        inst = "i2c";

  protected bit [31:0] ctrl;
  protected bit [31:0] fmt_hold[$];
  protected i2c_item   cur;
  // A transfer is open on the bus (a START seen, no STOP yet); one the block's reset cut is dropped
  // from the pin side (drop_cut), as the expected side drops it (chip_sw_eg2_pbf_periph_fault,
  // 2026-10-29: "fault ip_reset after 1 byte", then the cut transfer "S 52 w 09" was compared with
  // the recovery transfer).
  protected bit        bus_open, drop_cut;
  eg2_i2c_fifo_cg      fifo_cg;

  function void create_cov(string path);
    fifo_cg = new({path, "::eg2_i2c_fifo_cg"});
  endfunction

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo     = new("tl_fifo", this);
    pin_fifo    = new("pin_fifo", this);
    tl_exp_port = new("tl_exp_port", this);
    rx_exp_port = new("rx_exp_port", this);
    rx_rd_port  = new("rx_rd_port", this);
    pin_port    = new("pin_port", this);
  endfunction

  function void connect_phase(uvm_phase phase);
    super.connect_phase(phase);
    // Both ports are written from one thread per mode, so one FIFO keeps the bus order.
    target_mode_wr_item_port.connect(pin_fifo.analysis_export);
    target_mode_rd_item_port.connect(pin_fifo.analysis_export);
    controller_mode_wr_item_port.connect(pin_fifo.analysis_export);
    controller_mode_rd_item_port.connect(pin_fifo.analysis_export);
  endfunction

  task run_phase(uvm_phase phase);
    fork
      super.run_phase(phase);
      begin
        fork
          pin_thread();
          if (tl_vif != null) begin
            fork
              tl_thread();
              reset_thread();
              bus_thread();
            join
          end
        join
      end
    join
  endtask

  static function i2c_item byte_item(bit [7:0] b, bit [2:0] signal = 0);
    i2c_item it = i2c_item::type_id::create("it");
    it.data_q.push_back(b);
    it.num_data = 1;
    it.fbyte = b;
    it.signal = i2c_pkg::i2c_acq_byte_id_e'(signal);
    it.tran_id = -1;  // a byte, not a transfer (eg2_i2c_sb)
    return it;
  endfunction

  protected function void marker(uvm_analysis_port #(i2c_item) port,
                                 eg2_periph_marker::kind_e kind, int keep, string why);
    i2c_item m = i2c_item::type_id::create("marker");
    eg2_periph_marker::mark(m, kind, keep, why);
    port.write(m);
  endfunction

  protected function void finish();
    if (cur == null) return;
    cur.num_data = cur.data_q.size();
    tl_exp_port.write(cur);
    cur = null;
  endfunction

  protected function void run_entry(bit [31:0] e);
    bit [7:0] fbyte = e[7:0];
    bit       start = e[8], stop = e[9], readb = e[10];
    if (start) begin
      finish();
      cur = i2c_item::type_id::create("exp");
      cur.addr   = fbyte[7:1];
      cur.bus_op = fbyte[0] ? BusOpRead : BusOpWrite;
      cur.dir    = fbyte[0] ? i2c_pkg::READ : i2c_pkg::WRITE;
    end else if (cur != null) begin
      if (readb) repeat (fbyte == 0 ? 256 : fbyte) cur.data_q.push_back(8'h00);
      else cur.data_q.push_back(fbyte);
    end
    if (stop) finish();
  endfunction

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      if (t.d_error || eg2_tl_err::refused(inst, t)) continue;  // refused: no state change
      off = eg2_tl_offset(t, 32'h80);
      if (t.is_write()) begin
        case (off)
          i2c_reg_pkg::I2C_CTRL_OFFSET: begin
            ctrl = eg2_tl_merge(ctrl, t);
            if (ctrl[0]) while (fmt_hold.size() > 0) run_entry(fmt_hold.pop_front());
          end
          i2c_reg_pkg::I2C_FDATA_OFFSET: begin
            if (ctrl[0]) run_entry(t.a_data);
            else fmt_hold.push_back(t.a_data);
          end
          i2c_reg_pkg::I2C_TXDATA_OFFSET: tl_exp_port.write(byte_item(t.a_data[7:0]));
          i2c_reg_pkg::I2C_FIFO_CTRL_OFFSET: begin
            bit [31:0] d = t.a_data;
            if (fifo_cg != null && (d[0] || d[1] || d[7] || d[8])) fifo_cg.sample(d[8:0]);
            if (d[1]) begin  // FMTRST
              fmt_hold.delete();
              cur = null;
              marker(tl_exp_port, eg2_periph_marker::MarkFlush, 1, "FIFO_CTRL.FMTRST");
            end
            if (d[0] || d[7]) marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0,
                                      d[0] ? "FIFO_CTRL.RXRST" : "FIFO_CTRL.ACQRST");
            if (d[8]) marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, "FIFO_CTRL.TXRST");
          end
          default: ;
        endcase
      end else begin
        case (off)
          i2c_reg_pkg::I2C_RDATA_OFFSET: rx_rd_port.write(byte_item(t.d_data[7:0]));
          i2c_reg_pkg::I2C_ACQDATA_OFFSET: rx_rd_port.write(byte_item(t.d_data[7:0],
                                                                      t.d_data[10:8]));
          default: ;
        endcase
      end
    end
  endtask

  // The pin items: to the scoreboard, and the bytes the bench supplied as RX expectations.
  protected task pin_thread();
    forever begin
      i2c_item it;
      pin_fifo.get(it);
      if (drop_cut) begin
        drop_cut = 1'b0;
        `uvm_info(`gfn, $sformatf("EG2 %s: the transfer the block's reset cut is not compared: %s",
                                  inst, it.sprint(uvm_default_line_printer)), UVM_LOW)
        continue;
      end
      // A NACK on the bus is stimulus the bench may give on purpose: announced (logged and covered;
      // the scoreboard makes what was queued after it optional), need not mismatch.
      if (it.addr_ack == i2c_pkg::NACK || (it.bus_op == BusOpWrite && i2c_pkg::NACK inside {it.data_ack_q})) begin
        void'(eg2_inject_registry::announce({inst, " bus"}, inst, "a NACK on the bus", $realtime, $realtime,
                                            1'b0));
      end
      pin_port.write(it);
      if (tl_vif == null) continue;
      if (cfg.if_mode == Device) begin
        // The chip is the controller: on its reads, the bench's bytes go to RDATA.
        if (it.bus_op == BusOpRead) foreach (it.data_q[i]) rx_exp_port.write(byte_item(it.data_q[i]));
      end else begin
        // The chip is the target: the ACQ FIFO gets the address byte and the written bytes.
        rx_exp_port.write(byte_item({it.addr[6:0], it.bus_op == BusOpRead},
                                    it.rstart_front ? 3'b011 : 3'b001));
        if (it.bus_op == BusOpWrite) foreach (it.data_q[i]) rx_exp_port.write(byte_item(it.data_q[i]));
        if (it.stop) rx_exp_port.write(byte_item(8'h00, 3'b010));
      end
    end
  endtask

  // START: SDA falls while SCL is high; STOP: SDA rises while SCL is high.
  protected task bus_thread();
    forever begin
      @(cfg.vif.sda_i);
      if (cfg.vif.scl_i === 1'b1 && !$isunknown(cfg.vif.sda_i)) bus_open = (cfg.vif.sda_i == 1'b0);
    end
  endtask

  protected task reset_thread();
    forever begin
      @(negedge tl_vif.rst_n);
      // The open transfer ends without its STOP: its pin item comes later and is not compared.
      if (bus_open) drop_cut = 1'b1;
      ctrl = '0;
      fmt_hold.delete();
      cur = null;
      marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, "i2c reset");
      marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, "i2c reset");
    end
  endtask
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// GPIO (hw/top_earlgrey/ip_autogen/gpio: data/gpio.hjson, doc/programmers_guide.md) on the pads
// (pinmux: hw/top_earlgrey/ip_autogen/pinmux/data/pinmux.hjson).
//
// A new monitor (OpenTitan has no GPIO agent): its items are the GPIOs as the pads carry them.
// A GPIO reaches a pad when that pad's MIO_OUTSEL selects it (MioOutGpioGpio0 + 3 + i); the pads'
// output enables and levels are those pinmux drives into the pad ring (eg2_pads_if). Actual: a
// new item whenever what the routed GPIOs drive changes. Expected, from the TL-UL writes of gpio
// (DIRECT_OUT, MASKED_OUT_LOWER/UPPER, DIRECT_OE, MASKED_OE_LOWER/UPPER, PER_PIN_IO_i,
// PER_PIN_OE_i) and of pinmux (MIO_OUTSEL_i): a new item whenever that changes. Not followed: the
// inputs (DATA_IN is read through the input filter and synchronisers: timing the model does not
// have), the straps, pad attributes (inversion, virtual open drain), sleep-mode pad values.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_gpio_monitor extends uvm_monitor;
  `uvm_component_utils(eg2_gpio_monitor)

  localparam int unsigned NPads = pinmux_reg_pkg::NMioPads;
  localparam int unsigned OutselGpio0 = 3 + int'(top_earlgrey_pkg::MioOutGpioGpio0);

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;      // gpio's port
  uvm_tlm_analysis_fifo #(tl_seq_item) pinmux_fifo;  // pinmux's port
  uvm_analysis_port #(eg2_gpio_item)   pin_port;     // actual
  uvm_analysis_port #(eg2_gpio_item)   tl_exp_port;  // expected

  virtual eg2_pads_if pads_vif;
  virtual tl_if       tl_vif;
  virtual tl_if       pinmux_vif;

  protected bit [31:0] out, oe;
  protected bit [7:0]  outsel[NPads];
  protected int        pad_of[32];   // the pad a GPIO drives, -1 if none
  protected bit [31:0] routed;
  protected bit [31:0] last_exp_oe, last_exp_out, last_act_oe, last_act_out, last_act_known;
  protected bit        exp_started, act_started;
  eg2_gpio_cg          gpio_cg;
  eg2_pinmux_cg        pinmux_cg;
  // The pads of the bench's GPIO pad set (chip_if gpios_if, pinmux_testutils' GPIO pads), in order.
  localparam top_earlgrey_pkg::mio_pad_e GpioPads[32] = '{
      top_earlgrey_pkg::MioPadIoa0, top_earlgrey_pkg::MioPadIoa1, top_earlgrey_pkg::MioPadIoa2,
      top_earlgrey_pkg::MioPadIoa3, top_earlgrey_pkg::MioPadIoa4, top_earlgrey_pkg::MioPadIoa5,
      top_earlgrey_pkg::MioPadIoa6, top_earlgrey_pkg::MioPadIoa7, top_earlgrey_pkg::MioPadIoa8,
      top_earlgrey_pkg::MioPadIob6, top_earlgrey_pkg::MioPadIob7, top_earlgrey_pkg::MioPadIob8,
      top_earlgrey_pkg::MioPadIob9, top_earlgrey_pkg::MioPadIob10, top_earlgrey_pkg::MioPadIob11,
      top_earlgrey_pkg::MioPadIob12, top_earlgrey_pkg::MioPadIoc9, top_earlgrey_pkg::MioPadIoc10,
      top_earlgrey_pkg::MioPadIoc11, top_earlgrey_pkg::MioPadIoc12, top_earlgrey_pkg::MioPadIor0,
      top_earlgrey_pkg::MioPadIor1, top_earlgrey_pkg::MioPadIor2, top_earlgrey_pkg::MioPadIor3,
      top_earlgrey_pkg::MioPadIor4, top_earlgrey_pkg::MioPadIor5, top_earlgrey_pkg::MioPadIor6,
      top_earlgrey_pkg::MioPadIor7, top_earlgrey_pkg::MioPadIor10, top_earlgrey_pkg::MioPadIor11,
      top_earlgrey_pkg::MioPadIor12, top_earlgrey_pkg::MioPadIor13};

  function void create_cov(string path);
    gpio_cg   = new({path, "::eg2_gpio_cg"});
    pinmux_cg = new({path, "::eg2_pinmux_cg"});
  endfunction

  protected function void sample_gpio(bit [31:0] off);
    int kind = -1;
    case (off)
      gpio_reg_pkg::GPIO_DIRECT_OUT_OFFSET:           kind = 0;
      gpio_reg_pkg::GPIO_MASKED_OUT_LOWER_OFFSET:     kind = 1;
      gpio_reg_pkg::GPIO_MASKED_OUT_UPPER_OFFSET:     kind = 2;
      gpio_reg_pkg::GPIO_DIRECT_OE_OFFSET:            kind = 3;
      gpio_reg_pkg::GPIO_MASKED_OE_LOWER_OFFSET:      kind = 4;
      gpio_reg_pkg::GPIO_MASKED_OE_UPPER_OFFSET:      kind = 5;
      gpio_reg_pkg::GPIO_INTR_CTRL_EN_RISING_OFFSET:  kind = 8;
      gpio_reg_pkg::GPIO_INTR_CTRL_EN_FALLING_OFFSET: kind = 9;
      gpio_reg_pkg::GPIO_INTR_CTRL_EN_LVLHIGH_OFFSET: kind = 10;
      gpio_reg_pkg::GPIO_INTR_CTRL_EN_LVLLOW_OFFSET:  kind = 11;
      gpio_reg_pkg::GPIO_CTRL_EN_INPUT_FILTER_OFFSET: kind = 12;
      default: begin
        if (off >= 32'h100 && off < 32'h180) kind = 6;
        else if (off >= 32'h200 && off < 32'h300 && off[2:0] == 3'b000) kind = 7;
      end
    endcase
    if (gpio_cg != null && kind >= 0) gpio_cg.sample(kind, oe);
  endfunction

  protected function void sample_pinmux(int pad, bit [7:0] sel);
    if (pinmux_cg == null) return;
    foreach (GpioPads[b]) begin
      if (int'(GpioPads[b]) != pad) continue;
      pinmux_cg.sample(b, sel < 2 ? 0 : sel == 2 ? 1 :
                       (int'(sel) - OutselGpio0 >= 0 && int'(sel) - OutselGpio0 < 32) ? 2 : 3);
    end
  endfunction

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo     = new("tl_fifo", this);
    pinmux_fifo = new("pinmux_fifo", this);
    pin_port    = new("pin_port", this);
    tl_exp_port = new("tl_exp_port", this);
    pinmux_reset();
  endfunction

  protected function void pinmux_reset();
    // MIO_OUTSEL resets to 2 (high-Z) on every pad.
    foreach (outsel[p]) outsel[p] = 8'd2;
    remap();
  endfunction

  protected function void remap();
    routed = '0;
    foreach (pad_of[g]) pad_of[g] = -1;
    for (int p = 0; p < NPads; p++) begin
      int g = int'(outsel[p]) - OutselGpio0;
      if (g >= 0 && g < 32 && pad_of[g] < 0) begin
        pad_of[g] = p;
        routed[g] = 1'b1;
      end
    end
  endfunction

  virtual task run_phase(uvm_phase phase);
    if (pads_vif == null || tl_vif == null || pinmux_vif == null) return;
    fork
      gpio_tl_thread();
      pinmux_tl_thread();
      pads_thread();
      forever begin
        @(negedge tl_vif.rst_n);
        out = '0;
        oe  = '0;
        emit_exp();
      end
      forever begin
        @(negedge pinmux_vif.rst_n);
        pinmux_reset();
        emit_exp();
        sample_pads();
      end
    join
  endtask

  protected function void emit_exp();
    bit [31:0] e_oe = oe & routed, e_out = out & oe & routed;
    if (exp_started && e_oe == last_exp_oe && e_out == last_exp_out) return;
    exp_started = 1'b1;
    last_exp_oe = e_oe;
    last_exp_out = e_out;
    begin
      eg2_gpio_item it = eg2_gpio_item::type_id::create("exp");
      it.routed = routed;
      it.oe = e_oe;
      it.out = e_out;
      it.known = '1;
      tl_exp_port.write(it);
    end
  endfunction

  protected function void sample_pads();
    // A GPIO no pad carries is not observed: it counts as known, so that routing it (a MIO_OUTSEL
    // write that changes nothing on the pads) is no new item. Left 0, every routing write emitted an
    // item the expected side has no partner for, and the pairing ran one item ahead
    // (chip_sw_gpio_smoketest_eg2sb, 2026-10-29: "routed 00000001" against "routed ffffffff").
    bit [31:0] a_oe = '0, a_out = '0, a_known = '1;
    for (int g = 0; g < 32; g++) begin
      logic o, v;
      if (pad_of[g] < 0) continue;
      o = pads_vif.mio_oe[pad_of[g]];
      v = pads_vif.mio_out[pad_of[g]];
      a_oe[g] = o === 1'b1;
      a_out[g] = a_oe[g] && v === 1'b1;
      a_known[g] = !a_oe[g] || !$isunknown(v);
    end
    if (act_started && a_oe == last_act_oe && a_out == last_act_out && a_known == last_act_known)
      return;
    act_started = 1'b1;
    last_act_oe = a_oe;
    last_act_out = a_out;
    last_act_known = a_known;
    begin
      eg2_gpio_item it = eg2_gpio_item::type_id::create("act");
      it.routed = routed;
      it.oe = a_oe;
      it.out = a_out;
      it.known = a_known;
      pin_port.write(it);
    end
  endfunction

  protected task pads_thread();
    wait (tl_vif.rst_n === 1'b1);
    forever begin
      @(pads_vif.mio_out or pads_vif.mio_oe);
      sample_pads();
    end
  endtask

  protected task gpio_tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off, d;
      tl_fifo.get(t);
      if (t.d_error || !t.is_write() || eg2_tl_err::refused("gpio", t)) continue;
      off = eg2_tl_offset(t, 32'h400);
      d = t.a_data;
      case (off)
        gpio_reg_pkg::GPIO_DIRECT_OUT_OFFSET:        out = eg2_tl_merge(out, t);
        gpio_reg_pkg::GPIO_DIRECT_OE_OFFSET:         oe = eg2_tl_merge(oe, t);
        gpio_reg_pkg::GPIO_MASKED_OUT_LOWER_OFFSET:  out[15:0] = (out[15:0] & ~d[31:16]) |
                                                                 (d[15:0] & d[31:16]);
        gpio_reg_pkg::GPIO_MASKED_OUT_UPPER_OFFSET:  out[31:16] = (out[31:16] & ~d[31:16]) |
                                                                  (d[15:0] & d[31:16]);
        gpio_reg_pkg::GPIO_MASKED_OE_LOWER_OFFSET:   oe[15:0] = (oe[15:0] & ~d[31:16]) |
                                                                (d[15:0] & d[31:16]);
        gpio_reg_pkg::GPIO_MASKED_OE_UPPER_OFFSET:   oe[31:16] = (oe[31:16] & ~d[31:16]) |
                                                                 (d[15:0] & d[31:16]);
        default: begin
          // PER_PIN_IO_i (bit 0 data_out; a write must cover bytes 0 and 1), PER_PIN_OE_i.
          if (off >= 32'h100 && off < 32'h180 && t.a_mask[1:0] == 2'b11) begin
            out[(off - 32'h100) / 4] = d[0];
          end else if (off >= 32'h200 && off < 32'h300 && off[2:0] == 3'b000 && t.a_mask[0]) begin
            oe[(off - 32'h200) / 8] = d[0];
          end
        end
      endcase
      sample_gpio(off);
      emit_exp();
    end
  endtask

  protected task pinmux_tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      pinmux_fifo.get(t);
      if (t.d_error || !t.is_write() || eg2_tl_err::refused("pinmux", t)) continue;
      off = eg2_tl_offset(t, 32'h1000);
      if (off >= pinmux_reg_pkg::PINMUX_MIO_OUTSEL_0_OFFSET &&
          off < pinmux_reg_pkg::PINMUX_MIO_OUTSEL_0_OFFSET + 4 * NPads && t.a_mask[0]) begin
        outsel[(off - pinmux_reg_pkg::PINMUX_MIO_OUTSEL_0_OFFSET) / 4] = t.a_data[7:0];
        sample_pinmux((off - pinmux_reg_pkg::PINMUX_MIO_OUTSEL_0_OFFSET) / 4, t.a_data[7:0]);
        remap();
        emit_exp();
        sample_pads();
      end
    end
  endtask

endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// USB (usbdev's D+, D- and VBUS sense on the pads, beside the DPI host model usb20_usbdpi): one
// item per bus event: a packet (from the first K after idle to the end-of-packet SE0, with its
// direction: the chip's when the DPI host was not driving), a bus reset (SE0 longer than 2.5 us),
// VBUS rising (connect) and falling (disconnect); a data packet's payload is decoded (its CRC16
// left out). eg2_usbdev_monitor (eg2_device_monitors.sv) extends it with usbdev's register traffic
// and predicts the chip's packets (on_item, called before each item goes out).
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_usb_monitor extends uvm_monitor;
  `uvm_component_utils(eg2_usb_monitor)

  uvm_analysis_port #(eg2_usb_item) pin_port;
  usb20_vif                         vif;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    pin_port = new("pin_port", this);
  endfunction

  // Each item before it goes to the scoreboard (the subclass's prediction).
  virtual function void on_item(eg2_usb_item it);
  endfunction

  protected function void send(eg2_usb_item::kind_e kind, bit from_chip = 0, int tr = 0,
                               realtime dur = 0);
    eg2_usb_item it = eg2_usb_item::type_id::create("it");
    it.kind = kind;
    it.from_chip = from_chip;
    it.transitions = tr;
    it.duration = dur;
    on_item(it);
    pin_port.write(it);
  endfunction

  // One full-speed packet from its first K (USB 2.0 7.1: NRZI, a bit stuffed after six ones; SYNC,
  // then the PID LSB first, then a token's 7-bit address and 4-bit endpoint): the line is sampled
  // in the middle of each 83.3 ns bit until the end-of-packet SE0.
  protected task packet(realtime t0, bit host);
    eg2_usb_item it = eg2_usb_item::type_id::create("it");
    realtime     bit_t = 1000.0ns / 12;
    bit          bits[$];
    int          ones, tr;
    bit [1:0]    prev = 2'b10;  // J
    for (int k = 0; k < 1300; k++) begin
      bit [1:0] s;
      realtime  d = t0 + (k + 0.5) * bit_t - $realtime;
      if (d > 0) #(d);
      if ($isunknown({vif.usb_p, vif.usb_n})) break;
      s = {vif.usb_p, vif.usb_n};
      if (s == 2'b00) break;  // EOP
      if (s != prev) tr++;
      if (ones == 6) begin    // a stuffed bit
        ones = 0;
        prev = s;
        continue;
      end
      bits.push_back(s == prev);
      ones = (s == prev) ? ones + 1 : 0;
      prev = s;
    end
    it.kind = eg2_usb_item::UsbPacket;
    it.from_chip = !host;
    it.transitions = tr;
    it.duration = $realtime - t0;
    if (bits.size() >= 16) begin
      bit [7:0] pid;
      for (int i = 0; i < 8; i++) pid[i] = bits[8 + i];
      it.pid = pid[3:0];
      it.pid_ok = pid[7:4] == ~pid[3:0];
      it.token = it.pid_ok && pid[1:0] == 2'b01 && bits.size() >= 27;  // OUT, IN, SOF, SETUP
      if (it.token && pid[3:0] != 4'h5) begin
        for (int i = 0; i < 7; i++) it.address[i] = bits[16 + i];
        for (int i = 0; i < 4; i++) it.endpoint[i] = bits[23 + i];
      end else begin
        it.token = 1'b0;
      end
      // A data packet: the bytes after the PID, LSB first, without the 16-bit CRC.
      if (it.is_data() && bits.size() >= 32) begin
        for (int b = 0; b < (bits.size() - 32) / 8; b++) begin
          bit [7:0] v;
          for (int i = 0; i < 8; i++) v[i] = bits[16 + 8 * b + i];
          it.data.push_back(v);
        end
      end
    end
    on_item(it);
    pin_port.write(it);
  endtask

  virtual task run_phase(uvm_phase phase);
    if (vif == null) return;
    fork
      forever begin
        @(posedge vif.usb_vbus);
        send(eg2_usb_item::UsbConnect);
        @(negedge vif.usb_vbus);
        send(eg2_usb_item::UsbDisconnect);
      end
      forever begin
        // Idle J (D+ high, D- low), then the first K starts a packet.
        logic      p, n;
        realtime   t0, se0_t0;
        @(vif.usb_p or vif.usb_n);
        p = vif.usb_p;
        n = vif.usb_n;
        if (p === 1'b0 && n === 1'b0) begin
          // SE0: a bus reset if it lasts.
          se0_t0 = $realtime;
          @(vif.usb_p or vif.usb_n);
          if ($realtime - se0_t0 > 2.5us) send(eg2_usb_item::UsbReset, 0, 0, $realtime - se0_t0);
          continue;
        end
        if (!(p === 1'b0 && n === 1'b1)) continue;  // not a K
        t0 = $realtime;
        packet(t0, vif.usb_dp_en_p2d === 1'b1 || vif.usb_dn_en_p2d === 1'b1);
      end
    join
  endtask
endclass
