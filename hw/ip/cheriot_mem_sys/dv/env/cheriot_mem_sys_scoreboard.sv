// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The scoreboard compares what the monitors observe with what the reference model expects
// (cheriot_mem_sys_ref_model, when cfg.en_cmodel is set):
// - every response on the core data port, the two bitmap ports and the `regs` port (register
//   reads): the error, the data in the bits the model is sure of, and the tag;
// - fatal_fault: each alert the model expects must come, and no other;
// - the interrupt pin, at each register read;
// - the meta SRAM, word by word, whenever the bench is quiet after it changed, and at the end.
// It also checks what needs no model:
// - the meta SRAM is accessed only in CHERIoT mode;
// - the revocation engine reads only during a sweep;
// - a tag write-back changes its word, and none follows a failed read of it.
//
// It keeps the register model (RAL) up to date from the `regs` port; without the reference model
// (the common CIP tests) the RAL checks the register reads and alerts are checked the CIP way.
`uvm_analysis_imp_decl(_mode_sb)
`uvm_analysis_imp_decl(_core_req_sb)
`uvm_analysis_imp_decl(_tbre_h_req)
`uvm_analysis_imp_decl(_meta_req)
`uvm_analysis_imp_decl(_meta_rsp_sb)
`uvm_analysis_imp_decl(_activity)

