// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rv_dm against the debug module reference model (hw/top_earlgrey/dv/eg2_model/ip/eg2_dm_core.h,
// through eg2_dm_cmodel_pkg). On with +rv_dm_cmodel=1 (cfg.en_cmodel), next to rv_dm_scoreboard.
//
// Every DMI transaction the DM took (rsp_op Ok), every access of the core's TL-UL agent to the debug
// memory and every system bus response goes to the model as it is seen; the model predicts:
//   DMI read data, under the mask of bits it predicts (a data or program buffer read while an
//     abstract command is busy is not predicted);
//   debug-memory read data of data, program buffer and flags (WhereTo, AbstractCmd, the ROM: no);
//   each system bus request (read/write, address, size, byte lanes, write data);
//   debug_req, ndmreset_req and dmactive, settle_cycles after the last transaction.
// Life cycle comes from the mode agent's inputs and LATE_DEBUG_ENABLE's mirror, by rv_dm
// theory_of_operation.md "Late Debug Enable Mechanism". Input combinations that are no life-cycle
// state of that table (lc_dft_en On with lc_hw_debug_en not On; pinmux_hw_debug_en unlike
// lc_hw_debug_en) are not compared: the model keeps running and the run counts them; the model is
// then trusted again from the next reset.
class rv_dm_cmodel_sb extends uvm_component;
  `uvm_component_utils(rv_dm_cmodel_sb)

  typedef struct {
    bit          write;
    bit [31:0]   addr;
    int unsigned size;
    bit [31:0]   wdata;
  } sba_req_t;
  typedef struct {
    bit        write;
    bit [31:0] addr;
    bit [3:0]  mask;
    bit [31:0] data;
    bit        compared;
    bit        d_seen;
    bit [31:0] d_data;
    bit        d_error;
  } rtl_sba_t;
  protected rtl_sba_t rtl_sba_q[$];

  rv_dm_env_cfg cfg;
  int unsigned  settle_cycles = 20;  // the outputs are compared this many clk cycles
  int unsigned  settle_tck = 8;      // plus this many TCK periods after the last input
  // +rv_dm_cmodel_corrupt=1: the first predicted DMI read is wrong in bit 0, so the run must fail.
  bit           corrupt;
  // +rv_dm_cmodel_mask_known=1: the RTL findings already filed (dmcontrol haltreq/resumereq read
  // back; sbdata0 after a bus write; sbcs bits 28:23 read back; abstractcs.busy while a resume is
  // pending) are counted, not failed, so a run goes on to what is left.
  bit           mask_known;
  int unsigned  hist_len = 24;  // +rv_dm_cmodel_history: model inputs kept for a mismatch report

  uvm_tlm_analysis_fifo #(jtag_dmi_item) dmi_req_fifo;  // DMI requests, as they go to the DM
  uvm_tlm_analysis_fifo #(jtag_dmi_item) dmi_fifo;      // the same, with the response
  uvm_tlm_analysis_fifo #(tl_seq_item)   mem_d_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)   sba_a_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)   sba_d_fifo;
  uvm_tlm_analysis_fifo #(jtag_item)     jtag_fifo;     // every JTAG scan, for the DTM coverage

  // Field coverage of the registers (rv_dm_reg_cov), with coverage enabled (cfg.en_cov).
  rv_dm_reg_cov cov;

  protected chandle   h;
  protected sba_req_t exp_sba_q[$];       // started by the model, not yet seen on the bus

  protected bit       trusted = 1'b1;     // 0 after an undocumented life-cycle input combination
  protected int unsigned n_events;
  protected jtag_dmi_item pend_item;
  protected int unsigned  pend_data, pend_mask;
  protected bit           pend_ok, pend_cmp;
  protected bit           pend_cheriot;  // CHERIoT mode at the pending request (coverage)

  // Counts for the report.
  protected int unsigned n_dmi_cmp, n_mem_cmp, n_sba_cmp, n_out_cmp, n_mismatch;
  protected int unsigned n_not_predicted, n_gated, n_dtm_not_ok, n_untrusted, n_mem_err;
  protected int unsigned n_known_haltreq, n_known_sbdata, n_known_sbcs, n_known_busy_resume;
  protected bit          resume_pending;  // resumereq written, the core has not written Resuming yet
  protected bit          resumeack_known; // resumereq written with haltreq since the last real one
  protected int unsigned n_known_resumeack;
  protected int unsigned n_known_haltsum_unavail, n_known_sbcs_inactive, n_known_sbdata1;
  protected bit          dm_inactive;  // the last dmcontrol write cleared dmactive
  protected bit          last_sba_write;  // the last access the model started was a write
  // last_sba_write at the pending read's request: a read of sbdata0 with sbreadondata starts the next
  // bus read at the request, before its response is compared (2026-11-07, random_cheriot seed 8).
  protected bit          pend_after_sba_write;
  // The time of the last DMI request: rv_dm acts on it only once it has crossed from the JTAG clock
  // into the DM's clock (the settle time of the output checks), the model at once. A debug memory
  // read inside that window is not compared (2026-11-07, random seed 8: the core read the flags
  // 470 ns after a command, before rv_dm had set go).
  protected realtime     last_dmi_req_t = -1.0;
  protected int unsigned n_mem_in_cdc;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    dmi_req_fifo = new("dmi_req_fifo", this);
    dmi_fifo   = new("dmi_fifo", this);
    mem_d_fifo = new("mem_d_fifo", this);
    sba_a_fifo = new("sba_a_fifo", this);
    sba_d_fifo = new("sba_d_fifo", this);
    jtag_fifo  = new("jtag_fifo", this);
    if (cfg.en_cov) cov = rv_dm_reg_cov::type_id::create("rv_dm_reg_cov");
    void'($value$plusargs("rv_dm_cmodel_settle=%d", settle_cycles));
    void'($value$plusargs("rv_dm_cmodel_settle_tck=%d", settle_tck));
    void'($value$plusargs("rv_dm_cmodel_corrupt=%b", corrupt));
    void'($value$plusargs("rv_dm_cmodel_mask_known=%b", mask_known));
    // +rv_dm_cmodel_history=<n>: model inputs kept for the mismatch report (default 24); n=0 also
    // prints every input as it happens (for debugging a run that ends without a mismatch).
    void'($value$plusargs("rv_dm_cmodel_history=%d", hist_len));
    h = eg2_dm_cmodel_pkg::eg2_dm_create();
  endfunction

  task run_phase(uvm_phase phase);
    fork
      watch_reset();
      forever process_dmi_req();
      forever process_dmi();
      forever process_mem();
      forever process_sba_a();
      forever process_sba_d();
      forever process_jtag();
    join_none
  endtask

  // ---- Inputs ----

  protected task watch_reset();
    forever begin
      @(negedge cfg.clk_rst_vif.rst_n);
      eg2_dm_cmodel_pkg::eg2_dm_reset(h);
      exp_sba_q.delete();
      rtl_sba_q.delete();
      trusted = 1'b1;
      dmi_req_fifo.flush();
      dmi_fifo.flush();
      pend_item = null;
      mem_d_fifo.flush();
      sba_a_fifo.flush();
      sba_d_fifo.flush();
      resume_pending = 1'b0;
      jtag_fifo.flush();
      if (cov != null) begin
        cov.reset();
        cov.tap_reset();
      end
    end
  endtask

  // The model's life-cycle inputs; 0 if the inputs are no documented life-cycle state.
  protected function bit update_inputs();
    bit hw   = lc_ctrl_pkg::lc_tx_test_true_strict(cfg.m_mode_agent_cfg.vif.lc_hw_debug_en);
    bit dft  = lc_ctrl_pkg::lc_tx_test_true_strict(cfg.m_mode_agent_cfg.vif.lc_dft_en);
    bit pin  = lc_ctrl_pkg::lc_tx_test_true_strict(cfg.m_mode_agent_cfg.vif.pinmux_hw_debug_en);
    bit late_otp =
        prim_mubi_pkg::mubi8_test_true_strict(cfg.m_mode_agent_cfg.vif.otp_dis_rv_dm_late_debug);
    bit [31:0] late_val = cfg.ral.late_debug_enable.get_mirrored_value();
    bit late_reg = prim_mubi_pkg::mubi32_test_true_strict(prim_mubi_pkg::mubi32_t'(late_val));
    bit full = hw && (dft || late_otp || late_reg);
    eg2_dm_cmodel_pkg::eg2_dm_set_enables(h, full, full);
    eg2_dm_cmodel_pkg::eg2_dm_set_unavailable(h, cfg.rv_dm_vif.unavailable[0] === 1'b1);
`ifdef RV_DM_CHERIOT_SUPPORT
    eg2_dm_cmodel_pkg::eg2_dm_set_cheriot(
        h, prim_mubi_pkg::mubi4_test_true_strict(cfg.m_mode_agent_cfg.vif.cheriot_ena));
