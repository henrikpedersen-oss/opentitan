// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 per-peripheral scoreboards, one class per peripheral type (eg2_periph_sb_base.sv has
// the compare engine, the logs and the checks they share). Compiled with EG2_PERIPH_SB only.
//
//   class               instances            actual (pins)                expected
//   eg2_uart_sb         uart0..uart3         uart_monitor TX bytes        WDATA writes (TL-UL), or
//                                                                         the SystemC model's uart<N>
//                                                                         items where it is built in
//                       RX: RDATA reads      against the bytes the bench drove, as the RX FIFO takes them
//   eg2_spi_host_sb     spi_host0, 1         SD0 bytes (agent, device)    TXDATA bytes of TX and
//                                                                         bidirectional segments
//                       RX: RXDATA bytes     against the agent's SD1 bytes in RX segments
//   eg2_i2c_sb          i2c0..i2c2           host: the transfers the      FDATA entries, a transfer
//                                            agent (target) saw           each
//                                            target: bytes the agent read TXDATA writes
//                       RX: RDATA (host) or ACQDATA (target) against what the bench sent
//   eg2_gpio_sb         gpio                 GPIOs on their pads          gpio and pinmux writes
//   eg2_spi_device_sb   spi_device           each transaction's MISO      flash: status, JEDEC, SFDP,
//                                            bytes (host agent)           read buffer, mailbox; TPM
//                                                                         read FIFO (eg2_spi_device_
//                                                                         monitor); passthrough and
//                                                                         TPM HW registers NOT PREDICTED
//                       RX: upload FIFOs, payload and TPM write buffers against what the host sent
//   eg2_usbdev_sb       usbdev               the chip's USB packets       IN data from the packet
//                                                                         buffer, handshakes
//                                                                         (eg2_usbdev_monitor)
//                       RX: RXFIFO entries and packet-buffer words against the host's OUT/SETUP data
//   eg2_jtag_sb         jtag                 JTAG DMI items               NOT PREDICTED (the
//                                                                         sequences that drive it
//                                                                         check its values)
//   eg2_sysrst_ctrl_sb, eg2_adc_ctrl_sb, eg2_sensor_ctrl_sb, eg2_wkup_sb: the system monitor's
//                       blocks (decision 19), on eg2_sysmon_sb_base; their monitors are in
//                       eg2_sysmon_monitors.sv

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_uart_sb extends eg2_periph_sb_base #(uart_item);
  `uvm_component_utils(eg2_uart_sb)

  int unsigned idx;
  eg2_uart_tx_cg tx_cg;
`ifdef EG2_SYSMODEL
  // The bytes on the RX line, every one, as input events of the model.
  uvm_tlm_analysis_fifo #(uart_item) model_rx_fifo;
  protected bit model_used;
`endif

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    // The TX FIFO holds 32 bytes and one is on the wire: what the software wrote last may still be
    // there when the test ends.
    in_flight_max = uart_reg_pkg::TxFifoDepth + 1;
`ifdef EG2_SYSMODEL
    model_rx_fifo = new("model_rx_fifo", this);
