// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The reference model: the C model of the subsystem (dv/cmodel/cms_core.h, written from the
// design documents, through cms_cmodel_pkg), fed only by the monitors. For every access on the
// core data port, the two bitmap ports and the `regs` port it sends the scoreboard the response
// the documents allow, and it sends the alerts it expects. It never takes a value from the DUT;
// the one thing it follows is when a sweep has ended, which it learns from a register read as
// software does.
//
// What the subsystem receives on its host side is an input to it, and the model takes it from the
// device agents' monitors: the crossbar's error answer to each core access (one per access, in
// order), and the errors and integrity faults on the responses to the revocation engine and from
// the meta SRAM. Requester candidates come only from outstanding external requests and
// the independently modelled sweep. Where those ports cannot distinguish overlapping requests,
// each candidate retains every documented outcome; no internal source-ID encoding is inferred.
// Internal taps are optional print-only diagnostics and have no connection to this component.
`uvm_analysis_imp_decl(_core_present)
`uvm_analysis_imp_decl(_core_req)
`uvm_analysis_imp_decl(_core_rsp)
`uvm_analysis_imp_decl(_mode)
`uvm_analysis_imp_decl(_backdoor)
`uvm_analysis_imp_decl(_cored_h_rsp)
`uvm_analysis_imp_decl(_tbre_h_rsp)
`uvm_analysis_imp_decl(_meta_rsp)
`uvm_analysis_imp_decl(_csr_req)
`uvm_analysis_imp_decl(_window_req)
`uvm_analysis_imp_decl(_core_window_req)

class cheriot_mem_sys_ref_model extends uvm_component;
  `uvm_component_utils(cheriot_mem_sys_ref_model)

  cheriot_mem_sys_env_cfg cfg;

  // ---- From the monitors ----
  uvm_analysis_imp_core_present #(cheriot_mem_sys_core_item, cheriot_mem_sys_ref_model)
      core_present_imp;
  uvm_analysis_imp_core_req #(cheriot_mem_sys_core_item, cheriot_mem_sys_ref_model) core_req_imp;
  uvm_analysis_imp_core_rsp #(cheriot_mem_sys_core_item, cheriot_mem_sys_ref_model) core_rsp_imp;
  uvm_analysis_imp_mode #(prim_mubi_pkg::mubi4_t, cheriot_mem_sys_ref_model)        mode_imp;
  uvm_analysis_imp_backdoor #(cheriot_mem_sys_backdoor_item, cheriot_mem_sys_ref_model)
      backdoor_imp;
  uvm_analysis_imp_cored_h_rsp #(tl_seq_item, cheriot_mem_sys_ref_model) cored_h_rsp_imp;
  uvm_analysis_imp_tbre_h_rsp #(tl_seq_item, cheriot_mem_sys_ref_model)  tbre_h_rsp_imp;
  uvm_analysis_imp_meta_rsp #(tl_seq_item, cheriot_mem_sys_ref_model)    meta_rsp_imp;
  uvm_analysis_imp_csr_req #(tl_seq_item, cheriot_mem_sys_ref_model) csr_req_imp;
  uvm_analysis_imp_window_req #(tl_seq_item, cheriot_mem_sys_ref_model) window_req_imp;
  uvm_analysis_imp_core_window_req #(tl_seq_item, cheriot_mem_sys_ref_model) core_window_req_imp;
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

  uvm_analysis_port #(cheriot_mem_sys_cov_item) coverage_port;
  protected bit cov_read_denied, cov_other_fault;

  int unsigned completed_sweeps;
  protected bit fault_declared;

  protected chandle                model;
  protected prim_mubi_pkg::mubi4_t mode = prim_mubi_pkg::MuBi4False;
  // The crossbar's responses, including integrity codes, one per core access in order.
  protected tl_seq_item            host_rsp_q[$];
  // Possible owners recorded exclusively at the external request ports.
  protected cheriot_mem_sys_core_item core_pending[int unsigned];
  protected int unsigned core_ids[bit [top_pkg::TL_AIW-1:0]][$];
  protected int unsigned n_core_present;
  protected tl_seq_item window_pending[bit [top_pkg::TL_AIW:0]];
  protected tl_seq_item meta_inputs[$];
  protected bit core_meta_maybe[int unsigned];
  protected bit window_maybe[bit [top_pkg::TL_AIW:0]];
  // Faults from the external memory input, associated with all possible owners.
  protected bit                    core_meta_errs[int unsigned];
  protected bit                    core_meta_intg[int unsigned];
  protected tl_seq_item            window_faults[bit [top_pkg::TL_AIW:0]];
  protected bit early_start[bit [top_pkg::TL_AIW-1:0]];
  // The alerts already sent.
  protected bit                    fatal_sent;
  protected int                    alert_tests_sent;
  // The WTRC's capability sequence after a capability store's W1 is taken (theory_of_operation.md,
  // "WTRC", the FSM's CapW1 and CapTagWr), which the model, seeing only completed accesses, cannot
  // know: W1 taken (CapW1) until the crossbar answers W1's NVM read, then its tag write
  // (CapTagWr) until W1 is answered. Every core access is answered once on the crossbar port, in
  // order, so W1's NVM answer is the crossbar answer with W1's number.
  typedef enum bit [1:0] {WtrcOpen, WtrcW1, WtrcTagWr} wtrc_e;
  protected wtrc_e                 wtrc = WtrcOpen;
  protected int unsigned           n_core_taken, n_host_answers, w1_number;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    core_present_imp   = new("core_present_imp", this);
    core_req_imp       = new("core_req_imp", this);
    core_rsp_imp       = new("core_rsp_imp", this);
    mode_imp           = new("mode_imp", this);
    backdoor_imp       = new("backdoor_imp", this);
    cored_h_rsp_imp    = new("cored_h_rsp_imp", this);
    tbre_h_rsp_imp     = new("tbre_h_rsp_imp", this);
    meta_rsp_imp       = new("meta_rsp_imp", this);
    window_req_imp     = new("window_req_imp", this);
    core_window_req_imp = new("core_window_req_imp", this);
    csr_req_imp        = new("csr_req_imp", this);
    core_fifo          = new("core_fifo", this);
    revbm_fifo         = new("revbm_fifo", this);
    corerevbm_fifo     = new("corerevbm_fifo", this);
    regs_fifo          = new("regs_fifo", this);
    core_exp_port      = new("core_exp_port", this);
    revbm_exp_port     = new("revbm_exp_port", this);
    corerevbm_exp_port = new("corerevbm_exp_port", this);
    csr_exp_port       = new("csr_exp_port", this);
    alert_exp_port     = new("alert_exp_port", this);
    coverage_port = new("coverage_port", this);
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
        @(cfg.clk_rst_vif.cbn);
        uvm_wait_for_nba_region();
        process_meta_inputs();
        if (cfg.trvk_fault_arm && sweep_active()) begin
          cms_cmodel_pkg::cms_sweep_dont_care(model, cfg.trvk_fault_addr);
          if (!fault_declared) send_alert(AlertExpOptional);
          fault_declared = 1;
        end
      end
      forever begin
        cheriot_mem_sys_core_item it;
        core_fifo.get(it);
        // All monitors publish this clock edge before prediction consumes their inputs.
        // In particular, a combinational meta response can arrive with the core response.
        uvm_wait_for_nba_region();
        predict_core(it);
      end
      forever begin
        tl_seq_item it;
        revbm_fifo.get(it);
        uvm_wait_for_nba_region();
        predict_bitmap(it, 1'b0);
      end
      forever begin
        tl_seq_item it;
        corerevbm_fifo.get(it);
        uvm_wait_for_nba_region();
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

  // A request presented on the core data port, before it is taken. In CapW1 any request is out of
  // the capability sequence "as soon as it is presented"; in CapTagWr a W0 or W1 is (WTRC, "Out of
  // sequence"). Either raises fatal_fault ("Error Handling"). Ibex presents neither.
  function void write_core_present(cheriot_mem_sys_core_item it);
    // The documented tag/data paths may progress before the core request is accepted.
    // Track its first external a_valid presentation, not an internal acceptance tap.
    // A generation ID also separates source reuse from a previous response on the same edge.
    n_core_present++;
    core_pending[n_core_present] = it;
    core_ids[it.source].push_back(n_core_present);
    if (wtrc == WtrcW1 || (wtrc == WtrcTagWr && nvm_cap_store(it))) begin
      `uvm_info(`gfn, $sformatf("%s 0x%08x presented while a capability store's W1 is %s: %s",
                                it.opcode.name(), it.addr,
                                wtrc == WtrcW1 ? "taken (CapW1)" : "writing its tag (CapTagWr)",
                                "out of sequence"), UVM_MEDIUM)
      cms_cmodel_pkg::cms_raise_fatal(model);
      send_alerts();
    end
  endfunction

  function void write_core_req(cheriot_mem_sys_core_item it);
    n_core_taken++;
    if (nvm_cap_store(it) && it.addr[2]) begin
      wtrc      = WtrcW1;
      w1_number = n_core_taken;
    end
  endfunction

  function void write_core_rsp(cheriot_mem_sys_core_item it);
    if (nvm_cap_store(it) && it.addr[2]) wtrc = WtrcOpen;  // W1's answer closes the capability
  endfunction

  // A capability store to the NVM: a write in strict CHERIoT mode to the NVM with the tag set.
  protected function bit nvm_cap_store(cheriot_mem_sys_core_item it);
    return it.opcode != tlul_pkg::Get && it.ena == prim_mubi_pkg::MuBi4True && in_nvm(it.addr) &&
           it.tag;
  endfunction

  function void write_cored_h_rsp(tl_seq_item it);
    host_rsp_q.push_back(it);
    n_host_answers++;
    if (wtrc == WtrcW1 && n_host_answers >= w1_number) wtrc = WtrcTagWr;  // W1's NVM read answered
  endfunction

  // A response to the revocation engine: an error response leaves the capability alone and fails
  // the sweep; an integrity fault does too, and raises fatal_fault.
  function void write_tbre_h_rsp(tl_seq_item it);
    if (!it.d_error && tl_d_intg_ok(it)) return;
    if (it.d_error) cov_read_denied = 1;
    if (!tl_d_intg_ok(it)) cov_other_fault = 1;
    cms_cmodel_pkg::cms_sweep_read_error(model, it.a_addr);
    if (!it.d_error) cms_cmodel_pkg::cms_raise_fatal(model);
    send_alerts();
  endfunction

  protected function bit bitmap_request_legal(tl_seq_item it);
    return mode == prim_mubi_pkg::MuBi4True && it.a_addr >= MetaBase &&
           it.a_addr < MetaNvmTagBase && it.a_addr[1:0] == 0 && it.a_size == 2 &&
           it.a_mask == 4'hf && it.a_opcode inside {tlul_pkg::Get, tlul_pkg::PutFullData};
  endfunction

  function void write_window_req(tl_seq_item it);
    if (bitmap_request_legal(it)) window_pending[{1'b0, it.a_source}] = it;
  endfunction

  function void write_core_window_req(tl_seq_item it);
    if (bitmap_request_legal(it)) window_pending[{1'b1, it.a_source}] = it;
  endfunction

  // The meta agent observes a memory response supplied TO the DUT, an external input.
  // Defer until all request monitors have published this edge; never decode DUT source bits.
  function void write_meta_rsp(tl_seq_item it);
    meta_inputs.push_back(it);
  endfunction

  protected function void process_meta_inputs();
    while (meta_inputs.size() > 0) attribute_meta_fault(meta_inputs.pop_front());
  endfunction

  protected function void attribute_meta_fault(tl_seq_item it);
    bit [31:0] word = word_of(it.a_addr);
    bit [31:0] off = word - MetaBase;
    bit sweep_candidate;
    int candidates = 0, windows = 0;
    if (!it.d_error && tl_d_intg_ok(it)) return;
    sweep_candidate = cms_cmodel_pkg::cms_sweep_may_access_meta(model, off) != 0;
    if (sweep_candidate) candidates++;
    foreach (core_pending[s]) begin
      bit [31:0] tag_word;
      if (looks_up_tag(core_pending[s], tag_word) && tag_word == word) candidates++;
    end
    foreach (window_pending[k]) begin
      if (word_of(window_pending[k].a_addr) == word) begin candidates++; windows++; end
    end
    foreach (core_pending[s]) begin
      bit [31:0] tag_word;
      if (looks_up_tag(core_pending[s], tag_word) && tag_word == word) begin
        core_meta_errs[s] |= it.d_error;
        core_meta_intg[s] |= !tl_d_intg_ok(it);
        core_meta_maybe[s] |= candidates > 1;
      end
    end
    foreach (window_pending[k]) begin
      if (word_of(window_pending[k].a_addr) == word) begin
        window_faults[k] = it;
        window_maybe[k] |= candidates > 1;
      end
    end
    if (candidates > 1)
      `uvm_info(`gfn, $sformatf("external meta fault 0x%08x has %0d possible requesters; retaining documented alternatives", word, candidates), UVM_LOW)
    if (sweep_candidate) begin
      cov_other_fault = 1;
      cms_cmodel_pkg::cms_sweep_meta_fault(model, off, int'(candidates > 1));
    end
    // A tag/sweep fault must alert. A bitmap window fault has no specified alert requirement.
    if (windows == 0 && candidates > 0) cms_cmodel_pkg::cms_raise_fatal(model);
    else send_alert(AlertExpOptional);
    send_alerts();
  endfunction

  // ---- Predictions ----

  protected task predict_core(cheriot_mem_sys_core_item it);
    cheriot_mem_sys_rsp_exp exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    bit        host_err, meta_err, meta_intg, meta_maybe;
    tl_seq_item host_rsp;
    tlul_pkg::tl_d2h_t host_fields = '0;
    int        err, rtag_set;
    int unsigned request_id;
    if (!core_ids.exists(it.source) || core_ids[it.source].size() == 0)
      `uvm_fatal(`gfn, "completed core request was not observed on the external port")
    request_id = core_ids[it.source].pop_front();
    if (core_ids[it.source].size() == 0) core_ids.delete(it.source);
    wait (host_rsp_q.size() > 0);
    process_meta_inputs();
    host_rsp = host_rsp_q.pop_front();
    host_err = host_rsp.d_error;
    // [ToO] Tag Filter: data passes unchanged and the response integrity is adjusted by the
    // linear difference when a meta error sets d_error. Preserve both integrity syndromes.
    // A capability store to NVM is different: WTRC checks the input and constructs its own ack.
    if (!nvm_cap_store(it)) begin
      tlul_pkg::tl_d_user_t user_bits = host_rsp.d_user;
      host_fields.d_opcode = tlul_pkg::tl_d_op_e'(host_rsp.d_opcode);
      host_fields.d_size = host_rsp.d_size;
      host_fields.d_error = host_rsp.d_error;
      exp.rsp_intg_syndrome = user_bits.rsp_intg ^ tlul_pkg::get_rsp_intg(host_fields);
      exp.data_intg_syndrome = user_bits.data_intg ^ tlul_pkg::get_data_intg(host_rsp.d_data);
    end
    if (core_meta_maybe.exists(request_id)) begin
      meta_maybe = core_meta_maybe[request_id];
      core_meta_maybe.delete(request_id);
    end
    if (core_meta_errs.exists(request_id)) begin
      meta_err = core_meta_errs[request_id];
      core_meta_errs.delete(request_id);
    end
    if (core_meta_intg.exists(request_id)) begin
      meta_intg = core_meta_intg[request_id];
      core_meta_intg.delete(request_id);
    end
    // Ambiguous ownership predicts the successful branch, then explicitly allows error/tag
    // alternatives. Model memory is joined below; it never learns which branch the DUT took.
    if (meta_maybe) meta_err = 1'b0;
    cms_cmodel_pkg::cms_predict_core(model, int'(it.opcode), it.addr, int'(it.size), int'(it.mask),
                                     it.data, int'(it.tag), int'(it.ena), int'(host_err),
                                     int'(meta_err), int'(it.bad_cmd_intg || it.bad_data_intg), err, exp.rdata, exp.rdata_care, rtag_set);
    exp.err      = err[0];
    exp.rtag_set = rtag_set[1:0];
    if (meta_maybe) begin
      if (!err) exp.err_care = 1'b0;
      if (!it.is_write()) exp.rtag_set = 2'b11;
    end
    exp.what     = $sformatf("core %s 0x%08x size %0d mask %h data %08x tag %0d mode %h", it.opcode.name(),
                             it.addr, it.size, it.mask, it.data, it.tag, it.ena);
    // A separate relational comparison checks that observed tags agree across responses. It
    // stores comparison witnesses only: neither prediction nor model memory learns a DUT tag.
    if (!meta_maybe && cms_cmodel_pkg::cms_check_core_tag(model, int'(it.opcode), it.addr, int'(it.size),
          int'(it.mask), it.data, int'(it.tag), int'(it.ena), int'(host_err), int'(meta_err),
          int'(it.bad_cmd_intg || it.bad_data_intg), int'(it.err), int'(it.rtag)) != 0) exp.tag_relation_error = cms_cmodel_pkg::cms_last_messages();
    if (cfg.en_cov) cover_core(it);
    core_exp_port.write(exp);
    cms_cmodel_pkg::cms_update_core(model, int'(it.opcode), it.addr, int'(it.size), int'(it.mask),
                                    it.data, int'(it.tag), int'(it.ena), int'(host_err),
                                    meta_maybe ? 2 : int'(meta_err), int'(it.bad_cmd_intg || it.bad_data_intg));
    core_pending.delete(request_id);
    // Q43: an integrity-only RMW fault leaves the write-back decision unsettled. Keep that
    // uncertainty in the final tag comparison too; otherwise a later quiet-point comparison
    // would silently demand a write-back. d_error still has the documented abort semantics.
    if (meta_intg && !meta_err && it.is_write()) cms_cmodel_pkg::cms_open_tag(model, it.addr);
    send_alerts();
  endtask

  protected function void predict_bitmap(tl_seq_item it, bit core_port);
    cheriot_mem_sys_rsp_exp exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    bit [top_pkg::TL_AIW:0] key = {core_port, it.a_source};
    bit [31:0] off  = it.a_addr - MetaBase;
    int        err;
    process_meta_inputs();
    cms_cmodel_pkg::cms_predict_bitmap(model, int'(it.a_opcode), off, int'(it.a_size),
                                       int'(it.a_mask), int'(mode), err, exp.rdata,
                                       exp.rdata_care);
    exp.err  = err[0];
    exp.what = $sformatf("%s %s 0x%08x", core_port ? "corerevbm" : "revbm",
                         tlul_pkg::tl_a_op_e'(it.a_opcode), it.a_addr);
    // [ToO] Memory map and Error Handling: the bitmap window forwards the SRAM response.
    // An injected fault is an input, not permission to stop comparing. Preserve its error and
    // integrity syndromes; data without d_error still has to match the independent memory model.
    if (window_faults.exists(key)) begin
      tl_seq_item fault = window_faults[key];
      tlul_pkg::tl_d_user_t user_bits = fault.d_user;
      tlul_pkg::tl_d2h_t fields = '0;
      fields.d_opcode = tlul_pkg::tl_d_op_e'(fault.d_opcode);
      fields.d_size = fault.d_size;
      fields.d_error = fault.d_error;
      if (window_maybe.exists(key) && window_maybe[key]) begin
        if (fault.d_error && !exp.err) exp.err_care = 1'b0;
        exp.intg_optional = 1'b1;
      end else exp.err |= fault.d_error;
      exp.rsp_intg_syndrome = user_bits.rsp_intg ^ tlul_pkg::get_rsp_intg(fields);
      exp.data_intg_syndrome = user_bits.data_intg ^ tlul_pkg::get_data_intg(fault.d_data);
      window_faults.delete(key);
    end
    if (cfg.en_cov) cover_bitmap(it, core_port);
    if (core_port) corerevbm_exp_port.write(exp);
    else           revbm_exp_port.write(exp);
    // A refused write did not reach memory. An integrity fault on an otherwise successful
    // acknowledgement does not undo the write (as distinct from a device error).
    window_pending.delete(key);
    if (window_maybe.exists(key) && window_maybe[key] &&
        !exp.err_care && it.is_write()) begin
      cms_cmodel_pkg::cms_open_bitmap_write(model, off, it.a_data);
    end else if (!exp.err) begin
      cms_cmodel_pkg::cms_update_bitmap(model, int'(it.a_opcode), off, int'(it.a_size),
                                        int'(it.a_mask), it.a_data, int'(mode));
    end
    window_maybe.delete(key);
  endfunction

  // [ToO] Revocation Engine: an accepted START write starts the engine. Its bus response may
  // be delayed until after the first memory fault, so starting at that response would erase a
  // real sweep fault. Writes with valid command integrity apply START only when byte zero is enabled;
  // the response is still checked for an unexpected refusal. Other CSR accesses use the existing
  // completion path, and START must not be applied twice.
  function void write_csr_req(tl_seq_item it);
    if (it.is_write() && (it.a_addr & ~32'h3) == cfg.ral.tbre_start.get_address() &&
        tl_a_intg_ok(it)) begin
      cms_cmodel_pkg::cms_set_mode(model, int'(mode));
      if (cfg.en_cov) cover_start(it.a_mask[0] && it.a_data[0]);
      cms_cmodel_pkg::cms_update_csr_write(model, int'(cfg.ral.tbre_start.get_offset()),
                                           it.a_mask[0] ? (it.a_data & 32'h1) : 0);
      early_start[it.a_source] = 1'b1;
    end
  endfunction

  // A register access. A refused one (unmapped or bad command integrity) is not a register
  // access; a command integrity fault raises fatal_fault.
  protected function void predict_csr(tl_seq_item it);
    cheriot_mem_sys_rsp_exp exp;
    bit [31:0] off = {it.a_addr[5:2], 2'b00};
    if (!tl_a_intg_ok(it)) begin
      cms_cmodel_pkg::cms_raise_fatal(model);
      send_alerts();
    end
    if (it.is_write() && (it.a_addr & ~32'h3) == cfg.ral.tbre_start.get_address() &&
        early_start.exists(it.a_source)) begin
      early_start.delete(it.a_source);
      if (it.d_error) `uvm_error(`gfn, "valid TBRE_START write was refused")
      return;
    end
    if (it.d_error) return;
    cms_cmodel_pkg::cms_set_mode(model, int'(mode));
    if (it.is_write()) begin
      cms_cmodel_pkg::cms_update_csr_write(model, off, it.a_data);
      send_alerts();
      return;
    end
    process_meta_inputs();
    if (cfg.en_cov && sweep_active() &&
        ((off inside {32'h20, 32'h24} && !it.d_data[0]) ||
         (off == 32'h10 && it.d_data[0]))) cover_sweep_end();
    begin
      bit was_active = sweep_active();
      cms_cmodel_pkg::cms_sync_sweep_end(model, off, it.d_data);
      if (was_active && !sweep_active()) completed_sweeps++;
    end
    exp = cheriot_mem_sys_rsp_exp::type_id::create("exp");
    cms_cmodel_pkg::cms_predict_csr_read(model, off, exp.rdata, exp.rdata_care);
    if (!cms_cmodel_pkg::cms_csr_read_allowed(model, off, it.d_data))
      exp.tag_relation_error = "register value outside the model's allowed outcomes";
    exp.what = $sformatf("register read 0x%02x", off);
    csr_exp_port.write(exp);
  endfunction

  // ---- Coverage snapshots: read-only independent state, consumed only by env_cov ----
  protected function int unsigned cov_get(cms_cmodel_pkg::cms_cov_field_e field,
                                           bit [31:0] addr = 0);
    return cms_cmodel_pkg::cms_cov_get(model, int'(field), addr);
  endfunction

  protected function cheriot_mem_sys_cov_item cov_cap(bit [31:0] addr);
    cheriot_mem_sys_cov_item c = cheriot_mem_sys_cov_item::type_id::create("cap_cov");
    bit [31:0] w0 = cov_get(cms_cmodel_pkg::CovData, addr);
    bit [31:0] w1 = cov_get(cms_cmodel_pkg::CovData, addr + 4);
    bit [31:0] base = cap_base(w0, w1);
    c.kind = cheriot_mem_sys_cov_item::SweepCap;
    c.addr = addr;
    c.s_tag = 2'(cov_get(cms_cmodel_pkg::CovTagSet, addr));
    c.s_sealing = w1[29:28] == 0 && w1[27:25] != 0;
    c.s_base_class = (base >= MainSramBase && base < MainSramTop) ? 0 :
                     base < MainSramBase ? 1 : 2;
    c.s_bit = 2'(cov_get(cms_cmodel_pkg::CovBitmapSet, base));
    c.s_revoked = (c.s_sealing || c.s_base_class != 0 || c.s_tag == 2'b01 ||
                    c.s_bit == 2'b01) ? 2'b01 :
                   (c.s_tag == 2'b10 && c.s_bit == 2'b10) ? 2'b10 : 2'b11;
    c.s_exp = cap_exp(w1);
    c.s_corr = ((w0 >> c.s_exp) & 32'h1ff) < {23'b0, w1[8:0]};
    return c;
  endfunction

  protected function void cover_start(bit start);
    cheriot_mem_sys_cov_item c = cheriot_mem_sys_cov_item::type_id::create("start_cov");
    bit [31:0] base = cov_get(cms_cmodel_pkg::CovBase);
    int unsigned num = cov_get(cms_cmodel_pkg::CovNum);
    c.kind = cheriot_mem_sys_cov_item::TbreStart;
    c.st_outcome = !start ? 6 : sweep_active() ? 5 : num == 0 ? 1 :
                    base < MainSramBase ? 2 : !(region_of(base) inside {RegionSram, RegionNvm}) ? 3 :
                    mode != prim_mubi_pkg::MuBi4True ? 4 : 0;
    c.st_nvm = in_nvm(base);
    c.st_clamped = c.st_outcome == 0 && cov_get(cms_cmodel_pkg::CovCaps) < num;
    coverage_port.write(c);
    if (c.st_outcome == 0) begin
      cov_read_denied = 0;
      cov_other_fault = 0;
    end
  endfunction

  protected function void cover_sweep_end();
    cheriot_mem_sys_cov_item c;
    bit [31:0] base = cov_get(cms_cmodel_pkg::CovBase);
    int unsigned caps = cov_get(cms_cmodel_pkg::CovCaps);
    // These are model candidates before sweep settlement, not reconstructed internal decisions.
    for (int unsigned i = 0; i < caps; i++) coverage_port.write(cov_cap(base + 8*i));
    c = cheriot_mem_sys_cov_item::type_id::create("end_cov");
    c.kind = cheriot_mem_sys_cov_item::TbreEnd;
    c.e_failed = 2'(cov_get(cms_cmodel_pkg::CovFailedSet));
    c.e_denied = cov_read_denied && !cov_other_fault;
    c.e_nvm = in_nvm(base);
    c.e_intr_en = 1'(cov_get(cms_cmodel_pkg::CovIntrEnable));
    coverage_port.write(c);
  endfunction

  protected function void cover_core(cheriot_mem_sys_core_item it);
    cheriot_mem_sys_cov_item c;
    bit [31:0] base = cov_get(cms_cmodel_pkg::CovBase);
    int unsigned caps = cov_get(cms_cmodel_pkg::CovCaps);
    if (nvm_cap_store(it)) begin
      bit upper = it.addr[2];
      bit seq_bad = upper ? (!cov_get(cms_cmodel_pkg::CovWtrcOpen) ||
                         cov_get(cms_cmodel_pkg::CovWtrcAddr) != (it.addr & ~32'h7)) :
                         cov_get(cms_cmodel_pkg::CovWtrcOpen) != 0;
      bit full = it.opcode == tlul_pkg::PutFullData && it.size == 2 && it.mask == 4'hf;
      c = cheriot_mem_sys_cov_item::type_id::create("nvm_cov");
      c.kind = cheriot_mem_sys_cov_item::NvmCap;
      c.n_upper = upper;
      c.n_old_tag = 2'(cov_get(cms_cmodel_pkg::CovTagSet, it.addr));
      c.n_outcome = seq_bad ? 3 : !full ? 2 :
                      it.data != cov_get(cms_cmodel_pkg::CovData, it.addr) ? 1 : 0;
      coverage_port.write(c);
    end
    if (it.is_write() && sweep_active()) begin
      c = cov_cap(it.addr & ~32'h7);
      c.kind = cheriot_mem_sys_cov_item::SweepRace;
      c.addr = it.addr;
      c.r_kind = (it.addr >= base && ((it.addr - base) >> 3) < caps) ? 1 : 0;
      c.w_upper = it.addr[2];
      c.w_nvm = in_nvm(it.addr);
      // Race coverage records the independent candidate before the store. It does not decide
      // the snoop result or narrow the model's alternatives from an observed tag.
      c.w_rev = c.s_revoked;
      coverage_port.write(c);
    end
  endfunction

  protected function void cover_bitmap(tl_seq_item it, bit core_port);
    cheriot_mem_sys_cov_item c = cheriot_mem_sys_cov_item::type_id::create("meta_cov");
    c.kind = cheriot_mem_sys_cov_item::MetaAccess;
    c.a_port = core_port;
    c.a_write = it.is_write();
    c.a_outcome = mode != prim_mubi_pkg::MuBi4True ? 1 : it.a_addr < MetaBase ? 2 :
                    it.a_addr >= MetaNvmTagBase ? 3 : it.a_opcode == tlul_pkg::PutPartialData ? 6 :
                    it.a_addr[1:0] != 0 ? 4 : it.a_size != 2 ? 5 : 0;
    coverage_port.write(c);
  endfunction

  // ---- Helpers ----

  // Whether a core access looks up a tag, and the meta SRAM word of that tag.
  protected function bit looks_up_tag(cheriot_mem_sys_core_item it, output bit [31:0] word);
    word = tag_word_addr(it.addr);
    if (it.ena != prim_mubi_pkg::MuBi4True ||
        !((it.addr >= MainSramBase && it.addr < MainSramTop) || in_nvm(it.addr))) return 1'b0;
    if (it.opcode == tlul_pkg::Get) return it.tag && it.addr[2:0] == 0;
    // NVM capability stores verify W0 without a lookup; only a verified W1 can write a tag.
    return !nvm_cap_store(it) || it.addr[2];
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
      host_rsp_q.delete();
      core_pending.delete();
      core_ids.delete();
      n_core_present = 0;
      window_pending.delete();
      meta_inputs.delete();
      core_meta_maybe.delete();
      window_maybe.delete();
      core_meta_errs.delete();
      core_meta_intg.delete();
      early_start.delete();
      window_faults.delete();
      cms_cmodel_pkg::cms_dut_reset(model);
      fatal_sent     = 1'b0;
      fault_declared = 0;
      wtrc           = WtrcOpen;
      n_core_taken   = 0;
      n_host_answers = 0;
    end
  endtask

endclass
