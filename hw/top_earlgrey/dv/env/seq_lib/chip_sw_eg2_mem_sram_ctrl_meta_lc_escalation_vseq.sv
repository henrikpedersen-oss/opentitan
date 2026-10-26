// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_sram_ctrl_meta_lc_escalation (sw/device/tests/eg2_mem/eg2_sram_ctrl_meta_lc_escalation.c;
// EG2 spec REQ_CMS_14, REQ_SEC_18): OpenTitan's chip_sw_sram_lc_escalation procedure on
// sram_ctrl_meta. The escalation comes from the software's alert through the alert handler and
// lc_ctrl; nothing here forces it. The bench's half, at the software's DV log lines:
//   EG2 MEM metaesc ready <w> <fails>      the controller holds a key from OTP (not its netlist
//                                          constant, so the escalation's key reset shows); RAM
//                                          word w and the key taken
//   EG2 MEM metaesc escalate <fails>       <fails> != 0: boot 1's checks failed (the core stops
//                                          before it can report them). Then, once lc_ctrl's
//                                          lc_escalate_en reaches sram_ctrl_meta (On or any value
//                                          but Off, interfaces.md): STATUS.ESCALATED; key and
//                                          nonce reset to RndCnstSramKey / RndCnstSramNonce;
//                                          SCR_KEY_ROTATED MuBi4False (registers.md); a read and a
//                                          write of word w put on the meta SRAM's bus port
//                                          (cheriot_mem_sys_meta_sram_tl_req, while it is idle)
//                                          are answered with d_error by the controller's TL-UL
//                                          gate, neither reaches the TL-UL adapter or the memory
//                                          macro, word w is unchanged (no tag set), and the probe's
//                                          integrity was good (no BUS_INTEG_ERROR). Then
//                                          boot_phase = 1 and a power-on reset.
//   EG2 MEM metaesc renew begin / done     (RISC-V mode) the key changed and is not the netlist
//                                          constant
//   EG2 MEM metaesc init done              (RISC-V mode) sampled RAM words written (no X)
// The probe's responses go to the bench only: the port's d_valid towards cheriot_mem_sys is held
// at 0 meanwhile (the host made no request), and the reset that follows clears the outstanding
// count the gate keeps. The probe is announced as a deliberate stimulus.
class chip_sw_eg2_sram_ctrl_meta_lc_escalation_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_ctrl_meta_lc_escalation_vseq)

  `uvm_object_new

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string MetaHier = {PdMain, ".u_sram_ctrl_meta"};
  localparam string MetaMem = {MetaHier, ".u_prim_ram_1p_scr.u_prim_ram_1p_adv.gen_ram_inst[0]",
                               ".u_mem.mem"};
  localparam string MetaRamReq = {MetaHier, ".u_prim_ram_1p_scr.u_prim_ram_1p_adv.req_i"};
  localparam string MetaKey = {MetaHier, ".key_q"};
  localparam string MetaNonce = {MetaHier, ".nonce_q"};
  localparam string MetaLcEsc = {MetaHier, ".lc_escalate_en_i"};
  localparam string MetaEscalated = {MetaHier, ".u_reg_regs.status_escalated_qs"};
  localparam string MetaBusInteg = {MetaHier, ".u_reg_regs.status_bus_integ_error_qs"};
  localparam string MetaKeyRotated = {MetaHier, ".u_reg_regs.scr_key_rotated_qs"};
  localparam string GateErr = {MetaHier, ".u_tlul_lc_gate.tl_d2h_error"};
  localparam string AdapterIn = {MetaHier, ".ram_tl_in_gated"};
  localparam string PortReq = {PdMain, ".cheriot_mem_sys_meta_sram_tl_req"};
  localparam string PortRsp = {PdMain, ".cheriot_mem_sys_meta_sram_tl_rsp"};
  localparam int unsigned MetaWords = 38912 / 4;
  // How long lc_escalate_en may take from the alert to the controller, and the probe's patience.
  localparam int unsigned EscTimeoutClks = 200_000;
  localparam int unsigned ProbeClks = 200;

  rand int unsigned esc_signal;
  rand int unsigned esc_phase;
  rand int unsigned n_words;

  // Both of lc_ctrl's escalation inputs; the escalating phase uniform (the ones before it are
  // short and send nothing).
  constraint esc_c {
    esc_signal inside {[1:2]};
    esc_phase inside {[0:3]};
  }
  constraint n_words_c {
    n_words dist {[16:64] :/ 50, [65:256] :/ 50};
  }

  virtual function void write_params();
    set_param("esc_signal", esc_signal);
    set_param("esc_phase", esc_phase);
    set_param("n_words", n_words);
    set_param("boot_phase", 0);
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), $sformatf("cannot read %s", path))
    return v;
  endfunction

  protected function void frc(string path, uvm_hdl_data_t v);
    `DV_CHECK_FATAL(uvm_hdl_force(path, v), $sformatf("cannot force %s", path))
  endfunction

  protected function void rel(string path);
    `DV_CHECK_FATAL(uvm_hdl_release(path), $sformatf("cannot release %s", path))
  endfunction

  protected function bit [38:0] raw(int unsigned w);
    uvm_hdl_data_t v = rd($sformatf("%s[%0d]", MetaMem, w));
    return v[38:0];
  endfunction

  virtual task body();
    string paths[$] = '{MetaMem, MetaRamReq, MetaKey, MetaNonce, MetaLcEsc, MetaEscalated,
                        MetaBusInteg, MetaKeyRotated, GateErr, AdapterIn, PortReq, PortRsp};
    foreach (paths[i]) begin
      `DV_CHECK_FATAL(uvm_hdl_check_path(paths[i]), {"no such path: ", paths[i]})
    end
    super.body();
    fork
      check_lines();
    join_none
  endtask

  protected task check_lines();
    bit [38:0] snap;
    bit [127:0] key_before;
    int unsigned w, fails;
    forever begin
      string line;
      wait_sw_line("EG2 MEM metaesc ", line, 32'd4_000_000_000);
      if (line == "") return;
      if ($sscanf(line, "EG2 MEM metaesc ready %d %d", w, fails) == 2) begin
        bit [127:0] k = rd(MetaKey);
        `DV_CHECK_LT_FATAL(w, MetaWords, "probe word outside the meta SRAM")
        `DV_CHECK_NE(k, top_earlgrey_rnd_cnst_pkg::RndCnstSramCtrlMetaSramKey,
                     "before the escalation the meta key is still the netlist constant")
        snap = raw(w);
      end else if ($sscanf(line, "EG2 MEM metaesc escalate %d", fails) == 1) begin
        `DV_CHECK_EQ(fails, 0, $sformatf("EG2 MEM metaesc: boot 1 failed %0d check(s)", fails))
        check_escalated(w, snap);
        set_param("boot_phase", 1);
        `uvm_info(`gfn, "EG2 metaesc: escalated; power-on reset into boot 2", UVM_LOW)
        apply_reset();
      end else if (line == "EG2 MEM metaesc renew begin") begin
        key_before = rd(MetaKey);
      end else if (line == "EG2 MEM metaesc renew done") begin
        bit [127:0] k = rd(MetaKey);
        `DV_CHECK_NE(k, key_before, "sram_ctrl_meta kept its old key after the renewal")
        `DV_CHECK_NE(k, top_earlgrey_rnd_cnst_pkg::RndCnstSramCtrlMetaSramKey,
                     "sram_ctrl_meta's key after the renewal is the netlist constant")
      end else if (line == "EG2 MEM metaesc init done") begin
        for (int i = 0; i < 64; i++) begin
          int unsigned r = $urandom_range(0, MetaWords - 1);
          `DV_CHECK(!$isunknown(raw(r)), $sformatf("meta RAM word %0d not written by INIT", r))
        end
      end
      // The next line, not this one again.
      @(cfg.sw_logger_vif.printed_log_event);
    end
  endtask

  // After the software's alert: the escalation at sram_ctrl_meta and what it must do there.
  protected task check_escalated(int unsigned w, bit [38:0] snap);
    lc_ctrl_pkg::lc_tx_t esc;
    otp_ctrl_pkg::sram_key_t key;
    otp_ctrl_pkg::sram_nonce_t nonce;
    bit reached, err, any_rsp, adapter_seen, ram_seen;
    uvm_hdl_data_t v;
    for (int i = 0; i < EscTimeoutClks && !reached; i++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      v = rd(MetaLcEsc);
      esc = lc_ctrl_pkg::lc_tx_t'(v[3:0]);
      reached = esc != lc_ctrl_pkg::Off;
    end
    `DV_CHECK_FATAL(reached, $sformatf(
                    "lc_escalate_en never reached sram_ctrl_meta within %0d cycles of the alert",
                    EscTimeoutClks))
    `uvm_info(`gfn, $sformatf("EG2 metaesc: lc_escalate_en = 0x%0h at sram_ctrl_meta", esc),
              UVM_LOW)
    // prim_lc_sync and the status register.
    cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(10);
    `DV_CHECK_EQ(rd(MetaEscalated) & 1, 1, "STATUS.ESCALATED not set by lc_escalate_en")
    key = rd(MetaKey);
    nonce = rd(MetaNonce);
    `DV_CHECK_EQ(key, top_earlgrey_rnd_cnst_pkg::RndCnstSramCtrlMetaSramKey,
                 "the escalation did not reset the key to RndCnstSramKey")
    `DV_CHECK_EQ(nonce, top_earlgrey_rnd_cnst_pkg::RndCnstSramCtrlMetaSramNonce,
                 "the escalation did not reset the nonce to RndCnstSramNonce")
    `DV_CHECK_EQ(rd(MetaKeyRotated) & 4'hf, prim_mubi_pkg::MuBi4False,
                 "SCR_KEY_ROTATED not cleared by the escalation")
    // A read, then a write of a pattern, of word w.
    for (int k = 0; k < 2; k++) begin
      bit write = k == 1;
      probe(write, w, write ? ~snap[31:0] : 32'h0, any_rsp, err, adapter_seen, ram_seen);
      `DV_CHECK(any_rsp, $sformatf("escalated: no response to the %s of meta word %0d",
                                   write ? "write" : "read", w))
      `DV_CHECK_EQ(err, 1, $sformatf("escalated: the %s of meta word %0d was not refused",
                                     write ? "write" : "read", w))
      `DV_CHECK_EQ(adapter_seen, 0, "escalated: a request passed the TL-UL gate")
      `DV_CHECK_EQ(ram_seen, 0, "escalated: the memory macro was accessed")
    end
    `DV_CHECK_EQ(raw(w), snap, $sformatf("escalated: meta RAM word %0d changed", w))
    `DV_CHECK_EQ(rd(MetaBusInteg) & 1, 0,
                 "the bench's probe had bad integrity (BUS_INTEG_ERROR): the probe proves nothing")
  endtask

  // One TL-UL access of word `w` on the meta SRAM's port, held until accepted. Watches the gate's
  // error responder, the TL-UL adapter's input and the memory macro for ProbeClks cycles.
  protected task probe(bit write, int unsigned w, bit [31:0] wdata, output bit any_rsp,
                       output bit err, output bit adapter_seen, output bit ram_seen);
    tlul_pkg::tl_h2d_t req;
    bit accepted, idle;
    any_rsp = 0;
    err = 0;
    adapter_seen = 0;
    ram_seen = 0;
    // The host must be idle: the forced request replaces whatever it drives.
    for (int i = 0; i < 1000 && !idle; i++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      idle = rd({PortReq, ".a_valid"}) == 0 && rd({PortRsp, ".d_valid"}) == 0;
    end
    `DV_CHECK_FATAL(idle, "the meta SRAM port never idle: cannot probe")
    req = '0;
    req.a_valid = 1'b1;
    req.a_opcode = write ? tlul_pkg::PutFullData : tlul_pkg::Get;
    req.a_size = 2;
    req.a_source = 8'h5a;
    req.a_address = 4 * w;
    req.a_mask = 4'hf;
    req.a_data = wdata;
    req.a_user.instr_type = prim_mubi_pkg::MuBi4False;
    req.a_user.cmd_intg = tlul_pkg::get_cmd_intg(req);
    req.a_user.data_intg = tlul_pkg::get_data_intg(req.a_data);
    req.d_ready = 1'b1;
    // TODO(inject-registry): announce through eg2_inject_registry once it exists.
    announce("chip_sw_eg2_sram_ctrl_meta_lc_escalation_vseq", "sram_ctrl_meta",
             $sformatf("bench %s of meta word %0d on the escalated bus port",
                       write ? "write" : "read", w));
    frc({PortRsp, ".d_valid"}, 0);
    frc({PortReq, ".a_opcode"}, req.a_opcode);
    frc({PortReq, ".a_param"}, req.a_param);
    frc({PortReq, ".a_size"}, req.a_size);
    frc({PortReq, ".a_source"}, req.a_source);
    frc({PortReq, ".a_address"}, req.a_address);
    frc({PortReq, ".a_mask"}, req.a_mask);
    frc({PortReq, ".a_data"}, req.a_data);
    frc({PortReq, ".a_user"}, req.a_user);
    frc({PortReq, ".d_ready"}, 1);
    frc({PortReq, ".a_valid"}, 1);
    for (int i = 0; i < ProbeClks; i++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      if (!accepted && rd({PortRsp, ".a_ready"}) == 1) begin
        // Accepted at the next rising edge; the request is withdrawn after it.
        cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
        #1ps;
        frc({PortReq, ".a_valid"}, 0);
        accepted = 1;
        continue;
      end
      if (rd({AdapterIn, ".a_valid"}) == 1) adapter_seen = 1;
      if (rd(MetaRamReq) == 1) ram_seen = 1;
      if (rd({GateErr, ".d_valid"}) == 1) begin
        any_rsp = 1;
        err = rd({GateErr, ".d_error"});
      end
    end
    rel({PortReq, ".a_valid"});
    rel({PortReq, ".a_opcode"});
    rel({PortReq, ".a_param"});
    rel({PortReq, ".a_size"});
    rel({PortReq, ".a_source"});
    rel({PortReq, ".a_address"});
    rel({PortReq, ".a_mask"});
    rel({PortReq, ".a_data"});
    rel({PortReq, ".a_user"});
    rel({PortReq, ".d_ready"});
    rel({PortRsp, ".d_valid"});
    `DV_CHECK(accepted, $sformatf("the %s of meta word %0d was never accepted",
                                  write ? "write" : "read", w))
  endtask

endclass : chip_sw_eg2_sram_ctrl_meta_lc_escalation_vseq
