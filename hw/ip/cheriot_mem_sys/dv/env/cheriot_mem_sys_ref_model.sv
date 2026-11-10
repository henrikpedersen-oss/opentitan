// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The reference model: the C model of the subsystem (dv/cms/cmodel/cms_core.h, written from the
// design documents, through cms_cmodel_pkg), fed only by the monitors. For every access on the
// core data port, the two bitmap ports and the `regs` port it sends the scoreboard the response
// the documents allow, and it sends the alerts it expects. It never takes a value from the DUT;
// the one thing it follows is when a sweep has ended, which it learns from a register read as
// software does.
//
// What the subsystem receives on its host side is an input to it, and the model takes it from the
// device agents' monitors: the crossbar's error answer to each core access (one per access, in
// order), and the errors and integrity faults on the responses to the revocation engine and from
// the meta SRAM. A fault on a meta SRAM response is attributed to the requester that had an
// access to that word in flight: a core access to a granule of the tag word, a bitmap window
// access to the bitmap word, or else the running sweep.
`uvm_analysis_imp_decl(_core_req)
`uvm_analysis_imp_decl(_core_rsp)
`uvm_analysis_imp_decl(_mode)
`uvm_analysis_imp_decl(_backdoor)
`uvm_analysis_imp_decl(_cored_h_rsp)
`uvm_analysis_imp_decl(_tbre_h_rsp)
`uvm_analysis_imp_decl(_meta_rsp)
`uvm_analysis_imp_decl(_window_req)

class cheriot_mem_sys_ref_model extends uvm_component;
  `uvm_component_utils(cheriot_mem_sys_ref_model)

  cheriot_mem_sys_env_cfg cfg;

  // ---- From the monitors ----
  uvm_analysis_imp_core_req #(cheriot_mem_sys_core_item, cheriot_mem_sys_ref_model) core_req_imp;
  uvm_analysis_imp_core_rsp #(cheriot_mem_sys_core_item, cheriot_mem_sys_ref_model) core_rsp_imp;
  uvm_analysis_imp_mode #(prim_mubi_pkg::mubi4_t, cheriot_mem_sys_ref_model)        mode_imp;
  uvm_analysis_imp_backdoor #(cheriot_mem_sys_backdoor_item, cheriot_mem_sys_ref_model)
      backdoor_imp;
  uvm_analysis_imp_cored_h_rsp #(tl_seq_item, cheriot_mem_sys_ref_model) cored_h_rsp_imp;
  uvm_analysis_imp_tbre_h_rsp #(tl_seq_item, cheriot_mem_sys_ref_model)  tbre_h_rsp_imp;
  uvm_analysis_imp_meta_rsp #(tl_seq_item, cheriot_mem_sys_ref_model)    meta_rsp_imp;
  uvm_analysis_imp_window_req #(tl_seq_item, cheriot_mem_sys_ref_model)  window_req_imp;
  // Completed accesses, predicted in the order they complete
  uvm_tlm_analysis_fifo #(cheriot_mem_sys_core_item) core_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               revbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               corerevbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)               regs_fifo;

  // ---- To the scoreboard ----
  uvm_analysis_port #(cheriot_mem_sys_rsp_exp)   core_exp_port;
  uvm_analysis_port #(cheriot_mem_sys_rsp_exp)   revbm_exp_port;
  uvm_analysis_port #(cheriot_mem_sys_rsp_exp)   corerevbm_exp_port;
  uvm_analysis_port #(cheriot_mem_sys_rsp_exp)   csr_exp_port;
  uvm_analysis_port #(cheriot_mem_sys_alert_exp) alert_exp_port;

  protected chandle                model;
  protected prim_mubi_pkg::mubi4_t mode = prim_mubi_pkg::MuBi4False;
  // The crossbar's error answers, one per core access, in order.
  protected bit                    host_err_q[$];
  // Accesses in flight per meta SRAM word (offset): core accesses that look up a tag in a tag
  // word, bitmap window accesses to a bitmap word.
  protected int unsigned           core_lookups[bit [31:0]];
  protected int unsigned           window_accesses[bit [31:0]];
  // Faults attributed to an access that has not completed yet.
  protected int unsigned           core_meta_errs[bit [31:0]];
  protected bit                    window_faults[bit [31:0]];
  // The alerts already sent.
  protected bit                    fatal_sent;
  protected int                    alert_tests_sent;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    core_req_imp       = new("core_req_imp", this);
    core_rsp_imp       = new("core_rsp_imp", this);
    mode_imp           = new("mode_imp", this);
    backdoor_imp       = new("backdoor_imp", this);
    cored_h_rsp_imp    = new("cored_h_rsp_imp", this);
    tbre_h_rsp_imp     = new("tbre_h_rsp_imp", this);
    meta_rsp_imp       = new("meta_rsp_imp", this);
    window_req_imp     = new("window_req_imp", this);
    core_fifo          = new("core_fifo", this);
    revbm_fifo         = new("revbm_fifo", this);
    corerevbm_fifo     = new("corerevbm_fifo", this);
    regs_fifo          = new("regs_fifo", this);
    core_exp_port      = new("core_exp_port", this);
    revbm_exp_port     = new("revbm_exp_port", this);
    corerevbm_exp_port = new("corerevbm_exp_port", this);
    csr_exp_port       = new("csr_exp_port", this);
    alert_exp_port     = new("alert_exp_port", this);
`ifdef CHERIOT_MEM_SYS_CMS_MAP
    model = cms_cmodel_pkg::cms_new(1);