`else
    eg2_dm_cmodel_pkg::eg2_dm_set_cheriot(h, 0);
`endif
    if (cov != null) begin
      cov.sample_mode(cfg.m_mode_agent_cfg.vif.lc_hw_debug_en, cfg.m_mode_agent_cfg.vif.lc_dft_en,
                      cfg.m_mode_agent_cfg.vif.pinmux_hw_debug_en,
                      cfg.m_mode_agent_cfg.vif.otp_dis_rv_dm_late_debug,
                      prim_mubi_pkg::mubi32_t'(late_val),
                      cfg.ral.late_debug_enable_regwen.get_mirrored_value() != 0,
                      cfg.m_mode_agent_cfg.vif.cheriot_ena);
    end
    return !(dft && !hw) && (pin == hw);
  endfunction

  // CHERIoT mode as rv_dm selects it (rv_dm ToO "CHERIoT Mode": CheriotSupport and MuBi4True).
  protected function bit cheriot_mode();
`ifdef RV_DM_CHERIOT_SUPPORT
    return prim_mubi_pkg::mubi4_test_true_strict(cfg.m_mode_agent_cfg.vif.cheriot_ena);
`else
    return 1'b0;
`endif
  endfunction

  // Every JTAG scan, for the TAP and dtmcs coverage (rv_dm_reg_cov): an IR scan selects a register,
  // a DR scan of dtmcs gives what was written and what was read.
  protected task process_jtag();
    jtag_item it;
    jtag_fifo.get(it);
    if (cov == null) return;
    if (it.ir_len > 0) begin
      cov.sample_jtag(1'b1, it.ir[4:0], '0, '0);
    end else if (it.dr_len > 0) begin
      cov.sample_jtag(1'b0, '0, it.dr[31:0], it.dout[31:0]);
    end
  endtask

  protected function bit can_compare();
    if (!update_inputs()) trusted = 1'b0;
    if (!trusted) n_untrusted++;
    return trusted;
  endfunction

  // ---- DMI ----
  // The model takes each DMI operation when the request goes to the DM (the DTM's DR-Update:
  // dmi_req_fifo), as rv_dm does; a read's data is compared when the next scan returns it
  // (dmi_fifo, the same item with rsp_op and rdata). Taking it at the response instead put the model
  // one operation behind rv_dm (debug_req rose before the model had seen haltreq, 2026-11-06).


  protected task process_dmi_req();
    jtag_dmi_item it;
    dmi_req_fifo.get(it);
    if (!(it.req_op inside {DmiOpRead, DmiOpWrite})) return;
    pend_item = it;
    pend_cmp  = can_compare();
    last_dmi_req_t = $realtime;
    pend_cheriot = cheriot_mode();
    if (it.req_op == DmiOpRead) begin
      pend_after_sba_write = last_sba_write;
      pend_ok = eg2_dm_cmodel_pkg::eg2_dm_dmi_read(h, it.addr, pend_data, pend_mask);
      note($sformatf("DMI read 0x%02h (request)", it.addr));
    end else begin
      void'(eg2_dm_cmodel_pkg::eg2_dm_dmi_write(h, it.addr, it.wdata[31:0]));
      if (it.addr == 'h10) begin
        if (!it.wdata[0]) resume_pending = 1'b0;
        else if (it.wdata[30] && !it.wdata[31]) resume_pending = 1'b1;
        // rv_dm clears resumeack for resumereq written with haltreq (PULP dm_csrs.sv, the rising
        // edge of the stored resumereq); the Debug spec ignores that resumereq (3.12.2). From then
        // until the next resume request that counts, resumeack is the finding's.
        dm_inactive = !it.wdata[0];
        if (it.wdata[0] && it.wdata[30] && it.wdata[31]) resumeack_known = 1'b1;
        else if (!it.wdata[0] || (it.wdata[30] && !it.wdata[31])) resumeack_known = 1'b0;
      end
      note($sformatf("DMI write 0x%02h = 0x%08h", it.addr, it.wdata[31:0]));
    end
    take_sba();
    schedule_output_check();
  endtask

  protected task process_dmi();
    jtag_dmi_item it;
    int unsigned  d, m;
    dmi_fifo.get(it);
    if (!(it.req_op inside {DmiOpRead, DmiOpWrite})) return;
    if (it != pend_item) begin
      `uvm_error(`gfn, $sformatf("DMI response for a request the model did not take: %0s",
                                 it.sprint(uvm_default_line_printer)))
      return;
    end
    pend_item = null;
    if (cov != null) begin
      cov.sample_dmi(it.addr[6:0], it.req_op == DmiOpWrite, it.rsp_op, it.wdata[31:0],
                     it.rdata[31:0], pend_cheriot);
    end
    if (it.rsp_op != DmiOpOk) begin
      // The model took the request, but the DTM reports that it did not reach the DM.
      n_dtm_not_ok++;
      trusted = 1'b0;
      return;
    end
    if (it.req_op != DmiOpRead) return;
    note($sformatf("DMI read 0x%02h returns rv_dm 0x%08h", it.addr, it.rdata[31:0]));
    if (!pend_ok) begin
      n_gated++;
    end else if (pend_cmp && trusted) begin
      d = pend_data;
      if (pend_mask != '1) n_not_predicted++;
      if (corrupt && pend_mask[0]) begin
        d[0] = !d[0];
        corrupt = 1'b0;
      end
      m = pend_mask;
      if (mask_known) begin
        // rv_dm reads back dmcontrol.haltreq and resumereq; the documents have them read 0.
        if (it.addr == 'h10 && (it.rdata[31:30] != d[31:30])) begin
          n_known_haltreq++;
          m[31:30] = '0;
        end
        // After a bus write rv_dm's sbdata0 holds the write response's data.
        if (it.addr == 'h3c && pend_after_sba_write && (it.rdata[31:0] != d)) begin
          n_known_sbdata++;
          m = '0;
        end
        // rv_dm reads sbcs bits 28:23 back as written (PULP dm_csrs.sv copies the whole write into
        // sbcs); the Debug spec has them reserved, read 0.
        if (it.addr == 'h38 && (it.rdata[28:23] != d[28:23])) begin
          n_known_sbcs++;
          m[28:23] = '0;
        end
        // rv_dm reports abstractcs.busy 1 while a resume is pending (PULP dm_mem.sv, Resume state);
        // the Debug spec has busy only for an abstract command.
        if (it.addr == 'h16 && resume_pending && it.rdata[12] && !d[12]) begin
          n_known_busy_resume++;
          m[12] = 1'b0;
        end
        if (it.addr == 'h11 && resumeack_known && (it.rdata[17:16] != d[17:16])) begin
          n_known_resumeack++;
          m[17:16] = '0;
        end
        // rv_dm's haltsum0-3 count a halted hart that is unavailable as halted (PULP dm_csrs.sv
        // uses the raw halted bits); the Debug spec does not count it (haltsum0-3).
        // While dmactive is 0, rv_dm reads sbcs's constant fields (sbversion, sbasize, sbaccess8-128)
        // as 0 (PULP dm_csrs.sv sets them only out of reset); the Debug spec has the module's
        // reset values then (3.12.2), which for those read-only fields are the constants.
        if (it.addr == 'h38 && dm_inactive && ((it.rdata[31:0] ^ d) & 32'he000_0fff) != 0) begin
          n_known_sbcs_inactive++;
          m &= ~32'he000_0fff;
        end
        // rv_dm keeps a written sbdata1 although its bus is 32 bits wide (PULP stores sbdata[63:32]);
        // the Debug spec has sbdata1 only with 64- or 128-bit accesses (3.12.24).
        if (it.addr == 'h3d && it.rdata[31:0] != d) begin
          n_known_sbdata1++;
          m = '0;
        end
        if (it.addr inside {'h40, 'h13, 'h34, 'h35} && cfg.rv_dm_vif.unavailable[0] === 1'b1 &&
            it.rdata[0] && !d[0]) begin
          n_known_haltsum_unavail++;
          m[0] = 1'b0;
        end
      end
      compare($sformatf("DMI read 0x%02h", it.addr), it.rdata[31:0] & m, d & m);
      n_dmi_cmp++;
    end
  endtask

  // ---- The core's side: the debug memory ----

  protected task process_mem();
    tl_seq_item  it;
    int unsigned off, exp, m;
    bit          cmp;
    mem_d_fifo.get(it);
    off = (it.a_addr - cfg.ral_models[cfg.mem_ral_name].default_map.get_base_addr()) & ~32'h3;
    if (cov != null) cov.sample_mem(off[11:0], it.is_write(), it.d_error, it.d_data);
    if (it.d_error) begin
      n_mem_err++;  // debug disabled or a bad access: rv_dm_scoreboard checks these
      return;
    end
    cmp = can_compare();
    if (it.is_write()) begin
      eg2_dm_cmodel_pkg::eg2_dm_mem_write(h, off, it.a_data, it.a_mask);
      if (off == 'h110) resume_pending = 1'b0;
      note($sformatf("core write 0x%03h = 0x%08h", off, it.a_data));
    end else begin
      exp = eg2_dm_cmodel_pkg::eg2_dm_mem_read(h, off, m);
      note($sformatf("core read 0x%03h: rv_dm 0x%08h", off, it.d_data));
      if (m == 0) begin
        n_not_predicted++;
      end else if (last_dmi_req_t >= 0 &&
                   $realtime - last_dmi_req_t <
                   (settle_cycles * cfg.clk_rst_vif.clk_period_ps +
                    settle_tck * cfg.m_jtag_agent_cfg.vif.get_tck_period_ps()) * 1ps) begin
        n_mem_in_cdc++;
      end else if (cmp) begin
        compare($sformatf("debug memory read 0x%03h", off), it.d_data & m, exp & m);
        n_mem_cmp++;
      end
    end
    take_sba();
    schedule_output_check();
  endtask

  // ---- System bus ----
  // rv_dm starts an access when a DMI write reaches it, but the JTAG DMI monitor reports that write
  // only when the next scan reads its status back: the bus request and its response usually come
  // before the model has started the access. So the bus's accesses wait in rtl_sba_q, the model's
  // in exp_sba_q; they are paired in order and compared, and the model takes the response once
  // both are there.


  protected function void take_sba();
    sba_req_t r;
    int       w;
    int unsigned a, s, wd;
    forever begin
      while (eg2_dm_cmodel_pkg::eg2_dm_sba_take(h, w, a, s, wd)) begin
        r.write = w != 0;
        r.addr  = a;
        r.size  = s;
        r.wdata = wd;
        exp_sba_q.push_back(r);
        last_sba_write = r.write;
        note($sformatf("model starts a bus %0s 0x%08h size %0d", r.write ? "write" : "read", r.addr, r.size));
      end
      if (exp_sba_q.size() == 0 || rtl_sba_q.size() == 0) return;
      if (!rtl_sba_q[0].compared) begin
        compare_sba(rtl_sba_q[0], exp_sba_q[0]);
        rtl_sba_q[0].compared = 1'b1;
      end
      if (!rtl_sba_q[0].d_seen) return;
      r = exp_sba_q.pop_front();
      eg2_dm_cmodel_pkg::eg2_dm_sba_complete(
          h, (rtl_sba_q[0].d_data >> (8 * r.addr[1:0])) & mask_of(r.size), rtl_sba_q[0].d_error);
      void'(rtl_sba_q.pop_front());
    end
  endfunction

  // A write's byte lanes are its size at its address; a read (TL-UL Get) has every lane set: the
  // TL-UL host adapter sets all mask bits for a Get (tlul_adapter_host.sv: "For TL-UL Get opcode
  // all active bytes must have their mask bit set, so all reads get all tl_be bits set").
  protected static function bit [3:0] lanes(sba_req_t r);
    if (!r.write) return 4'hf;
    return (r.size == 4 ? 4'hf : r.size == 2 ? 4'h3 : 4'h1) << r.addr[1:0];
  endfunction

  protected function void compare_sba(rtl_sba_t t, sba_req_t r);
    if (!can_compare()) return;
    n_sba_cmp++;
    compare("system bus request write", t.write, r.write);
    compare("system bus request word address", t.addr & ~32'h3, r.addr & ~32'h3);
    compare("system bus request byte lanes", t.mask, lanes(r));
    if (r.write) begin
      compare("system bus write data", (t.data >> (8 * r.addr[1:0])) & mask_of(r.size), r.wdata);
    end
  endfunction

  protected task process_sba_a();
    tl_seq_item it;
    rtl_sba_t   t;
    sba_a_fifo.get(it);
    t.write = it.is_write();
    t.addr  = it.a_addr;
    t.mask  = it.a_mask;
    t.data  = it.a_data;
    rtl_sba_q.push_back(t);
    take_sba();
  endtask

  protected task process_sba_d();
    tl_seq_item it;
    sba_d_fifo.get(it);
    foreach (rtl_sba_q[i]) begin
      if (!rtl_sba_q[i].d_seen) begin
        rtl_sba_q[i].d_seen  = 1'b1;
        rtl_sba_q[i].d_data  = it.d_data;
        rtl_sba_q[i].d_error = it.d_error;
        if (cov != null) cov.sample_sba_bus(rtl_sba_q[i].write, rtl_sba_q[i].mask, it.d_error);
        note($sformatf("bus response %0s 0x%08h data 0x%08h%0s", rtl_sba_q[i].write ? "write" : "read",
                       rtl_sba_q[i].addr, it.d_data, it.d_error ? " error" : ""));
        break;
      end
    end
    take_sba();
    schedule_output_check();
  endtask

  protected static function bit [31:0] mask_of(int unsigned size);
    return size >= 4 ? '1 : (32'h1 << (8 * size)) - 1;
  endfunction

  // ---- Outputs ----

  protected function void schedule_output_check();
    int unsigned ev = ++n_events;
    fork
      begin
        cfg.clk_rst_vif.wait_clks(settle_cycles);
        // A DMI request crosses from the JTAG clock into the DM's clock first: wait some TCK periods.
        #(settle_tck * cfg.m_jtag_agent_cfg.vif.get_tck_period_ps() * 1ps);
        if (ev == n_events && cfg.clk_rst_vif.rst_n && can_compare()) begin
          n_out_cmp++;
          compare("debug_req", cfg.rv_dm_vif.debug_req[0] === 1'b1,
                  eg2_dm_cmodel_pkg::eg2_dm_debug_req(h) != 0);
          compare("ndmreset_req", cfg.rv_dm_vif.ndmreset_req === 1'b1,
                  eg2_dm_cmodel_pkg::eg2_dm_ndmreset(h) != 0);
          compare("dmactive", cfg.rv_dm_vif.dmactive === 1'b1,
                  eg2_dm_cmodel_pkg::eg2_dm_dmactive(h) != 0);
        end
      end
    join_none
  endfunction

  // The last transactions the model was given, for the mismatch reports.
  protected string hist[$];
  protected string last_note;
  protected function void note(string s);
    if (s == last_note) return;  // the emulated core polls the flags: keep one line of it
    last_note = s;
    hist.push_back($sformatf("%t %0s", $time, s));
    if (hist_len == 0) `uvm_info(`gfn, $sformatf("model input: %0s", s), UVM_LOW)
    else if (hist.size() > hist_len) void'(hist.pop_front());
    if (hist_len == 0 && hist.size() > 24) void'(hist.pop_front());
  endfunction
  protected function string history();
    string s = "\n  model inputs before it (oldest first):";
    foreach (hist[i]) s = {s, "\n    ", hist[i]};
    return s;
  endfunction

  protected function void compare(string what, bit [31:0] got, bit [31:0] exp);
    if (got !== exp) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf("%0s: rv_dm 0x%08h, model 0x%08h%0s", what, got, exp,
                                 n_mismatch <= 5 ? history() : ""))
    end
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    if (exp_sba_q.size() != 0 && trusted) begin
      `uvm_error(`gfn, $sformatf("%0d system bus access(es) the model started never reached the bus",
                                 exp_sba_q.size()))
    end
    if (rtl_sba_q.size() != 0 && trusted) begin
      `uvm_error(`gfn, $sformatf("%0d system bus access(es) rv_dm made that the model never started",
                                 rtl_sba_q.size()))
    end
    if (n_dmi_cmp + n_mem_cmp + n_sba_cmp + n_out_cmp == 0) begin
      `uvm_error(`gfn, "the model compared nothing (no DMI, debug memory or bus traffic it predicts)")
    end
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"rv_dm cmodel: compared DMI %0d, debug memory %0d, bus %0d, ",
                               "outputs %0d; mismatches %0d; not predicted %0d, DMI gated %0d, ",
                               "DTM not Ok %0d, untrusted (undocumented life cycle) %0d, ",
                               "debug memory errors %0d, debug memory reads during a DMI crossing %0d; ",
                               "known RTL findings masked: haltreq/resumereq read-back %0d, ",
                               "sbdata0 after a write %0d, sbcs reserved bits %0d, busy while a resume is pending %0d, ",
                               "resumeack cleared by an ignored resumereq %0d, ",
                               "haltsum of an unavailable hart %0d, sbcs while dmactive is 0 %0d, sbdata1 stored %0d"},
                              n_dmi_cmp, n_mem_cmp, n_sba_cmp, n_out_cmp, n_mismatch,
                              n_not_predicted, n_gated, n_dtm_not_ok, n_untrusted, n_mem_err, n_mem_in_cdc,
                              n_known_haltreq,
                              n_known_sbdata, n_known_sbcs, n_known_busy_resume, n_known_resumeack,
                              n_known_haltsum_unavail, n_known_sbcs_inactive, n_known_sbdata1),
              UVM_NONE)
    if (h != null) eg2_dm_cmodel_pkg::eg2_dm_release(h);
    h = null;
  endfunction
endclass
