// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 conversions of the two device-side peripherals, spi_device and usbdev (EG2 spec
// "Per-peripheral scoreboards"): the host on the bench (the spi_agent in host mode, the DPI USB
// host) asks, the chip answers from what the software put into the block's buffers and registers.
// Each monitor takes the block's TL-UL port through its tap (tb/eg2_periph_tl.svh) and the host's
// requests from the pins, and predicts the chip's answer when the host's request ends:
//
//   eg2_spi_device_monitor  the bytes spi_device sends on MISO for each flash command and TPM read
//   eg2_usbdev_monitor      each packet usbdev sends: an IN data packet, or the handshake
//
// and, for the receive direction, the expected value of each register and buffer word the core
// reads back of what the host sent (upload FIFOs, payload and TPM write buffers; usbdev's RXFIFO
// and packet buffer). The prediction follows the blocks' documentation (hw/ip/spi_device/doc,
// hw/ip/usbdev/doc, data/<ip>.hjson), never their RTL. Compiled with EG2_PERIPH_SB only.

// The value a byte of a software-written buffer had at a given time: the last two writes are kept,
// and a byte written within `margin` of that time is not predicted.
class eg2_buf_hist;
  protected bit [7:0] v0[int], v1[int];
  protected realtime  t0[int], t1[int];

  function void write(int idx, bit [7:0] v, realtime t = $realtime);
    if (v0.exists(idx)) begin
      v1[idx] = v0[idx];
      t1[idx] = t0[idx];
    end
    v0[idx] = v;
    t0[idx] = t;
  endfunction

  // 1 with `v` if the value at `t` is known, 0 if not.
  function bit at(int idx, realtime t, realtime margin, output bit [7:0] v);
    v = '0;
    if (!v0.exists(idx)) return 1'b0;
    if (t0[idx] <= t - margin) begin
      v = v0[idx];
      return 1'b1;
    end
    if (t0[idx] >= t + margin && t1.exists(idx) && t1[idx] <= t - margin) begin
      v = v1[idx];
      return 1'b1;
    end
    return 1'b0;
  endfunction

  function bit cur(int idx, output bit [7:0] v);
    v = v0.exists(idx) ? v0[idx] : '0;
    return v0.exists(idx);
  endfunction

  function void clear();
    v0.delete();
    v1.delete();
    t0.delete();
    t1.delete();
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// spi_device (hw/ip/spi_device: README.md, doc/theory_of_operation.md, doc/programmers_guide.md,
// doc/registers.md, data/spi_device.hjson).
//
// Item (eg2_spi_device_item): one SPI transaction as the host agent saw it: flash opcode and
// address, or TPM direction and address, and the bytes on MISO (a read) or MOSI (a write: logged,
// not an output). Expected, per transaction, when it ends, from the command information entry the
// opcode matches (CMD_INFO_<i>, the highest matching index; EN4B, EX4B, WREN, WRDI) and CONTROL.MODE:
//   Read Status (CMD_INFO_0..2)  the byte of FLASH_STATUS (busy, WEL as the earlier commands left
//                                them: an upload entry with busy sets BUSY, WREN/WRDI set and clear
//                                WEL; the rest as software wrote it), every byte
//   Read JEDEC ID (CMD_INFO_3)   JEDEC_CC.num_cc times JEDEC_CC.cc, JEDEC_ID.mf, id[7:0], id[15:8]
//   Read SFDP (CMD_INFO_4)       the SFDP buffer (egress 0xC00) at address[7:0], wrapping at 256
//   Read (CMD_INFO_5..10)        the mailbox (egress 0x800) for addresses in the 1 kB at
//                                MAILBOX_ADDR when CFG.mailbox_en, else the read buffer (egress 0x000)
//                                at address[10:0]
// in flash mode, and in passthrough mode where INTERCEPT_EN intercepts them (status, jedec, sfdp,
// mbx); a byte of a buffer software rewrote close to when the host read it is not predicted.
//   TPM read                     the bytes software wrote to TPM_READ_FIFO while the command waited
//                                (a write while no read command is active is dropped:
//                                INTR_STATE.tpm_rdfifo_drop); a read of a return-by-HW register
//                                (FIFO mode, TPM_CFG.hw_reg_dis clear) or an invalid locality is
//                                NOT PREDICTED
// RX: UPLOAD_CMDFIFO (opcode, busy, WEL, 4-byte mode of each uploaded command), UPLOAD_ADDRFIFO,
// the payload buffer (ingress 0xE00) of the last upload, TPM_CMD_ADDR and the TPM write buffer
// (ingress 0xF80) against what the host sent.
// NOT PREDICTED (per transaction, said in the log): passthrough commands that are not intercepted
// (the downstream flash answers), commands while CONTROL.MODE is disabled, generic-mode transfers,
// TPM return-by-HW registers, dual/quad read pipelines' first bytes are compared as the agent
// decoded them.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_spi_device_monitor extends uvm_monitor;
  `uvm_component_utils(eg2_spi_device_monitor)

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_tlm_analysis_fifo #(spi_item)    host_fifo;   // the agent's transactions (CSB high again)
  uvm_tlm_analysis_fifo #(spi_item)    req_fifo;    // after the address phase (data starts)
  uvm_tlm_analysis_fifo #(spi_item)    gen_fifo;    // generic mode: the chip's bytes
  uvm_analysis_port #(eg2_spi_device_item) pin_port;
  uvm_analysis_port #(eg2_spi_device_item) tl_exp_port;
  uvm_analysis_port #(eg2_spi_device_item) rx_exp_port;
  uvm_analysis_port #(eg2_spi_device_item) rx_rd_port;
  // spi_host0's expected stream: passthrough drives spi_host0's pins.
  uvm_analysis_port #(spi_item)            passthrough_port;

  virtual tl_if tl_vif;
  spi_agent_cfg agent_cfg;
  string        inst = "spi_device";
  realtime      margin = 2us;

  typedef struct {
    bit [7:0]  opcode;
    bit        busy;
    bit        wel;
    bit        addr4b;
    bit        busy_unsure;
  } upload_t;

  // Registers.
  protected bit [1:0]  mode = 2'd1;
  protected bit        mailbox_en;
  protected bit [3:0]  intercept;
  protected bit        addr_4b;
  protected bit [23:0] flash_status;
  protected bit [23:0] status_unsure;   // bits software changed: not predicted in the next read
  protected bit [7:0]  jedec_cc = 8'h7f, jedec_num_cc;
  protected bit [23:0] jedec_id;
  protected bit [31:0] mailbox_addr;
  protected bit [31:0] cmd_info[24];
  protected bit [31:0] cmd_en4b, cmd_ex4b, cmd_wren, cmd_wrdi;
  protected bit [4:0]  tpm_cfg;
  // Buffers.
  protected eg2_buf_hist egress;
  protected bit [7:0]  payload[int];      // ingress payload buffer, by offset
  protected bit [7:0]  tpm_wr[int];       // TPM write buffer, by offset
  protected upload_t   cmdfifo[$];
  protected bit [31:0] addrfifo[$];
  protected bit [31:0] tpm_cmdaddr[$];
  protected bit [7:0]  tpm_rdfifo[$];
  protected bit        tpm_rd_active, tpm_rdfifo_unsure;
  // When each transaction's data phase began (the agent's req item).
  protected realtime   data_t0[spi_item];
  protected bit        passthrough_announced;
  int unsigned         n_not_pred[string];
  eg2_spi_device_cg    spid_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo          = new("tl_fifo", this);
    host_fifo        = new("host_fifo", this);
    req_fifo         = new("req_fifo", this);
    gen_fifo         = new("gen_fifo", this);
    pin_port         = new("pin_port", this);
    tl_exp_port      = new("tl_exp_port", this);
    rx_exp_port      = new("rx_exp_port", this);
    rx_rd_port       = new("rx_rd_port", this);
    passthrough_port = new("passthrough_port", this);
    egress = new();
    reset_model();
  endfunction

  function void create_cov(string path);
    spid_cg = new({path, "::eg2_spi_device_cg"});
  endfunction

  protected function void reset_model();
    mode = 2'd1;
    mailbox_en = 1'b0;
    intercept = '0;
    addr_4b = 1'b0;
    flash_status = '0;
    status_unsure = '0;
    jedec_cc = 8'h7f;
    jedec_num_cc = '0;
    jedec_id = '0;
    mailbox_addr = '0;
    foreach (cmd_info[i]) cmd_info[i] = 32'h0000_7000;
    cmd_en4b = '0;
    cmd_ex4b = '0;
    cmd_wren = '0;
    cmd_wrdi = '0;
    tpm_cfg = '0;
    egress.clear();
    payload.delete();
    tpm_wr.delete();
    cmdfifo.delete();
    addrfifo.delete();
    tpm_cmdaddr.delete();
    tpm_rdfifo.delete();
    tpm_rd_active = 1'b0;
    tpm_rdfifo_unsure = 1'b0;
  endfunction

  virtual task run_phase(uvm_phase phase);
    if (tl_vif == null) return;
    fork
      tl_thread();
      host_thread();
      req_thread();
      gen_thread();
      forever begin
        @(negedge tl_vif.rst_n);
        reset_model();
        marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, "spi_device reset");
        marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, "spi_device reset");
      end
    join
  endtask

  protected function void marker(uvm_analysis_port #(eg2_spi_device_item) port,
                                 eg2_periph_marker::kind_e kind, int keep, string why);
    eg2_spi_device_item m = eg2_spi_device_item::type_id::create("marker");
    eg2_periph_marker::mark(m, kind, keep, why);
    port.write(m);
  endfunction

  protected function void not_predicted(string what);
    if (!n_not_pred.exists(what)) n_not_pred[what] = 0;
    n_not_pred[what]++;
  endfunction

  // ----- The register traffic -----

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      if (t.d_error || eg2_tl_err::refused(inst, t)) continue;
      off = eg2_tl_offset(t, 32'h2000);
      if (t.is_write()) tl_write(off, t);
      else tl_read(off, t);
    end
  endtask

  protected function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    if (off >= spi_device_reg_pkg::SPI_DEVICE_EGRESS_BUFFER_OFFSET &&
        off < spi_device_reg_pkg::SPI_DEVICE_EGRESS_BUFFER_OFFSET +
              spi_device_reg_pkg::SPI_DEVICE_EGRESS_BUFFER_SIZE) begin
      int base = off - spi_device_reg_pkg::SPI_DEVICE_EGRESS_BUFFER_OFFSET;
      for (int b = 0; b < 4; b++) if (t.a_mask[b]) egress.write(base + b, d[8 * b +: 8]);
      return;
    end
    case (off)
      spi_device_reg_pkg::SPI_DEVICE_CONTROL_OFFSET: begin
        mode = d[5:4];
        if (mode == 2'd2 && !passthrough_announced) begin
          spi_item m = spi_item::type_id::create("passthrough");
          passthrough_announced = 1'b1;
          eg2_periph_marker::mark(m, eg2_periph_marker::MarkUnpredicted, 0,
                                  "spi_device passthrough drives spi_host0's pins");
          passthrough_port.write(m);
        end
      end
      spi_device_reg_pkg::SPI_DEVICE_CFG_OFFSET: mailbox_en = d[24];
      spi_device_reg_pkg::SPI_DEVICE_INTERCEPT_EN_OFFSET: intercept = d[3:0];
      spi_device_reg_pkg::SPI_DEVICE_ADDR_MODE_OFFSET: addr_4b = d[0];
      spi_device_reg_pkg::SPI_DEVICE_FLASH_STATUS_OFFSET: begin
        // Software may clear BUSY and WEL but not set them; the rest it writes. The change reaches
        // the host at a commit point: not predicted in the next Read Status.
        bit [23:0] n = {d[23:2], flash_status[1] & d[1], flash_status[0] & d[0]};
        status_unsure |= n ^ flash_status;
        flash_status = n;
      end
      spi_device_reg_pkg::SPI_DEVICE_JEDEC_CC_OFFSET: begin
        jedec_cc = d[7:0];
        jedec_num_cc = d[15:8];
      end
      spi_device_reg_pkg::SPI_DEVICE_JEDEC_ID_OFFSET: jedec_id = d[23:0];
      spi_device_reg_pkg::SPI_DEVICE_MAILBOX_ADDR_OFFSET: mailbox_addr = {d[31:10], 10'h0};
      spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_EN4B_OFFSET: cmd_en4b = d;
      spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_EX4B_OFFSET: cmd_ex4b = d;
      spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_WREN_OFFSET: cmd_wren = d;
      spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_WRDI_OFFSET: cmd_wrdi = d;
      spi_device_reg_pkg::SPI_DEVICE_TPM_CFG_OFFSET: tpm_cfg = d[4:0];
      spi_device_reg_pkg::SPI_DEVICE_TPM_READ_FIFO_OFFSET: begin
        // Accepted only while a TPM read command waits for it (else dropped).
        for (int b = 0; b < 4; b++) begin
          if (!t.a_mask[b]) continue;
          if (tpm_rd_active) tpm_rdfifo.push_back(d[8 * b +: 8]);
        end
      end
      default: begin
        if (off >= spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_0_OFFSET &&
            off <= spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_23_OFFSET) begin
          cmd_info[(off - spi_device_reg_pkg::SPI_DEVICE_CMD_INFO_0_OFFSET) / 4] = d;
        end
      end
    endcase
  endfunction

  // The receive side: each read of what the host sent, against the model's copy.
  protected function void rx(string what, bit [31:0] actual, bit [31:0] expected,
                             bit [31:0] care);
    eg2_spi_device_item e = eg2_spi_device_item::type_id::create("rx_exp");
    eg2_spi_device_item a = eg2_spi_device_item::type_id::create("rx_rd");
    e.kind = eg2_spi_device_item::SpidRx;
    e.what = what;
    e.word = expected & care;
    e.word_care = care;
    a.kind = eg2_spi_device_item::SpidRx;
    a.what = what;
    a.word = actual;
    rx_exp_port.write(e);
    rx_rd_port.write(a);
  endfunction

  protected function void tl_read(bit [31:0] off, tl_seq_item t);
    bit [31:0] ing = spi_device_reg_pkg::SPI_DEVICE_INGRESS_BUFFER_OFFSET;
    case (off)
      spi_device_reg_pkg::SPI_DEVICE_UPLOAD_CMDFIFO_OFFSET: if (cmdfifo.size() > 0) begin
        upload_t u = cmdfifo.pop_front();
        rx("UPLOAD_CMDFIFO", t.d_data, {u.addr4b, u.wel, u.busy, 5'h0, u.opcode},
           u.busy_unsure ? 32'h0000_c0ff : 32'h0000_e0ff);
      end
      spi_device_reg_pkg::SPI_DEVICE_UPLOAD_ADDRFIFO_OFFSET: if (addrfifo.size() > 0) begin
        rx("UPLOAD_ADDRFIFO", t.d_data, addrfifo.pop_front(), '1);
      end
      spi_device_reg_pkg::SPI_DEVICE_TPM_CMD_ADDR_OFFSET: if (tpm_cmdaddr.size() > 0) begin
        rx("TPM_CMD_ADDR", t.d_data, tpm_cmdaddr.pop_front(), '1);
      end
      default: begin
        // The ingress buffers: payload at SRAM 0xE00, the TPM write buffer at 0xF80.
        if (off >= ing && off < ing + spi_device_reg_pkg::SPI_DEVICE_INGRESS_BUFFER_SIZE) begin
          int sram = off - ing + 32'he00;
          bit [31:0] e = '0, c = '0;
          for (int b = 0; b < 4; b++) begin
            int i = sram + b;
            if (i >= 32'he00 && i < 32'hf00 && payload.exists(i - 32'he00)) begin
              e[8 * b +: 8] = payload[i - 32'he00];
              c[8 * b +: 8] = 8'hff;
            end else if (i >= 32'hf80 && i < 32'hfc0 && tpm_wr.exists(i - 32'hf80)) begin
              e[8 * b +: 8] = tpm_wr[i - 32'hf80];
              c[8 * b +: 8] = 8'hff;
            end
          end
          if (c != 0) rx($sformatf("ingress[%03h]", sram), t.d_data, e, c);
        end
      end
    endcase
  endfunction

  // ----- The host's transactions -----

  // The end of a transaction's address phase (and dummy cycles): what spi_device makes visible to
  // software then (an uploaded command's opcode and address; a TPM read's command and address) is
  // put into the model's FIFOs now, as software may read them while the host still clocks.
  protected task req_thread();
    forever begin
      spi_item it;
      req_fifo.get(it);
      data_t0[it] = $realtime;
      if (agent_cfg == null) continue;
      if (agent_cfg.spi_func_mode == SpiModeTpm) tpm_req(it);
      else if (agent_cfg.spi_func_mode == SpiModeFlash) flash_req(it);
    end
  endtask

  protected function bit [31:0] addr_of(spi_item it);
    bit [31:0] ad;
    foreach (it.address_q[i]) ad = {ad[23:0], it.address_q[i]};
    return ad;
  endfunction

  protected function void flash_req(spi_item it);
    int unsigned slot = matched_slot(it.opcode);
    if (mode == 0 || slot < 11 || slot >= 24 || !cmd_info[slot][24]) return;
    begin
      upload_t u;
      u.opcode = it.opcode;
      u.busy = flash_status[0];
      u.wel = flash_status[1];
      u.addr4b = addr_4b;
      // An entry that sets BUSY itself: whether its BUSY bit shows the old or new state is not
      // documented.
      u.busy_unsure = cmd_info[slot][25];
      cmdfifo.push_back(u);
      if (cmd_info[slot][9:8] != 0 && it.address_q.size() > 0) addrfifo.push_back(addr_of(it));
      // The payload arrives from now: not predicted until the transaction ends.
      if (it.write_command) payload.delete();
    end
  endfunction

  // A TPM read: uploaded (TPM_CMD_ADDR) unless the hardware answers it; the read FIFO takes
  // software's data from now on.
  protected function void tpm_req(spi_item it);
    bit [23:0] ad = addr_of(it);
    if (it.write_command || !tpm_cfg[0]) return;
    tpm_rd_active = 1'b1;
    tpm_rdfifo.delete();
    if (tpm_cfg[4] && ad[15:12] > 4) return;
    if (tpm_hw_reg(ad)) return;
    tpm_cmdaddr.push_back({1'b1, 1'b0, 6'(it.read_size - 1), ad});
  endfunction

  protected task gen_thread();
    forever begin
      spi_item it;
      eg2_spi_device_item e, a;
      gen_fifo.get(it);
      if (agent_cfg == null || agent_cfg.spi_func_mode != SpiModeGeneric) continue;
      a = eg2_spi_device_item::type_id::create("act");
      a.kind = eg2_spi_device_item::SpidGeneric;
      a.data = it.data;
      a.pred = 1'b0;
      e = eg2_spi_device_item::type_id::create("exp");
      e.kind = eg2_spi_device_item::SpidGeneric;
      e.data = it.data;
      e.pred = 1'b0;
      e.what = "generic mode";
      not_predicted("generic mode");
      tl_exp_port.write(e);
      pin_port.write(a);
    end
  endtask

  protected task host_thread();
    forever begin
      spi_item it;
      host_fifo.get(it);
      if (agent_cfg == null) continue;
      case (agent_cfg.spi_func_mode)
        SpiModeFlash: flash_txn(it);
        SpiModeTpm:   tpm_txn(it);
        default: ;
      endcase
      if (data_t0.exists(it)) data_t0.delete(it);
    end
  endtask

  protected function int unsigned matched_slot(bit [7:0] op);
    int unsigned slot = 24;
    for (int i = 0; i < 24; i++) if (cmd_info[i][31] && cmd_info[i][7:0] == op) slot = i;
    return slot;
  endfunction

  protected function void flash_txn(spi_item it);
    eg2_spi_device_item a = eg2_spi_device_item::type_id::create("act");
    eg2_spi_device_item e = eg2_spi_device_item::type_id::create("exp");
    int unsigned        slot = matched_slot(it.opcode);
    realtime            t0 = data_t0.exists(it) ? data_t0[it] : $realtime;
    realtime            per;
    int unsigned        n;
    bit                 hw_cmd = 0;
    a.kind = eg2_spi_device_item::SpidFlash;
    a.opcode = it.opcode;
    a.addr_bytes = it.address_q.size();
    foreach (it.address_q[i]) a.addr = {a.addr[23:0], it.address_q[i]};
    a.write = it.write_command;
    a.data = it.payload_q;
    e.kind = a.kind;
    e.opcode = a.opcode;
    e.addr = a.addr;
    e.addr_bytes = a.addr_bytes;
    e.write = a.write;
    n = it.payload_q.size();
    per = n > 0 ? ($realtime - t0) / n : 0;

    // HW-handled single-byte commands: EN4B, EX4B, WREN, WRDI (flash and passthrough).
    if (mode != 0) begin
      if (cmd_en4b[31] && cmd_en4b[7:0] == it.opcode) begin
        addr_4b = 1'b1;
        hw_cmd = 1;
      end else if (cmd_ex4b[31] && cmd_ex4b[7:0] == it.opcode) begin
        addr_4b = 1'b0;
        hw_cmd = 1;
      end else if (cmd_wren[31] && cmd_wren[7:0] == it.opcode) begin
        flash_status[1] = 1'b1;
        hw_cmd = 1;
      end else if (cmd_wrdi[31] && cmd_wrdi[7:0] == it.opcode) begin
        flash_status[1] = 1'b0;
        hw_cmd = 1;
      end
    end

    if (mode == 0) begin
      e.pred = 1'b0;
      e.what = "spi_device disabled";
    end else if (hw_cmd) begin
      e.what = "4B/WEL command";
      e.pred = 1'b0;
    end else if (slot <= 2 && (mode == 1 || intercept[0])) begin
      e.what = $sformatf("status %0d", slot + 1);
      for (int k = 0; k < n; k++) begin
        e.data.push_back(flash_status[8 * slot +: 8]);
        e.care.push_back(status_unsure[8 * slot +: 8] == 0);
      end
      // The software changes reach the host by this read: predicted from the next one.
      status_unsure[8 * slot +: 8] = '0;
    end else if (slot == 3 && (mode == 1 || intercept[1])) begin
      e.what = "jedec";
      for (int k = 0; k < n; k++) begin
        bit [7:0] v;
        bit       c = 1'b1;
        if (k < jedec_num_cc) v = jedec_cc;
        else if (k == jedec_num_cc) v = jedec_id[23:16];
        else if (k == jedec_num_cc + 1) v = jedec_id[7:0];
        else if (k == jedec_num_cc + 2) v = jedec_id[15:8];
        else c = 1'b0;
        e.data.push_back(v);
        e.care.push_back(c);
      end
    end else if (slot == 4 && (mode == 1 || intercept[2])) begin
      e.what = "sfdp";
      for (int k = 0; k < n; k++) begin
        bit [7:0] v;
        bit       c = egress.at(32'hc00 + ((a.addr + k) & 32'hff), t0 + (k + 0.5) * per,
                                margin + 2 * per, v);
        e.data.push_back(v);
        e.care.push_back(c);
      end
    end else if (slot >= 5 && slot <= 10) begin
      bit any_pt = 1'b0;
      e.what = "read buffer";
      for (int k = 0; k < n; k++) begin
        bit [31:0] ad = a.addr + k;
        bit [7:0]  v;
        bit        c;
        bit        mbx = mailbox_en && ad >= mailbox_addr && ad - mailbox_addr < 32'h400;
        if (mbx && (mode == 1 || intercept[3])) begin
          c = egress.at(32'h800 + (ad - mailbox_addr), t0 + (k + 0.5) * per, margin + 2 * per, v);
          e.what = "mailbox";
        end else if (mode == 1) begin
          c = egress.at(ad & 32'h7ff, t0 + (k + 0.5) * per, margin + 2 * per, v);
        end else begin
          c = 1'b0;
          any_pt = 1'b1;
        end
        e.data.push_back(v);
        e.care.push_back(c);
      end
      if (any_pt) begin
        e.what = {e.what, ", passthrough"};
        not_predicted("passthrough read");
      end
    end else if (mode == 2) begin
      e.pred = 1'b0;
      e.what = "passthrough";
    end else begin
      // Flash mode, no hardware submodule: MISO is not defined.
      e.pred = 1'b0;
      e.what = slot < 24 ? "flash, software command" : "flash, no command entry";
    end

    // Upload (CMD_INFO_11..23 with upload set), in flash and passthrough mode: the opcode and
    // address went to the FIFOs at the address phase's end (flash_req); the payload now.
    if (mode != 0 && slot >= 11 && slot < 24 && cmd_info[slot][24]) begin
      if (it.write_command) begin
        payload.delete();
        foreach (it.payload_q[k]) if (k < 256) payload[k] = it.payload_q[k];
      end
      if (cmd_info[slot][25]) flash_status[0] = 1'b1;
      if (e.what == "") e.what = "upload";
      if (spid_cg != null) spid_cg.sample(4, n);
    end

    if (!e.pred) not_predicted(e.what);
    else if (spid_cg != null) spid_cg.sample(slot <= 2 ? 0 : slot == 3 ? 1 : slot == 4 ? 2 : 3, n);
    // Only what the chip sent is compared: a read with payload bytes.
    // The actual item says whether the bench predicts it (+sb_corrupt picks predicted ones).
    a.pred = e.pred && (1'b1 inside {e.care});
    if (!it.write_command && n > 0) tl_exp_port.write(e);
    pin_port.write(a);
  endfunction

  // TPM over SPI: a return-by-HW register (FIFO mode, hw_reg_dis clear) at addr[11:0].
  protected function bit tpm_hw_reg(bit [23:0] ad);
    bit [11:0] o = ad[11:0];
    if (tpm_cfg[2] || tpm_cfg[1]) return 1'b0;
    // Without tpm_reg_chk_dis, only addresses 0xD4_XXXX are TPM registers.
    if (!tpm_cfg[3] && ad[23:16] != 8'hd4) return 1'b0;
    return o[11:2] inside {10'h000, 10'h002, 10'h003, 10'h004, 10'h005, 10'h006, 10'h3c0, 10'h3c1};
  endfunction

  protected function void tpm_txn(spi_item it);
    eg2_spi_device_item a = eg2_spi_device_item::type_id::create("act");
    eg2_spi_device_item e = eg2_spi_device_item::type_id::create("exp");
    bit [23:0] ad;
    bit [7:0]  cmd;
    int unsigned n = it.data.size();
    foreach (it.address_q[i]) ad = {ad[15:0], it.address_q[i]};
    cmd = {it.write_command ? 1'b0 : 1'b1, 1'b0, 6'(n - 1)};
    a.kind = eg2_spi_device_item::SpidTpm;
    a.write = it.write_command;
    a.addr = ad;
    a.addr_bytes = 3;
    a.data = it.data;
    e.kind = a.kind;
    e.write = a.write;
    e.addr = ad;
    e.addr_bytes = 3;
    tpm_rd_active = 1'b0;
    if (!tpm_cfg[0]) begin
      e.pred = 1'b0;
      e.what = "TPM disabled";
    end else if (!it.write_command && tpm_cfg[4] && ad[15:12] > 4) begin
      e.what = "invalid locality";
      repeat (n) begin
        e.data.push_back(8'hff);
        e.care.push_back(1'b1);
      end
    end else if (!it.write_command && tpm_hw_reg(ad)) begin
      e.pred = 1'b0;
      e.what = "TPM return-by-HW register";
    end else begin
      // Uploaded: a write's command and address go to TPM_CMD_ADDR with its last data bit (a
      // read's went there at its address phase, tpm_req).
      if (it.write_command) begin
        tpm_cmdaddr.push_back({cmd, ad});
        tpm_wr.delete();
        foreach (it.data[k]) if (k < 64) tpm_wr[k] = it.data[k];
      end else begin
        e.what = "TPM read FIFO";
        for (int k = 0; k < n; k++) begin
          if (tpm_rdfifo.size() > 0 && !tpm_rdfifo_unsure) begin
            e.data.push_back(tpm_rdfifo.pop_front());
            e.care.push_back(1'b1);
          end else begin
            e.data.push_back(8'h00);
            e.care.push_back(1'b0);
          end
        end
        // The read FIFO serves one command: what is left (a word's padding) goes with it, and
        // software's writes outside a read command are dropped (INTR_STATE.tpm_rdfifo_drop).
        tpm_rdfifo.delete();
      end
    end
    if (spid_cg != null) spid_cg.sample(it.write_command ? 6 : 5, n);
    if (!e.pred) not_predicted(e.what);
    // The actual item says whether the bench predicts it (+sb_corrupt picks predicted ones).
    a.pred = e.pred && (1'b1 inside {e.care});
    if (!it.write_command && n > 0) tl_exp_port.write(e);
    pin_port.write(a);
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// usbdev (hw/ip/usbdev: README.md, doc/theory_of_operation.md "Buffer Interface", "USB Link
// State", doc/registers.md, data/usbdev.hjson) beside the DPI USB host.
//
// Item (eg2_usb_item): each packet on the bus (eg2_usb_monitor: PID, token address and endpoint,
// the payload of a data packet); the chip's packets are compared, the host's are logged.
// Expected, decided when the host's packet that asks for an answer ends:
//   IN token to the device's address (USBCTRL.device_address, zeroed by a bus reset) on an
//   endpoint enabled in EP_IN_ENABLE: STALL if IN_STALL; else the data of CONFIGIN_<ep>.buffer,
//   CONFIGIN.size bytes (64 at most), as software wrote the packet buffer (or the host's earlier
//   OUT data the model put there), if CONFIGIN.rdy; else NAK. The host's ACK clears rdy and sets
//   IN_SENT; a SETUP to the endpoint or a bus reset cancels rdy.
//   OUT/SETUP data to the device's address on an endpoint enabled in EP_OUT_ENABLE: SETUP is ACKed
//   when RXENABLE_SETUP is set, a buffer is in the Available SETUP Buffer FIFO and the Received
//   Buffer FIFO has room (else ignored) and clears both STALL bits; OUT is STALLed with OUT_STALL,
//   NAKed without RXENABLE_OUT or a buffer or room (the last RX FIFO entry is kept for SETUP), else
//   ACKed (SET_NAK_OUT then clears RXENABLE_OUT); an isochronous OUT endpoint is not answered.
//   An accepted packet takes the next available buffer: the model puts its data there and an entry
//   {endpoint, SETUP, size, buffer} into the Received Buffer FIFO.
// A register write or RXFIFO read within `race` of the decision makes NAK acceptable too.
// RX: each RXFIFO read against the model's next entry; each packet-buffer word the core reads
// against the model's copy of that buffer.
// NOT PREDICTED: the data toggle (DATA0 and DATA1 are both a data packet), isochronous IN
// endpoints, a token while the link is not active (before the first bus reset), packets with a bad
// PID check or CRC (logged).
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_usbdev_monitor extends eg2_usb_monitor;
  `uvm_component_utils(eg2_usbdev_monitor)

  localparam int unsigned NEp = usbdev_reg_pkg::NEndpoints;

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_analysis_port #(eg2_usb_item)    tl_exp_port;
  uvm_analysis_port #(eg2_usb_item)    rx_exp_port;
  uvm_analysis_port #(eg2_usb_item)    rx_rd_port;
  virtual tl_if                        tl_vif;
  string                               inst = "usbdev";
  realtime                             race = 2us;

  typedef struct {
    bit [3:0] ep;
    bit       setup;
    int       size;
    bit [4:0] buffer;
    bit       unsure;
  } rx_entry_t;

  // Registers.
  protected bit [6:0]     dev_addr;
  protected bit           usb_enable;
  protected bit [NEp-1:0] ep_out_en, ep_in_en, rxen_setup, rxen_out, set_nak_out, out_stall,
                          in_stall, out_iso, in_iso, in_sent;
  protected bit [31:0]    configin[NEp];
  protected bit [4:0]     av_out[$], av_setup[$];
  protected rx_entry_t    rx_fifo[$];
  // The packet buffer: software's writes and the received data.
  protected bit [7:0]     mem[int];
  protected bit           mem_unsure[int];
  protected realtime      last_cfg;
  // The transaction in progress.
  protected eg2_usb_item  tok;
  protected bit           in_data_sent;   // the chip answered the last IN with data
  protected bit [3:0]     in_data_ep;
  protected bit           link_active;
  eg2_usbdev_xfer_cg      xfer_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo     = new("tl_fifo", this);
    tl_exp_port = new("tl_exp_port", this);
    rx_exp_port = new("rx_exp_port", this);
    rx_rd_port  = new("rx_rd_port", this);
    reset_model();
  endfunction

  function void create_cov(string path);
    xfer_cg = new({path, "::eg2_usbdev_xfer_cg"});
  endfunction

  protected function void reset_model();
    dev_addr = '0;
    usb_enable = 1'b0;
    ep_out_en = '0;
    ep_in_en = '0;
    rxen_setup = '0;
    rxen_out = '0;
    set_nak_out = '0;
    out_stall = '0;
    in_stall = '0;
    out_iso = '0;
    in_iso = '0;
    in_sent = '0;
    foreach (configin[i]) configin[i] = '0;
    av_out.delete();
    av_setup.delete();
    rx_fifo.delete();
    mem.delete();
    mem_unsure.delete();
    tok = null;
    in_data_sent = 1'b0;
    link_active = 1'b0;
  endfunction

  virtual task run_phase(uvm_phase phase);
    fork
      super.run_phase(phase);
      if (tl_vif != null) begin
        fork
          tl_thread();
          forever begin
            @(negedge tl_vif.rst_n);
            reset_model();
            marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, "usbdev reset");
            marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, "usbdev reset");
          end
        join
      end
    join
  endtask

  protected function void marker(uvm_analysis_port #(eg2_usb_item) port,
                                 eg2_periph_marker::kind_e kind, int keep, string why);
    eg2_usb_item m = eg2_usb_item::type_id::create("marker");
    eg2_periph_marker::mark(m, kind, keep, why);
    port.write(m);
  endfunction

  // ----- The register traffic -----

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      if (t.d_error || eg2_tl_err::refused(inst, t)) continue;
      off = eg2_tl_offset(t, 32'h1000);
      if (t.is_write()) tl_write(off, t);
      else tl_read(off, t);
    end
  endtask

  protected function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    if (off >= usbdev_reg_pkg::USBDEV_BUFFER_OFFSET &&
        off < usbdev_reg_pkg::USBDEV_BUFFER_OFFSET + usbdev_reg_pkg::USBDEV_BUFFER_SIZE) begin
      int base = off - usbdev_reg_pkg::USBDEV_BUFFER_OFFSET;
      for (int b = 0; b < 4; b++) begin
        if (!t.a_mask[b]) continue;
        mem[base + b] = d[8 * b +: 8];
        mem_unsure[base + b] = 1'b0;
      end
      return;
    end
    last_cfg = $realtime;
    case (off)
      usbdev_reg_pkg::USBDEV_USBCTRL_OFFSET: begin
        dev_addr = d[22:16];
        usb_enable = d[0];
      end
      usbdev_reg_pkg::USBDEV_EP_OUT_ENABLE_OFFSET: ep_out_en = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_EP_IN_ENABLE_OFFSET: ep_in_en = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_AVOUTBUFFER_OFFSET: if (av_out.size() < 8) av_out.push_back(d[4:0]);
      usbdev_reg_pkg::USBDEV_AVSETUPBUFFER_OFFSET: if (av_setup.size() < 4) av_setup.push_back(d[4:0]);
      usbdev_reg_pkg::USBDEV_RXENABLE_SETUP_OFFSET: rxen_setup = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_RXENABLE_OUT_OFFSET: begin
        // Only the bits whose preserve bit is 0 are written.
        for (int e = 0; e < NEp; e++) if (!d[16 + e]) rxen_out[e] = d[e];
      end
      usbdev_reg_pkg::USBDEV_SET_NAK_OUT_OFFSET: set_nak_out = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_IN_SENT_OFFSET: in_sent &= ~d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_OUT_STALL_OFFSET: out_stall = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_IN_STALL_OFFSET: in_stall = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_OUT_ISO_OFFSET: out_iso = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_IN_ISO_OFFSET: in_iso = d[NEp-1:0];
      usbdev_reg_pkg::USBDEV_FIFO_CTRL_OFFSET: begin
        if (d[0]) av_out.delete();
        if (d[1]) av_setup.delete();
        if (d[2]) rx_fifo.delete();
      end
      default: begin
        if (off >= usbdev_reg_pkg::USBDEV_CONFIGIN_0_OFFSET &&
            off <= usbdev_reg_pkg::USBDEV_CONFIGIN_11_OFFSET) begin
          int e = (off - usbdev_reg_pkg::USBDEV_CONFIGIN_0_OFFSET) / 4;
          // rdy, size, buffer as written; pend and sending are write-1-to-clear status.
          configin[e] = {d[31], configin[e][30] & ~d[30], configin[e][29] & ~d[29], 14'h0,
                         d[14:8], 3'h0, d[4:0]};
        end
      end
    endcase
  endfunction

  protected function void rx(eg2_usb_item e, eg2_usb_item a);
    rx_exp_port.write(e);
    rx_rd_port.write(a);
  endfunction

  protected function void tl_read(bit [31:0] off, tl_seq_item t);
    if (off == usbdev_reg_pkg::USBDEV_RXFIFO_OFFSET) begin
      eg2_usb_item a = eg2_usb_item::type_id::create("rx_rd");
      eg2_usb_item e;
      last_cfg = $realtime;
      a.kind = eg2_usb_item::UsbRxEntry;
      a.endpoint = t.d_data[23:20];
      a.setup = t.d_data[19];
      a.size = t.d_data[14:8];
      a.buffer = t.d_data[4:0];
      // An entry the model was unsure of may be missing.
      while (rx_fifo.size() > 0 && rx_fifo[0].unsure &&
             !(rx_fifo[0].ep == a.endpoint && rx_fifo[0].setup == a.setup &&
               rx_fifo[0].size == a.size)) begin
        void'(rx_fifo.pop_front());
      end
      if (rx_fifo.size() == 0) return;  // read while the model has nothing: not compared
      e = eg2_usb_item::type_id::create("rx_exp");
      e.kind = eg2_usb_item::UsbRxEntry;
      e.endpoint = rx_fifo[0].ep;
      e.setup = rx_fifo[0].setup;
      e.size = rx_fifo[0].size;
      e.buffer = rx_fifo[0].buffer;
      void'(rx_fifo.pop_front());
      rx(e, a);
    end else if (off >= usbdev_reg_pkg::USBDEV_BUFFER_OFFSET &&
                 off < usbdev_reg_pkg::USBDEV_BUFFER_OFFSET + usbdev_reg_pkg::USBDEV_BUFFER_SIZE) begin
      eg2_usb_item a = eg2_usb_item::type_id::create("rx_rd");
      eg2_usb_item e = eg2_usb_item::type_id::create("rx_exp");
      int          base = (off - usbdev_reg_pkg::USBDEV_BUFFER_OFFSET) & ~3;
      bit [31:0]   c = '0;
      for (int b = 0; b < 4; b++) begin
        if (mem.exists(base + b) && !mem_unsure[base + b]) begin
          e.word[8 * b +: 8] = mem[base + b];
          c[8 * b +: 8] = 8'hff;
        end
      end
      if (c == 0) return;
      a.kind = eg2_usb_item::UsbBufRead;
      a.offset = base;
      a.word = t.d_data;
      e.kind = eg2_usb_item::UsbBufRead;
      e.offset = base;
      e.care = c;
      e.word &= c;
      rx(e, a);
    end
  endfunction

  // ----- The bus -----

  protected function eg2_usb_item exp_packet(bit [3:0] pid, string why, bit loose);
    eg2_usb_item e = eg2_usb_item::type_id::create("exp");
    e.kind = eg2_usb_item::UsbPacket;
    e.from_chip = 1'b1;
    e.pid = pid;
    e.pid_ok = 1'b1;
    e.why = why;
    e.loose = loose;
    if (loose) eg2_periph_marker::mark(e, eg2_periph_marker::MarkOptional, 0, "register write raced");
    return e;
  endfunction

  protected function void expect_packet(eg2_usb_item e, int unsigned kind, bit [3:0] ep);
    tl_exp_port.write(e);
    if (xfer_cg != null) xfer_cg.sample(kind, e.pid, ep, e.data.size());
  endfunction

  // Each item of the bus: the host's tokens and data decide the expected answer.
  virtual function void on_item(eg2_usb_item it);
    bit loose = $realtime - last_cfg < race;
    case (it.kind)
      eg2_usb_item::UsbReset: begin
        // A bus reset: the address is zeroed, waiting IN transactions are cancelled (pend).
        dev_addr = '0;
        link_active = 1'b1;
        for (int e = 0; e < NEp; e++) begin
          if (configin[e][31]) configin[e][30] = 1'b1;
          configin[e][31] = 1'b0;
        end
        tok = null;
      end
      eg2_usb_item::UsbDisconnect: link_active = 1'b0;
      eg2_usb_item::UsbPacket: begin
        if (!it.pid_ok) return;
        if (it.from_chip) begin
          in_data_sent = tok != null && tok.pid == 4'h9 && it.is_data();
          if (in_data_sent) in_data_ep = tok.endpoint;
          if (!(tok != null && tok.pid == 4'h9)) tok = null;
          return;
        end
        case (it.pid)
          4'h9: begin  // IN
            tok = it;
            in_data_sent = 1'b0;
            if (!link_active || it.address != dev_addr || !ep_in_en[it.endpoint]) return;
            if (in_iso[it.endpoint]) begin
              tok = null;
              return;
            end
            if (in_stall[it.endpoint]) begin
              expect_packet(exp_packet(4'he, "IN_STALL", loose), 0, it.endpoint);
            end else if (configin[it.endpoint][31]) begin
              eg2_usb_item e = exp_packet(4'h3, $sformatf("CONFIGIN_%0d buffer %0d", it.endpoint,
                                                          configin[it.endpoint][4:0]), loose);
              int size = configin[it.endpoint][14:8] > 64 ? 64 : configin[it.endpoint][14:8];
              int base = 64 * configin[it.endpoint][4:0];
              bit unsure = 1'b0;
              for (int k = 0; k < size; k++) begin
                bit known = mem.exists(base + k) && !mem_unsure[base + k];
                e.data.push_back(mem.exists(base + k) ? mem[base + k] : 8'h00);
                e.data_care.push_back(known);
                if (!known) unsure = 1'b1;
              end
              // Bytes the model does not know are not compared (`--` in the log).
              if (unsure) e.why = {e.why, ", some bytes not known"};
              configin[it.endpoint][29] = 1'b1;  // sending
              expect_packet(e, 0, it.endpoint);
            end else begin
              expect_packet(exp_packet(4'ha, "not rdy", loose), 0, it.endpoint);
            end
          end
          4'h1, 4'hd: begin  // OUT, SETUP: the answer follows the data packet
            tok = it;
          end
          4'h2: begin  // ACK from the host: an IN data packet was delivered
            if (in_data_sent) begin
              configin[in_data_ep][31] = 1'b0;
              configin[in_data_ep][29] = 1'b0;
              in_sent[in_data_ep] = 1'b1;
            end
            in_data_sent = 1'b0;
            tok = null;
          end
          4'h3, 4'hb: begin  // DATA from the host
            if (tok != null && tok.pid inside {4'h1, 4'hd}) host_data(tok, it, loose);
            tok = null;
          end
          default: ;
        endcase
      end
      default: ;
    endcase
  endfunction

  protected function void host_data(eg2_usb_item tk, eg2_usb_item it, bit loose);
    bit [3:0] ep = tk.endpoint;
    bit       setup = tk.pid == 4'hd;
    if (!link_active || tk.address != dev_addr || !ep_out_en[ep]) return;
    if (setup) begin
      if (!rxen_setup[ep] || av_setup.size() == 0 || rx_fifo.size() >= 8) begin
        // Ignored: no handshake. A raced register write may have made it accepted.
        if (loose) begin
          eg2_usb_item e = exp_packet(4'h2, "SETUP may be accepted", 1'b1);
          expect_packet(e, 1, ep);
        end
        return;
      end
      expect_packet(exp_packet(4'h2, "SETUP accepted", loose), 1, ep);
      receive(ep, 1'b1, it.data, av_setup.pop_front(), loose);
      in_stall[ep] = 1'b0;
      out_stall[ep] = 1'b0;
      if (configin[ep][31]) configin[ep][30] = 1'b1;
      configin[ep][31] = 1'b0;
      return;
    end
    if (out_iso[ep]) begin
      if (rxen_out[ep] && av_out.size() > 0 && rx_fifo.size() < 7) begin
        receive(ep, 1'b0, it.data, av_out.pop_front(), 1'b1);
      end
      return;
    end
    if (out_stall[ep]) begin
      expect_packet(exp_packet(4'he, "OUT_STALL", loose), 2, ep);
    end else if (!rxen_out[ep] || av_out.size() == 0 || rx_fifo.size() >= 7) begin
      expect_packet(exp_packet(4'ha, !rxen_out[ep] ? "RXENABLE_OUT clear" :
                                     av_out.size() == 0 ? "no OUT buffer" : "RX FIFO full", loose),
                    2, ep);
    end else begin
      expect_packet(exp_packet(4'h2, "OUT accepted", loose), 2, ep);
      receive(ep, 1'b0, it.data, av_out.pop_front(), loose);
      if (set_nak_out[ep]) rxen_out[ep] = 1'b0;
    end
  endfunction

  // An accepted packet: its data into the buffer, an entry into the Received Buffer FIFO.
  protected function void receive(bit [3:0] ep, bit setup, bit [7:0] data[$], bit [4:0] buf_id,
                                  bit unsure);
    rx_entry_t r;
    r.ep = ep;
    r.setup = setup;
    r.size = data.size();
    r.buffer = buf_id;
    r.unsure = unsure;
    foreach (data[k]) begin
      if (k >= 64) break;
      mem[64 * buf_id + k] = data[k];
      mem_unsure[64 * buf_id + k] = unsure;
    end
    rx_fifo.push_back(r);
  endfunction
endclass