`else
    model = cms_cmodel_pkg::cms_new(0);
`endif
  endfunction

  task run_phase(uvm_phase phase);
    fork
      track_reset();
      forever begin
        cheriot_mem_sys_core_item it;
        core_fifo.get(it);
        predict_core(it);
      end
      forever begin
        tl_seq_item it;
        revbm_fifo.get(it);
        predict_bitmap(it, 1'b0);
      end
      forever begin
        tl_seq_item it;
        corerevbm_fifo.get(it);
        predict_bitmap(it, 1'b1);
      end
      forever begin
        tl_seq_item it;
        regs_fifo.get(it);
        predict_csr(it);
      end
    join
  endtask

  // ---- Queries of the scoreboard ----

  function bit sweep_active();
    return cms_cmodel_pkg::cms_sweep_active(model) != 0;
  endfunction

  function bit intr_expected();
    return cms_cmodel_pkg::cms_intr_expected(model) != 0;
  endfunction

  function int unsigned meta_bytes();
    return cms_cmodel_pkg::cms_meta_top(model);
  endfunction

  // A meta SRAM word as the model expects it, and the bits it is sure of.
  function void meta_word(bit [31:0] off, output bit [31:0] value, output bit [31:0] care);
    cms_cmodel_pkg::cms_meta_word(model, off, value, care);
  endfunction

  // ---- Inputs observed by the monitors ----

  function void write_mode(prim_mubi_pkg::mubi4_t m);
    mode = m;
  endfunction

  function void write_backdoor(cheriot_mem_sys_backdoor_item it);
    cms_cmodel_pkg::cms_backdoor_data(model, it.addr, it.data);
  endfunction

  function void write_core_req(cheriot_mem_sys_core_item it);
    bit [31:0] word;
    if (looks_up_tag(it, word)) core_lookups[word]++;
  endfunction

  function void write_core_rsp(cheriot_mem_sys_core_item it);
    bit [31:0] word;
    if (looks_up_tag(it, word) && core_lookups[word] > 0) core_lookups[word]--;
  endfunction

  function void write_window_req(tl_seq_item it);
    window_accesses[word_of(it.a_addr)]++;
  endfunction

  function void write_cored_h_rsp(tl_seq_item it);
    host_err_q.push_back(it.d_error);
  endfunction

  // A response to the revocation engine: an error response leaves the capability alone and fails
  // the sweep; an integrity fault does too, and raises fatal_fault.
  function void write_tbre_h_rsp(tl_seq_item it);
    if (!it.d_error && tl_d_intg_ok(it)) return;
    cms_cmodel_pkg::cms_sweep_read_error(model, it.a_addr);
    if (!it.d_error) cms_cmodel_pkg::cms_raise_fatal(model);
    send_alerts();
  endfunction

  // A response from the meta SRAM with an error or an integrity fault.
  function void write_meta_rsp(tl_seq_item it);
    bit [31:0] word = word_of(it.a_addr);
    bit [31:0] off  = word - MetaBase;
    if (!it.d_error && tl_d_intg_ok(it)) return;
    if (it.a_addr >= MetaNvmTagBase) begin
      if (core_lookups.exists(word) && core_lookups[word] > 0) begin
        // A device error on the tag path aborts the read-modify-write and answers the core with
        // an error; any fault raises fatal_fault ([ToO] "Error Handling").
        if (it.d_error) core_meta_errs[word]++;
        cms_cmodel_pkg::cms_raise_fatal(model);
      end else if (sweep_active()) begin
        cms_cmodel_pkg::cms_sweep_tag_fault(model, off);
      end else begin
        cms_cmodel_pkg::cms_raise_fatal(model);
      end
    end else begin
      if (window_accesses.exists(word) && window_accesses[word] > 0) begin
        // A fault on a response to a bitmap window: passed to the requester; whether it also
        // raises fatal_fault, the documents do not say.
        window_faults[word] = 1'b1;
        send_alert(AlertExpOptional);
      end else if (sweep_active()) begin
        cms_cmodel_pkg::cms_sweep_lookup_fault(model, off);
      end else begin
        cms_cmodel_pkg::cms_raise_fatal(model);
      end
    end
    send_alerts();
  endfunction

  // ---- Predictions ----

  protected task predict_core(cheriot_mem_sys_core_item it);
    cheriot_mem_sys_rsp_exp exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    bit        host_err, meta_err;
    bit [31:0] word;
    int        err, rtag_set;
    wait (host_err_q.size() > 0);
    host_err = host_err_q.pop_front();
    if (looks_up_tag(it, word) && core_meta_errs.exists(word) && core_meta_errs[word] > 0) begin
      core_meta_errs[word]--;
      meta_err = 1'b1;
    end
    cms_cmodel_pkg::cms_predict_core(model, int'(it.opcode), it.addr, int'(it.size), int'(it.mask),
                                     it.data, int'(it.tag), int'(it.ena), int'(host_err),
                                     int'(meta_err), err, exp.rdata, exp.rdata_care, rtag_set);
    exp.err      = err[0];
    exp.rtag_set = rtag_set[1:0];
    exp.what     = $sformatf("core %s 0x%08x size %0d mask %h data %08x tag %0d mode %h", it.opcode.name(),
                             it.addr, it.size, it.mask, it.data, it.tag, it.ena);
    core_exp_port.write(exp);
    cms_cmodel_pkg::cms_update_core(model, int'(it.opcode), it.addr, int'(it.size), int'(it.mask),
                                    it.data, int'(it.tag), int'(it.ena), int'(host_err),
                                    int'(meta_err));
    send_alerts();
  endtask

  protected function void predict_bitmap(tl_seq_item it, bit core_port);
    cheriot_mem_sys_rsp_exp exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    bit [31:0] word = word_of(it.a_addr);
    bit [31:0] off  = it.a_addr - MetaBase;
    int        err;
    if (window_accesses.exists(word) && window_accesses[word] > 0) window_accesses[word]--;
    cms_cmodel_pkg::cms_predict_bitmap(model, int'(it.a_opcode), off, int'(it.a_size),
                                       int'(it.a_mask), int'(mode), err, exp.rdata,
                                       exp.rdata_care);
    exp.err  = err[0];
    exp.what = $sformatf("%s %s 0x%08x", core_port ? "corerevbm" : "revbm",
                         tlul_pkg::tl_a_op_e'(it.a_opcode), it.a_addr);
    // A response the meta SRAM answered with a fault: not predicted.
    if (window_faults.exists(word)) begin
      window_faults.delete(word);
      exp.err_care   = 1'b0;
      exp.rdata_care = '0;
    end
    if (core_port) corerevbm_exp_port.write(exp);
    else           revbm_exp_port.write(exp);
    cms_cmodel_pkg::cms_update_bitmap(model, int'(it.a_opcode), off, int'(it.a_size),
                                      int'(it.a_mask), it.a_data, int'(mode));
  endfunction

  // A register access. A refused one (unmapped, partial write, integrity) is not a register
  // access; a command integrity fault raises fatal_fault.
  protected function void predict_csr(tl_seq_item it);
    cheriot_mem_sys_rsp_exp exp;
    bit [31:0] off = {it.a_addr[5:2], 2'b00};
    if (!tl_a_intg_ok(it)) begin
      cms_cmodel_pkg::cms_raise_fatal(model);
      send_alerts();
    end
    if (it.d_error) return;
    cms_cmodel_pkg::cms_set_mode(model, int'(mode));
    if (it.is_write()) begin
      cms_cmodel_pkg::cms_update_csr_write(model, off, it.a_data);
      send_alerts();
      return;
    end
    cms_cmodel_pkg::cms_sync_sweep_end(model, off, it.d_data);
    exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    cms_cmodel_pkg::cms_predict_csr_read(model, off, exp.rdata, exp.rdata_care);
    exp.what = $sformatf("register read 0x%02x", off);
    csr_exp_port.write(exp);
  endfunction

  // ---- Helpers ----

  // Whether a core access looks up a tag, and the meta SRAM word of that tag.
  protected function bit looks_up_tag(cheriot_mem_sys_core_item it, output bit [31:0] word);
    word = tag_word_addr(it.addr);
    return it.ena == prim_mubi_pkg::MuBi4True &&
           ((it.addr >= MainSramBase && it.addr < MainSramTop) || in_nvm(it.addr));
  endfunction

  protected function bit [31:0] word_of(bit [31:0] addr);
    return {addr[31:2], 2'b00};
  endfunction

  // The alerts the model expects that have not been sent yet.
  protected function void send_alerts();
    bit fatal = cms_cmodel_pkg::cms_fatal_expected(model) != 0;
    int tests = cms_cmodel_pkg::cms_alert_tests(model);
    if (fatal && !fatal_sent) send_alert(AlertExpFatal);
    fatal_sent = fatal;
    repeat (tests - alert_tests_sent) send_alert(AlertExpTest);
    alert_tests_sent = tests;
  endfunction

  protected function void send_alert(alert_exp_e kind);
    cheriot_mem_sys_alert_exp exp = cheriot_mem_sys_alert_exp::type_id::create("exp");
    exp.kind = kind;
    alert_exp_port.write(exp);
  endfunction

  // A reset of the subsystem: its registers and sweep go back, the memories stay.
  protected task track_reset();
    forever begin
      @(negedge cfg.clk_rst_vif.rst_n);
      core_fifo.flush();
      revbm_fifo.flush();
      corerevbm_fifo.flush();
      regs_fifo.flush();
      host_err_q.delete();
      core_lookups.delete();
      window_accesses.delete();
      core_meta_errs.delete();
      window_faults.delete();
      cms_cmodel_pkg::cms_dut_reset(model);
      fatal_sent = 1'b0;
    end
  endtask

endclass