class cheriot_mem_sys_scoreboard extends cip_base_scoreboard #(
    .CFG_T(cheriot_mem_sys_env_cfg),
    .RAL_T(cheriot_mem_sys_regs_reg_block),
    .COV_T(cheriot_mem_sys_env_cov)
  );
  `uvm_component_utils(cheriot_mem_sys_scoreboard)

  // The reference model (null without it).
  cheriot_mem_sys_ref_model ref_model;

  // ---- Observed, from the monitors ----
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_core_item) core_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               revbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               corerevbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               regs_fifo;
  uvm_analysis_imp_mode_sb #(prim_mubi_pkg::mubi4_t, cheriot_mem_sys_scoreboard)  mode_imp;
  uvm_analysis_imp_core_req_sb #(cheriot_mem_sys_core_item, cheriot_mem_sys_scoreboard)
      core_req_imp;
  uvm_analysis_imp_tbre_h_req #(tl_seq_item, cheriot_mem_sys_scoreboard) tbre_h_req_imp;
  uvm_analysis_imp_meta_req #(tl_seq_item, cheriot_mem_sys_scoreboard)   meta_req_imp;
  uvm_analysis_imp_meta_rsp_sb #(tl_seq_item, cheriot_mem_sys_scoreboard) meta_rsp_imp;
  // Any item on any other port: the bench is not quiet.
  uvm_analysis_imp_activity #(tl_seq_item, cheriot_mem_sys_scoreboard)   activity_imp;

  // ---- Expected, from the reference model ----
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_rsp_exp)   core_exp_fifo;
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_rsp_exp)   revbm_exp_fifo;
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_rsp_exp)   corerevbm_exp_fifo;
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_rsp_exp)   csr_exp_fifo;
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_alert_exp) alert_exp_fifo;

  // An expected alert must come, and an observed one be expected, within this many cycles.
  localparam int unsigned AlertWindow = 2000;
  // A latched fatal alert repeats at least this often.
  localparam int unsigned FatalRepeat = 1000;
  // The bench is quiet after this many cycles without an item on any port.
  localparam int unsigned QuietCycles = 20;

  protected prim_mubi_pkg::mubi4_t mode = prim_mubi_pkg::MuBi4False;
  protected longint unsigned       cycle, last_activity;
  protected int unsigned           core_in_flight;
  protected bit                    meta_changed;
  // The meta SRAM as its writes leave it, and the tag words whose read failed (their
  // read-modify-write is aborted).
  protected bit [31:0]             meta_image[bit [31:0]];
  protected bit                    aborted_words[bit [31:0]];
  // Alerts: the fatal one latched, ALERT_TEST alerts and observed alerts waiting for their match
  // (the cycle each arrived), an optional one allowed.
  protected bit                    fatal_expected, fatal_seen;
  protected longint unsigned       fatal_since, last_alert;
  protected longint unsigned       alert_tests_q[$], unmatched_alerts_q[$];
  protected bit                    alert_optional;

  protected int unsigned n_core, n_bitmap, n_csr, n_meta_words, n_mismatch, n_reads;
  protected bit          corrupted;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    core_fifo          = new("core_fifo", this);
    revbm_fifo         = new("revbm_fifo", this);
    corerevbm_fifo     = new("corerevbm_fifo", this);
    regs_fifo          = new("regs_fifo", this);
    mode_imp           = new("mode_imp", this);
    core_req_imp       = new("core_req_imp", this);
    tbre_h_req_imp     = new("tbre_h_req_imp", this);
    meta_req_imp       = new("meta_req_imp", this);
    meta_rsp_imp       = new("meta_rsp_imp", this);
    activity_imp       = new("activity_imp", this);
    core_exp_fifo      = new("core_exp_fifo", this);
    revbm_exp_fifo     = new("revbm_exp_fifo", this);
    corerevbm_exp_fifo = new("corerevbm_exp_fifo", this);
    csr_exp_fifo       = new("csr_exp_fifo", this);
    alert_exp_fifo     = new("alert_exp_fifo", this);
    // With the reference model, it is the alerts' checker.
    if (cfg.en_cmodel) do_alert_check = 1'b0;
  endfunction

  task run_phase(uvm_phase phase);
    super.run_phase(phase);
    fork
      count_cycles();
      forever begin
        cheriot_mem_sys_core_item act;
        core_fifo.get(act);
        if (cfg.en_cov) cov.core_access_cg.sample(act, region_of(act.addr));
        if (core_in_flight > 0) core_in_flight--;
        if (ref_model != null) compare_core(act);
      end
      forever begin
        tl_seq_item act;
        revbm_fifo.get(act);
        if (cfg.en_cov) cov.bitmap_access_cg.sample(1'b0, act);
        if (ref_model != null) compare_bitmap(act, revbm_exp_fifo);
      end
      forever begin
        tl_seq_item act;
        corerevbm_fifo.get(act);
        if (cfg.en_cov) cov.bitmap_access_cg.sample(1'b1, act);
        if (ref_model != null) compare_bitmap(act, corerevbm_exp_fifo);
      end
      forever begin
        tl_seq_item act;
        regs_fifo.get(act);
        if (ref_model != null && !act.d_error && !act.is_write()) compare_csr(act);
      end
      forever begin
        cheriot_mem_sys_alert_exp exp;
        alert_exp_fifo.get(exp);
        expect_alert(exp.kind);
      end
    join_none
  endtask

  // ---- Responses ----

  protected task compare_core(cheriot_mem_sys_core_item act);
    cheriot_mem_sys_rsp_exp exp;
    bit [31:0]              rdata = act.rdata;
    core_exp_fifo.get(exp);
    n_core++;
    // The _inject_fail test: the n-th read response is compared with one bit flipped.
    if (act.opcode == tlul_pkg::Get && !act.err && ++n_reads == cfg.cmodel_corrupt) begin
      rdata[0]  = !rdata[0];
      corrupted = 1'b1;
      `uvm_info(`gfn, $sformatf("read %0d at 0x%08x compared with one bit flipped", n_reads,
                                act.addr), UVM_LOW)
    end
    compare(exp, act.err, rdata);
    if (!act.err && !exp.rtag_set[act.rtag]) begin
      mismatch($sformatf("%s: tag %0d, expected %s", exp.what, act.rtag,
                         exp.rtag_set == 2'b01 ? "0" : exp.rtag_set == 2'b10 ? "1" : "0 or 1"));
    end
  endtask

  protected task compare_bitmap(tl_seq_item act,
                                uvm_tlm_analysis_fifo #(cheriot_mem_sys_rsp_exp) exp_fifo);
    cheriot_mem_sys_rsp_exp exp;
    exp_fifo.get(exp);
    n_bitmap++;
    compare(exp, act.d_error, act.d_data);
  endtask

  protected task compare_csr(tl_seq_item act);
    cheriot_mem_sys_rsp_exp exp;
    csr_exp_fifo.get(exp);
    n_csr++;
    compare(exp, 1'b0, act.d_data);
    // The interrupt pin is INTR_STATE & INTR_ENABLE. The model learns that a sweep has ended
    // later than the pin rises, so while it still counts a sweep the pin is not judged.
    if (!ref_model.sweep_active() && cfg.intr_vif.pins[0] != ref_model.intr_expected()) begin
      mismatch($sformatf("intr_tbre_done_o %0d, expected %0d", cfg.intr_vif.pins[0],
                         ref_model.intr_expected()));
    end
  endtask

  protected function void compare(cheriot_mem_sys_rsp_exp exp, bit err, bit [31:0] rdata);
    if (exp.err_care && err != exp.err) begin
      mismatch($sformatf("%s: d_error %0d, expected %0d", exp.what, err, exp.err));
    end else if (!err && (rdata & exp.rdata_care) != (exp.rdata & exp.rdata_care)) begin
      mismatch($sformatf("%s: data 0x%08x, expected 0x%08x (bits 0x%08x)", exp.what, rdata,
                         exp.rdata, exp.rdata_care));
    end
  endfunction

  protected function void mismatch(string msg);
    n_mismatch++;
    `uvm_error(`gfn, msg)
  endfunction

  // ---- Alerts ----

  protected function void expect_alert(alert_exp_e kind);
    case (kind)
      AlertExpFatal: begin
        fatal_expected = 1'b1;
        fatal_since    = cycle;
        // An alert that came first, from the fault the model has just learnt of
        if (unmatched_alerts_q.size() > 0) begin
          unmatched_alerts_q.delete();
          fatal_seen = 1'b1;
          last_alert = cycle;
        end
      end
      AlertExpTest: begin
        if (unmatched_alerts_q.size() > 0) void'(unmatched_alerts_q.pop_front());
        else alert_tests_q.push_back(cycle);
      end
      default: alert_optional = 1'b1;
    endcase
  endfunction

  virtual function void on_alert(string alert_name, alert_agent_pkg::alert_seq_item item);
    if (ref_model == null) begin
      super.on_alert(alert_name, item);
      return;
    end
    last_alert = cycle;
    if (alert_tests_q.size() > 0) void'(alert_tests_q.pop_front());
    else if (fatal_expected) fatal_seen = 1'b1;
    else if (!alert_optional) unmatched_alerts_q.push_back(cycle);
  endfunction

  // Every cycle: the deadlines of the expected and the unmatched alerts, and the quiet points.
  protected task count_cycles();
    forever begin
      @(cfg.clk_rst_vif.cbn);
      cycle++;
      if (ref_model == null || cfg.under_reset) continue;
      if (alert_tests_q.size() > 0 && cycle - alert_tests_q[0] > AlertWindow) begin
        void'(alert_tests_q.pop_front());
        mismatch("an ALERT_TEST write raised no fatal_fault alert");
      end
      if (unmatched_alerts_q.size() > 0 && cycle - unmatched_alerts_q[0] > AlertWindow) begin
        void'(unmatched_alerts_q.pop_front());
        mismatch("fatal_fault alert that nothing raised");
      end
      if (fatal_expected && !fatal_seen && cycle - fatal_since > AlertWindow) begin
        fatal_seen = 1'b1;  // reported once
        mismatch("a fault the documents say raises fatal_fault raised none");
      end
      if (fatal_expected && fatal_seen && cycle - last_alert > FatalRepeat) begin
        last_alert = cycle;
        mismatch("fatal_fault is not latched: it stopped before the reset");
      end
      if (meta_changed && core_in_flight == 0 && cycle - last_activity >= QuietCycles &&
          !ref_model.sweep_active()) begin
        meta_changed = 1'b0;
        compare_meta("at a quiet point");
      end
    end
  endtask

  // ---- What needs no model ----

  function void write_mode_sb(prim_mubi_pkg::mubi4_t m);
    mode = m;
  endfunction

  function void write_core_req_sb(cheriot_mem_sys_core_item it);
    core_in_flight++;
    last_activity = cycle;
  endfunction

  function void write_activity(tl_seq_item it);
    last_activity = cycle;
  endfunction

  function void write_meta_req(tl_seq_item it);
    last_activity = cycle;
    if (mode != prim_mubi_pkg::MuBi4True) begin
      `uvm_error(`gfn, $sformatf("meta SRAM accessed at 0x%08x with cheriot_ena_i 0x%0h",
                                 it.a_addr, mode))
    end
  endfunction

  // The revocation engine reads only during a sweep. The model learns of a start when the
  // TBRE_START write completes, which can be after the engine's first read: a few cycles' grace.
  function void write_tbre_h_req(tl_seq_item it);
    last_activity = cycle;
    if (ref_model == null || ref_model.sweep_active()) return;
    fork
      begin
        bit [31:0] addr = it.a_addr;
        cfg.clk_rst_vif.wait_clks(10);
        if (!ref_model.sweep_active()) begin
          `uvm_error(`gfn, $sformatf("the revocation engine read 0x%08x outside a sweep", addr))
        end
      end
    join_none
  endfunction

  // A tag word's write-back: it changes the word, and none follows a failed read of it.
  function void write_meta_rsp_sb(tl_seq_item it);
    bit [31:0] word  = {it.a_addr[31:2], 2'b00};
    bit        tag   = it.a_addr >= MetaNvmTagBase;
    bit        fault = it.d_error || !tl_d_intg_ok(it);
    last_activity = cycle;
    if (!it.is_write()) begin
      if (tag) begin
        if (fault) aborted_words[word] = 1'b1;
        else       aborted_words.delete(word);
      end
      return;
    end
    if (tag && aborted_words.exists(word)) begin
      `uvm_error(`gfn, $sformatf("tag word 0x%08x written back after its read failed", word))
    end
    if (tag && meta_image.exists(word) && meta_image[word] == it.a_data && it.a_mask == 4'hf) begin
      `uvm_error(`gfn, $sformatf("tag word 0x%08x written back unchanged (0x%08x)", word,
                                 it.a_data))
    end
    if (!it.d_error) begin
      meta_image[word] = it.a_data;
      meta_changed = 1'b1;
    end
  endfunction

  // ---- The meta SRAM ----

  protected function void compare_meta(string why);
    bit [31:0] value, care, actual;
    int unsigned n;
    for (int unsigned off = 0; off < ref_model.meta_bytes(); off += 4) begin
      ref_model.meta_word(off, value, care);
      actual = cfg.meta_mem.read(MetaBase + off);
      n_meta_words++;
      if ((actual & care) != (value & care) && n++ < 8) begin
        mismatch($sformatf("meta SRAM %s: word 0x%04x is 0x%08x, expected 0x%08x (bits 0x%08x)",
                           why, off, actual, value, care));
      end
    end
  endfunction

  // ---- The `regs` port and the register model ----

  virtual task process_tl_access(tl_seq_item item, tl_channels_e channel, string ral_name);
    uvm_reg csr = cfg.ral_models[ral_name].default_map.get_reg_by_offset(
        cfg.ral_models[ral_name].get_word_aligned_addr(item.a_addr));
    if (csr == null) return;
    if (channel == AddrChannel && item.is_write()) begin
      void'(csr.predict(.value(item.a_data), .kind(UVM_PREDICT_WRITE), .be(item.a_mask)));
    end
    if (channel == DataChannel && !item.is_write()) begin
      void'(csr.predict(.value(item.d_data), .kind(UVM_PREDICT_READ)));
    end
  endtask

  virtual function void do_read_check(dv_base_reg dv_reg, tl_seq_item item);
    // With the reference model, the scoreboard compares every register read with it.
    if (ref_model != null) return;
    // The revocation engine changes these; only the reference model predicts them.
    if (dv_reg.get_name() inside {"intr_state", "tbre_regwen", "tbre_status", "tbre_epoch"}) return;
    super.do_read_check(dv_reg, item);
  endfunction

  // A reset of the subsystem: the alerts it latched and the accesses in flight are gone.
  virtual function void reset(string kind = "HARD");
    super.reset(kind);
    core_fifo.flush();
    revbm_fifo.flush();
    corerevbm_fifo.flush();
    regs_fifo.flush();
    core_exp_fifo.flush();
    revbm_exp_fifo.flush();
    corerevbm_exp_fifo.flush();
    csr_exp_fifo.flush();
    if (ref_model != null) begin
      if (fatal_expected && !fatal_seen) mismatch("a fault raised no fatal_fault before the reset");
      if (alert_tests_q.size() > 0) mismatch("an ALERT_TEST alert did not come before the reset");
    end
    core_in_flight = 0;
    aborted_words.delete();
    fatal_expected = 1'b0;
    fatal_seen     = 1'b0;
    alert_optional = 1'b0;
    alert_tests_q.delete();
    unmatched_alerts_q.delete();
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    if (ref_model == null) return;
    compare_meta("at the end of the test");
    if (alert_tests_q.size() > 0) mismatch("an ALERT_TEST alert did not come");
    if (unmatched_alerts_q.size() > 0) mismatch("fatal_fault alert that nothing raised");
    if (fatal_expected && !fatal_seen) mismatch("a fault raised no fatal_fault");
    if (!core_exp_fifo.is_empty() || !core_fifo.is_empty()) begin
      mismatch("core accesses left uncompared at the end of the test");
    end
  endfunction

  function void report_phase(uvm_phase phase);
    super.report_phase(phase);
    if (ref_model == null) return;
    `uvm_info(`gfn, $sformatf({"compared with the reference model: %0d core, %0d bitmap and %0d ",
                              "register accesses, %0d meta SRAM words; %0d mismatches%s"}, n_core,
                              n_bitmap, n_csr, n_meta_words, n_mismatch,
                              corrupted ? " (one read compared corrupted)" : ""), UVM_LOW)
    if (cfg.cmodel_corrupt != 0 && !corrupted) begin
      `uvm_error(`gfn, $sformatf("the corruption at read %0d never happened (%0d reads)",
                                 cfg.cmodel_corrupt, n_reads))
    end
  endfunction

endclass