`endif
  endfunction

  virtual function string item_line(uart_item it);
    return $sformatf("%02h %s", it.data, (it.data inside {[8'h20:8'h7e]}) ? string'(it.data) : ".");
  endfunction

  virtual function bit compare(uart_item act, uart_item exp, output string why);
    why = "";
    return act.data == exp.data;
  endfunction

  virtual function void corrupt(uart_item it);
    it.data = ~it.data;
  endfunction

  virtual function void create_cov(string path);
    tx_cg = new({path, "::eg2_uart_tx_cg"});
  endfunction

  virtual function void cov_exp(uart_item it, int unsigned pending);
    if (tx_cg != null) tx_cg.sample(pending);
  endfunction

  virtual function string agent_settings();
    uart_agent_cfg c = cfg.m_uart_agent_cfgs[idx];
    return $sformatf("uart_agent %0d: %0d baud, parity %s, TX monitor %0d, RX monitor %0d",
                     idx, int'(c.baud_rate), c.en_parity ? (c.odd_parity ? "odd" : "even") : "none",
                     c.en_tx_monitor, c.en_rx_monitor);
  endfunction

  function void check_phase(uvm_phase phase);
    required = cfg.eg2_sb_uart_required[idx];
    super.check_phase(phase);
  endfunction

`ifdef EG2_SYSMODEL
  function void start_of_simulation_phase(uvm_phase phase);
    super.start_of_simulation_phase(phase);
    // The model predicts the UARTs (eg2_sysmodel.sv): its items are the expected ones.
    exp_external = 1'b1;
    source = {"the SystemC model's ", inst, " items (eg2_sysmodel; this instance's TL-UL items are ",
              "logged in ", inst, ".expected.log, not compared)"};
  endfunction

  virtual task run_phase(uvm_phase phase);
    super.run_phase(phase);
    fork
      forever begin
        uart_item it;
        model_rx_fifo.get(it);
        eg2_sysmodel::give_input($sformatf("%s.rx", inst), it.data);
      end
    join_none
  endtask

  // The model's items as far as it has come (its time follows the chip's).
  virtual function void pull_model();
    if (!eg2_sysmodel::active()) begin
      if (exp_external) begin
        // No model in this run after all (it is built at the first RVFI reset item, and not in
        // secure-boot runs): back to the TL-UL items, those kept so far first.
        exp_external = 1'b0;
        source = "the peripheral's TL-UL traffic, converted by its extended monitor (no model in this run)";
        while (ext_held_q.size() > 0) exp_q.push_front(ext_held_q.pop_back());
      end
      return;
    end
    ext_held_q.delete();
    if (eg2_sysmodel::stop_reason() != "") begin
      // The model stopped following (a feature it does not model yet): from here nothing is a
      // prediction; said in the log and the verdict, never compared against the TL-UL items.
      if (!unpredictable) begin
        unpredictable = 1'b1;
        unpredictable_why = {"the SystemC model stopped: ", eg2_sysmodel::stop_reason()};
        log.comment($sformatf("%0t: from here NOT PREDICTED: %s (%0d items not compared)", $realtime,
                              unpredictable_why, act_q.size()));
        act_q.delete();
        exp_q.delete();
      end
      return;
    end
    eg2_sysmodel::step();
    while (eg2_sysmodel::pending(inst) > 0) begin
      entry_t   e;
      string    line = eg2_sysmodel::pull(inst);
      bit [7:0] b;
      void'($sscanf(line, "%h", b));
      e.it = uart_item::type_id::create("model");
      e.it.data = b;
      e.t = $realtime;
      exp_q.push_back(e);
      n_exp++;
    end
  endfunction
`endif
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_spi_host_sb extends eg2_periph_sb_base #(spi_item);
  `uvm_component_utils(eg2_spi_host_sb)

  int unsigned idx;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    // The agent's monitor hands bytes on in groups of num_bytes_per_trans_in_mon (4): up to 3 may
    // still wait in it at the end, and a segment's TX words may wait in the FIFO.
    in_flight_max = 7;
  endfunction

  virtual function void split(spi_item it, ref spi_item out[$], input bit rx = 1'b0);
    foreach (it.data[i]) begin
      spi_item b = spi_item::type_id::create("byte");
      b.data.push_back(it.data[i]);
      out.push_back(b);
    end
  endfunction

  virtual function string item_line(spi_item it);
    if (it.data.size() == 0) return "--";
    return $isunknown(it.data[0]) ? "xx (not defined: an RX segment)" : $sformatf("%02h", it.data[0]);
  endfunction

  virtual function bit compare(spi_item act, spi_item exp, output string why);
    why = "";
    if (act.data.size() == 0 || exp.data.size() == 0) return act.data.size() == exp.data.size();
    // An expected X (an RX segment's SD0) is not compared.
    return act.data[0] ==? exp.data[0];
  endfunction

  virtual function void corrupt(spi_item it);
    if (it.data.size() > 0) it.data[0] = ~it.data[0];
  endfunction

  virtual function string agent_settings();
    spi_agent_cfg c = cfg.m_spi_device_agent_cfgs[idx];
    return $sformatf("spi_agent (device mode) %0d: CPOL %0d CPHA %0d, csid %0d, %0d bytes per item",
                     idx, c.sck_polarity[0], c.sck_phase[0], c.csid, c.num_bytes_per_trans_in_mon);
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_spi_device_sb extends eg2_periph_sb_base #(eg2_spi_device_item);
  `uvm_component_utils(eg2_spi_device_sb)

  eg2_spi_device_monitor mon;   // for the per-mode NOT PREDICTED counts at the end

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    in_flight_max = 0;
    source = {"spi_device's TL-UL traffic (command entries, FLASH_STATUS, JEDEC, buffers, TPM ",
              "read FIFO), converted by eg2_spi_device_monitor when each host transaction ends"};
  endfunction

  virtual function string item_line(eg2_spi_device_item it);
    return it.line();
  endfunction

  // A transaction in which the chip drove MISO is an output; the host's writes are logged only.
  virtual function bit is_output(eg2_spi_device_item it);
    return it.kind != eg2_spi_device_item::SpidRx && !it.write && it.data.size() > 0;
  endfunction

  virtual function bit compare(eg2_spi_device_item act, eg2_spi_device_item exp, output string why);
    why = "";
    if (act.kind != exp.kind || act.opcode != exp.opcode || act.addr != exp.addr) begin
      why = "a different transaction";
      return 1'b0;
    end
    if (!exp.pred) return 1'b1;
    if (act.data.size() != exp.data.size()) begin
      why = $sformatf("%0d bytes, expected %0d", act.data.size(), exp.data.size());
      return 1'b0;
    end
    foreach (exp.data[i]) begin
      if (i < exp.care.size() && !exp.care[i]) continue;
      if (act.data[i] !== exp.data[i]) begin
        why = $sformatf("byte %0d of %s", i, exp.what);
        return 1'b0;
      end
    end
    return 1'b1;
  endfunction

  virtual function bit compare_rx(eg2_spi_device_item rd, eg2_spi_device_item drv,
                                  output string why);
    why = "";
    if (rd.what != drv.what) begin
      why = "a different register";
      return 1'b0;
    end
    return ((rd.word ^ drv.word) & drv.word_care) == 0;
  endfunction

  virtual function void corrupt(eg2_spi_device_item it);
    foreach (it.data[i]) it.data[i] = ~it.data[i];
  endfunction

  // A transaction the monitor predicted (with at least one byte fixed).
  virtual function bit corruptible(eg2_spi_device_item it);
    return it.pred;
  endfunction

  virtual function string agent_settings();
    return $sformatf("spi_agent (host mode): spi mode %s", cfg.m_spi_host_agent_cfg.spi_func_mode.name());
  endfunction

  function void report_phase(uvm_phase phase);
    if (mon != null) begin
      foreach (mon.n_not_pred[w]) note($sformatf("NOT PREDICTED: %0d transactions (%s)",
                                                 mon.n_not_pred[w], w));
    end
    super.report_phase(phase);
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_i2c_sb extends eg2_periph_sb_base #(i2c_item);
  `uvm_component_utils(eg2_i2c_sb)

  int unsigned idx;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    in_flight_max = 1;
  endfunction

  // The chip is the target when the agent is the controller (Host).
  protected function bit chip_is_target();
    return cfg.m_i2c_agent_cfgs[idx].if_mode == Host;
  endfunction

  // Host mode: one item per transfer. Target mode: the bytes the agent read are the chip's
  // output, one item per byte; the agent's writes are the chip's input, not an output.
  virtual function void split(i2c_item it, ref i2c_item out[$], input bit rx = 1'b0);
    if (rx || !chip_is_target() || !is_transfer(it)) begin
      out.push_back(it);
      return;
    end
    if (it.bus_op != BusOpRead) return;
    foreach (it.data_q[i]) out.push_back(eg2_i2c_monitor::byte_item(it.data_q[i]));
  endfunction

  protected function bit is_transfer(i2c_item it);
    return it.tran_id != -1;
  endfunction

  virtual function string item_line(i2c_item it);
    string s;
    if (!is_transfer(it)) begin
      return it.signal == 0 ? $sformatf("%02h", it.data_q[0]) :
                              $sformatf("%02h %s", it.data_q[0], it.signal.name());
    end
    s = $sformatf("S %02h %s", it.addr, it.bus_op == BusOpRead ? "r" : "w");
    if (it.addr_ack == i2c_pkg::NACK) return {s, " NACK"};
    if (it.bus_op == BusOpRead) s = {s, $sformatf(" %0d bytes", it.data_q.size())};
    else foreach (it.data_q[i]) s = {s, $sformatf(" %02h", it.data_q[i])};
    foreach (it.data_ack_q[i]) if (it.bus_op == BusOpWrite && it.data_ack_q[i] == i2c_pkg::NACK) begin
      s = {s, $sformatf(" NACK@%0d", i)};
      break;
    end
    return s;
  endfunction

  virtual function bit compare(i2c_item act, i2c_item exp, output string why);
    int n;
    why = "";
    if (!is_transfer(act) || !is_transfer(exp)) begin
      return act.data_q.size() == exp.data_q.size() &&
             (act.data_q.size() == 0 || act.data_q[0] == exp.data_q[0]);
    end
    if (act.addr != exp.addr || act.bus_op != exp.bus_op) begin
      why = "address or direction";
      return 1'b0;
    end
    if (act.addr_ack == i2c_pkg::NACK) begin
      // The target did not answer: the controller stops; what was queued after it is optional.
      foreach (exp_q[i]) exp_q[i].optional = 1'b1;
      return 1'b1;
    end
    n = exp.data_q.size();
    foreach (act.data_ack_q[i]) begin
      if (act.bus_op == BusOpWrite && act.data_ack_q[i] == i2c_pkg::NACK) begin
        n = i + 1;  // NACKed: the transfer ends after this byte
        foreach (exp_q[j]) exp_q[j].optional = 1'b1;
        break;
      end
    end
    if (act.data_q.size() != n) begin
      why = $sformatf("%0d bytes, expected %0d", act.data_q.size(), n);
      return 1'b0;
    end
    if (act.bus_op == BusOpWrite) begin
      for (int i = 0; i < n; i++) begin
        if (act.data_q[i] != exp.data_q[i]) begin
          why = $sformatf("byte %0d", i);
          return 1'b0;
        end
      end
    end
    return 1'b1;
  endfunction

  virtual function bit compare_rx(i2c_item rd, i2c_item drv, output string why);
    why = "";
    if (rd.data_q.size() != 1 || drv.data_q.size() != 1) return 1'b0;
    // ACQDATA: the entry's kind too (START, STOP, data).
    if (chip_is_target() && rd.signal != drv.signal) begin
      why = "ACQDATA signal";
      return 1'b0;
    end
    // A STOP entry's ABYTE "contains no data" (i2c.hjson ACQDATA.SIGNAL STOP): not compared. The
    // RTL leaves the last shifted-in byte there (i2c_target_fsm.sv, {AcqStop, input_byte}; 0x0c in
    // chip_sw_i2c_device_tx_rx_eg2sb, 2026-10-28).
    if (chip_is_target() && drv.signal inside {i2c_pkg::AcqStop, i2c_pkg::AcqNackStop}) return 1'b1;
    if (rd.data_q[0] != drv.data_q[0]) return 1'b0;
    return 1'b1;
  endfunction

  virtual function void corrupt(i2c_item it);
    if (is_transfer(it)) it.addr ^= 10'h1;
    else if (it.data_q.size() > 0) it.data_q[0] ^= 8'h1;
  endfunction

  eg2_i2c_xfer_cg xfer_cg;

  virtual function void create_cov(string path);
    xfer_cg = new({path, "::eg2_i2c_xfer_cg"});
  endfunction

  // Each transfer the agent saw (in target mode the transfers arrive split into bytes: the
  // agent's reads are sampled per byte as one-byte reads).
  virtual function void cov_act(i2c_item it);
    int unsigned ack;
    if (xfer_cg == null) return;
    if (it.addr_ack == i2c_pkg::NACK) ack = 1;
    foreach (it.data_ack_q[i]) begin
      if (ack == 0 && it.bus_op == BusOpWrite && it.data_ack_q[i] == i2c_pkg::NACK) ack = 2;
    end
    xfer_cg.sample(chip_is_target(), is_transfer(it) ? it.bus_op == BusOpRead : 1'b1, ack,
                   it.data_q.size());
  endfunction

  virtual function string agent_settings();
    i2c_agent_cfg c = cfg.m_i2c_agent_cfgs[idx];
    return $sformatf("i2c_agent %0d: %s mode (the chip is the %s), target address 0x%02h, monitor %0d",
                     idx, c.if_mode == Host ? "controller" : "target",
                     c.if_mode == Host ? "target" : "controller", c.target_addr0, c.en_monitor);
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_gpio_sb extends eg2_periph_sb_base #(eg2_gpio_item);
  `uvm_component_utils(eg2_gpio_sb)

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    in_flight_max = 1;
  endfunction

  virtual function string item_line(eg2_gpio_item it);
    return it.line();
  endfunction

  virtual function bit compare(eg2_gpio_item act, eg2_gpio_item exp, output string why);
    why = "";
    if ((act.known & act.oe) != act.oe) begin
      why = $sformatf("X or Z driven on GPIOs %08h", act.oe & ~act.known);
      return 1'b0;
    end
    if (act.oe != exp.oe) begin
      why = $sformatf("output enables differ on %08h", act.oe ^ exp.oe);
      return 1'b0;
    end
    if ((act.out & act.oe) != (exp.out & exp.oe)) begin
      why = $sformatf("levels differ on %08h", (act.out ^ exp.out) & act.oe);
      return 1'b0;
    end
    return 1'b1;
  endfunction

  virtual function void corrupt(eg2_gpio_item it);
    it.oe = ~it.oe & it.routed;
    if (it.oe == '0) it.oe = 32'h1;
  endfunction

  virtual function string agent_settings();
    return "eg2_gpio_monitor: the MIO pads pinmux drives, GPIOs mapped by MIO_OUTSEL";
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_usbdev_sb extends eg2_periph_sb_base #(eg2_usb_item);
  `uvm_component_utils(eg2_usbdev_sb)

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    in_flight_max = 1;
    source = {"usbdev's TL-UL traffic (CONFIGIN, the packet buffer, the endpoint, stall and ",
              "receive-enable registers, the available-buffer FIFOs), converted by ",
              "eg2_usbdev_monitor when the host's token or data packet ends"};
  endfunction

  virtual function string item_line(eg2_usb_item it);
    return it.line();
  endfunction

  // The chip's packets are compared; the host's packets and the bus events are logged.
  virtual function bit is_output(eg2_usb_item it);
    return it.kind == eg2_usb_item::UsbPacket && it.from_chip;
  endfunction

  virtual function bit compare(eg2_usb_item act, eg2_usb_item exp, output string why);
    why = "";
    if (exp.loose && act.pid_ok && act.pid == 4'ha) return 1'b1;
    if (exp.is_data()) begin
      if (!act.is_data()) begin
        why = "a data packet expected";
        return 1'b0;
      end
      if (act.data.size() != exp.data.size()) begin
        why = $sformatf("%0d bytes, expected %0d", act.data.size(), exp.data.size());
        return 1'b0;
      end
      foreach (exp.data[i]) begin
        if (i < exp.data_care.size() && !exp.data_care[i]) continue;
        if (act.data[i] != exp.data[i]) begin
          why = $sformatf("byte %0d", i);
          return 1'b0;
        end
      end
      return 1'b1;
    end
    if (exp.loose && exp.pid == 4'ha && act.is_data()) return 1'b1;
    if (!act.pid_ok || act.pid != exp.pid) begin
      why = "handshake";
      return 1'b0;
    end
    return 1'b1;
  endfunction

  virtual function bit compare_rx(eg2_usb_item rd, eg2_usb_item drv, output string why);
    why = "";
    if (rd.kind != drv.kind) begin
      why = "a different register";
      return 1'b0;
    end
    if (drv.kind == eg2_usb_item::UsbRxEntry) begin
      return rd.endpoint == drv.endpoint && rd.setup == drv.setup && rd.size == drv.size &&
             rd.buffer == drv.buffer;
    end
    return rd.offset == drv.offset && ((rd.word ^ drv.word) & drv.care) == 0;
  endfunction

  // The packet becomes a STALL: never what the model predicted in place of data, ACK or NAK.
  virtual function void corrupt(eg2_usb_item it);
    it.pid = 4'he;
    it.pid_ok = 1'b1;
    it.data.delete();
  endfunction

  // A STALL the model predicted would not change: pick another packet.
  virtual function bit corruptible(eg2_usb_item it);
    return !(it.pid_ok && it.pid == 4'he);
  endfunction

  eg2_usb_cg usb_cg;

  virtual function void create_cov(string path);
    usb_cg = new({path, "::eg2_usb_cg"});
  endfunction

  virtual function void cov_act(eg2_usb_item it);
    if (usb_cg != null && it.kind == eg2_usb_item::UsbPacket && it.pid_ok) begin
      usb_cg.sample(it.pid, it.endpoint, it.token, it.from_chip);
    end
  endfunction

  virtual function string agent_settings();
    return "eg2_usb_monitor on D+, D-, VBUS beside the DPI host (usb20_usbdpi)";
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// A port with no pins of its own to compare (pinmux): only its TL-UL accesses are classified.
class eg2_tl_port_sb extends eg2_periph_sb_base #(tl_seq_item);
  `uvm_component_utils(eg2_tl_port_sb)

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    predicted = 1'b0;
    unpredictable_why = "no pins of its own here: only its TL-UL error responses are checked";
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_jtag_sb extends eg2_periph_sb_base #(jtag_riscv_item);
  `uvm_component_utils(eg2_jtag_sb)

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    predicted = 1'b0;
    unpredictable_why = "JTAG values are checked by the sequences that drive it (the register model's JTAG map)";
  endfunction

  virtual function string item_line(jtag_riscv_item it);
    return $sformatf("ir %0h dr %0h op %0h addr %0h data %08h status %0h", it.ir, it.dr, it.op,
                     it.addr, it.data, it.status);
  endfunction

  virtual function string agent_settings();
    jtag_riscv_agent_cfg c = cfg.m_jtag_riscv_agent_cfg;
    return $sformatf("jtag_riscv_agent: %s", c.use_jtag_dmi ? "DMI over JTAG" : "RV_DM TAP");
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// The system monitor's blocks (status/decisions.md 19): one scoreboard per block, its items from
// that block's monitor (eg2_sysmon_monitors.sv). A state item matches on the bits the expected
// item fixes; an event by its name; a register the core read on the bits the model fixes.
class eg2_sysmon_sb_base extends eg2_periph_sb_base #(eg2_sysmon_item);
  `uvm_component_utils(eg2_sysmon_sb_base)

  eg2_sysmon_monitor mon;
  string             agent_desc = "";

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    has_rx = 1'b1;
    in_flight_max = 1;
  endfunction

  virtual function string item_line(eg2_sysmon_item it);
    return it.line();
  endfunction

  virtual function bit compare(eg2_sysmon_item act, eg2_sysmon_item exp, output string why);
    why = "";
    if (act.kind != exp.kind || act.name != exp.name) begin
      why = "a different item";
      return 1'b0;
    end
    if (exp.kind == eg2_sysmon_item::SysEvent) return 1'b1;
    if (((act.value ^ exp.value) & exp.care) != 0) begin
      why = $sformatf("bits %08h differ", (act.value ^ exp.value) & exp.care);
      return 1'b0;
    end
    return 1'b1;
  endfunction

  virtual function bit compare_rx(eg2_sysmon_item rd, eg2_sysmon_item drv, output string why);
    why = "";
    if (rd.name != drv.name) begin
      why = "a different register";
      return 1'b0;
    end
    if (((rd.value ^ drv.value) & drv.care) != 0) begin
      why = $sformatf("bits %08h differ", (rd.value ^ drv.value) & drv.care);
      return 1'b0;
    end
    return 1'b1;
  endfunction

  virtual function void corrupt(eg2_sysmon_item it);
    if (it.kind == eg2_sysmon_item::SysEvent) it.name = it.name == "recov_alert" ? "fatal_alert" : "recov_alert";
    else it.value = ~it.value;
  endfunction

  virtual function string agent_settings();
    if (mon == null) return agent_desc;
    return $sformatf("%s; always-on clock %0.3f us, outputs settle for %0d cycles, %0d-cycle race window",
                     agent_desc, mon.aon_period / 1us, mon.settle_cycles, mon.race_cycles);
  endfunction

  function void report_phase(uvm_phase phase);
    if (mon != null) note($sformatf("%0d register reads checked, %0d input events modelled",
                                    mon.n_reads, mon.n_events));
    super.report_phase(phase);
  endfunction
endclass

class eg2_sysrst_ctrl_sb extends eg2_sysmon_sb_base;
  `uvm_component_utils(eg2_sysrst_ctrl_sb)
  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    agent_desc = "eg2_sysrst_ctrl_monitor on sysrst_ctrl's pins, reset request and TL-UL port";
    source = {"sysrst_ctrl's registers (pin override, key inversion, combo, key interrupt, ULP, ",
              "auto-block, EC reset) and the input pins, by eg2_sysrst_ctrl_monitor"};
  endfunction
endclass

class eg2_adc_ctrl_sb extends eg2_sysmon_sb_base;
  `uvm_component_utils(eg2_adc_ctrl_sb)
  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    agent_desc = "eg2_adc_ctrl_monitor on adc_ctrl's AST ADC interface, interrupt, wake-up and TL-UL port";
    source = {"adc_ctrl's filter, sample and interrupt registers and the AST ADC's samples, by ",
              "eg2_adc_ctrl_monitor (OpenTitan adc_ctrl_scoreboard's filter and debounce model)"};
  endfunction
endclass

class eg2_sensor_ctrl_sb extends eg2_sysmon_sb_base;
  `uvm_component_utils(eg2_sensor_ctrl_sb)
  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    agent_desc = {"eg2_sensor_ctrl_monitor on sensor_ctrl's AST events and status, its TL-UL port ",
                  "and the alert agents of sensor_ctrl_recov_alert and sensor_ctrl_fatal_alert"};
    source = {"sensor_ctrl's ALERT_EN, FATAL_ALERT_EN, ALERT_TEST and the AST events, by ",
              "eg2_sensor_ctrl_monitor"};
  endfunction
endclass

class eg2_wkup_sb extends eg2_sysmon_sb_base;
  `uvm_component_utils(eg2_wkup_sb)
  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    agent_desc = {"eg2_wkup_monitor on the pads pinmux receives, its wake-up request to pwrmgr ",
                  "and the TL-UL ports of pinmux and pwrmgr"};
    source = {"pinmux's wake-up detector registers and the pad activity, and pwrmgr's WAKEUP_EN, by ",
              "eg2_wkup_monitor"};
  endfunction
endclass
