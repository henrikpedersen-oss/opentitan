// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_base_vseq extends cip_base_vseq #(
    .RAL_T               (cheriot_mem_sys_regs_reg_block),
    .CFG_T               (cheriot_mem_sys_env_cfg),
    .COV_T               (cheriot_mem_sys_env_cov),
    .VIRTUAL_SEQUENCER_T (cheriot_mem_sys_virtual_sequencer)
  );
  `uvm_object_utils(cheriot_mem_sys_base_vseq)

  // The address map in use (cms_pkg: Earl Grey 2, or the cms testbench's small map in build mode
  // cms_map) and the windows of it the sequences use, preloaded with random words: part of the main
  // SRAM and of the NVM (tagged; the whole of them with the small map) and the untagged memory; the
  // meta SRAM is preloaded with zeros (tags 0, bitmap clear).
  localparam bit [31:0]   SramBase     = cms_pkg::MainSramBase;
  localparam bit [31:0]   NvmBase      = cms_pkg::NvmBase;
  localparam bit [31:0]   UntaggedBase = cms_pkg::UntaggedBase;
  localparam bit [31:0]   MetaBase     = cms_pkg::MetaBase;
  localparam int unsigned SramWin      = (cms_pkg::SramBytes <= 'h4000) ? cms_pkg::SramBytes : 'h2000;
  localparam int unsigned NvmWin       = (cms_pkg::NvmBytes <= 'h2000) ? cms_pkg::NvmBytes : 'h1000;
  localparam int unsigned UntaggedWin  = cms_pkg::UntaggedTop - cms_pkg::UntaggedBase;
  localparam int unsigned MetaTop      = cms_pkg::MetaTop - cms_pkg::MetaBase;

  // various knobs to enable certain routines
  bit do_cheriot_mem_sys_init = 1'b1;

  protected cheriot_mem_sys_mem_device_seq m_cored_h_seq, m_tbre_h_seq, m_meta_seq;
  protected bit               mem_side_started;
  // One core request is presented at a time until it is taken, so the tag queued for it is its own.
  protected semaphore         cored_issue = new(1);
  // A capability store to the NVM: the core sends nothing else until its second word is answered
  // (assumption A4). Other core accesses take one key, such a store takes both.
  protected semaphore         cored_excl = new(2);

  `uvm_object_new

  virtual task dut_init(string reset_kind = "HARD");
    super.dut_init();
    if (!mem_side_started) begin
      preload_memories();
      start_mem_side();
      mem_side_started = 1'b1;
    end
    if (do_cheriot_mem_sys_init) cheriot_mem_sys_init();
  endtask

  virtual task dut_shutdown();
    // check for pending CHERIoT operations and wait for them to complete
    // TODO
  endtask

  // setup basic CHERIoT features
  virtual task cheriot_mem_sys_init();
  endtask

  // A word behind the host ports, also given to the C model.
  protected function void preload(bit [31:0] addr, bit [31:0] word);
    cfg.data_mem.write(addr, word);
    if (cfg.cmodel_h != null) cms_cmodel_pkg::cms_backdoor_data(cfg.cmodel_h, addr, word);
  endfunction

  protected virtual function void preload_memories();
    for (int unsigned a = 0; a < SramWin; a += 4) preload(SramBase + a, $urandom());
    for (int unsigned a = 0; a < NvmWin; a += 4) preload(NvmBase + a, $urandom());
    for (int unsigned a = 0; a < UntaggedWin; a += 4) preload(UntaggedBase + a, $urandom());
    for (int unsigned a = 0; a < MetaTop; a += 4) cfg.meta_mem.write(MetaBase + a, 32'h0);
  endfunction

  // The devices behind the host ports answer from the memories, with random delays and no errors.
  protected virtual task start_mem_side();
    m_cored_h_seq = new_device_seq("m_cored_h_seq", cfg.data_mem);
    m_tbre_h_seq  = new_device_seq("m_tbre_h_seq", cfg.data_mem);
    m_meta_seq    = new_device_seq("m_meta_seq", cfg.meta_mem);
    // The main crossbar's errors, as the cms testbench's memories: its error range, and writes to
    // the NVM (read-only on the interconnect; a capability store reaches it as reads).
    m_cored_h_seq.err_lo = cms_pkg::DataErrBase; m_cored_h_seq.err_hi = cms_pkg::UntaggedTop;
    m_cored_h_seq.ro_lo  = cms_pkg::NvmBase;     m_cored_h_seq.ro_hi  = cms_pkg::NvmTop;
    m_tbre_h_seq.err_lo  = cms_pkg::DataErrBase; m_tbre_h_seq.err_hi  = cms_pkg::UntaggedTop;
    m_tbre_h_seq.ro_lo   = cms_pkg::NvmBase;     m_tbre_h_seq.ro_hi   = cms_pkg::NvmTop;
    fork
      m_cored_h_seq.start(p_sequencer.cored_h_sqr);
      m_tbre_h_seq.start(p_sequencer.tbre_h_sqr);
      m_meta_seq.start(p_sequencer.meta_sqr);
    join_none
    #0;
  endtask

  protected function cheriot_mem_sys_mem_device_seq new_device_seq(string name,
                                                                   mem_model_pkg::mem_model#(32) mem);
    cheriot_mem_sys_mem_device_seq s = cheriot_mem_sys_mem_device_seq::type_id::create(name);
    s.mem = mem;
    s.min_rsp_delay = 0;
    s.max_rsp_delay = 10;
    s.rsp_abort_pct = 0;
    s.d_error_pct = 0;
    `DV_CHECK_RANDOMIZE_FATAL(s)
    return s;
  endfunction

  // One TL-UL request on a host agent of the memory side; returns its response.
  protected task tl_access(tl_sequencer sqr, bit [31:0] addr, tlul_pkg::tl_a_op_e op,
                           bit [1:0] size, bit [3:0] mask, bit [31:0] data,
                           output tl_seq_item rsp, input bit cored_tag = 1'b0,
                           input bit cored = 1'b0);
    cheriot_mem_sys_tl_host_seq seq;
    `uvm_create_on(seq, sqr)
    seq.control_addr_alignment = 1'b1;
    seq.control_rand_size = 1'b1;
    seq.control_rand_opcode = 1'b1;
    `DV_CHECK_RANDOMIZE_WITH_FATAL(seq,
        addr   == local::addr;
        write  == (local::op != tlul_pkg::Get);
        opcode == local::op;
        size   == local::size;
        mask   == local::mask;
        data   == local::data;)
    if (cored) begin
      cored_issue.get();
      cfg.side_vif.push_tag(cored_tag);
      fork
        `uvm_send(seq)
        begin
          @(cfg.side_vif.a_taken);
          cored_issue.put();
        end
      join
    end else begin
      `uvm_send(seq)
    end
    rsp = seq.rsp;
  endtask

  // A core data access with its tag sideband.
  protected task core_access(bit [31:0] addr, tlul_pkg::tl_a_op_e op, bit [1:0] size,
                             bit [3:0] mask, bit [31:0] data, bit tag, output tl_seq_item rsp);
    cored_excl.get(1);
    tl_access(p_sequencer.cored_sqr, addr, op, size, mask, data, rsp, tag, 1'b1);
    cored_excl.put(1);
  endtask

  // A capability store: W0 then W1, both with tag `tag`. To the NVM it runs alone (A4).
  protected task core_store_cap(bit [31:0] addr, bit [31:0] w0, bit [31:0] w1, bit tag);
    tl_seq_item rsp;
    bit nvm = addr >= NvmBase && addr < cms_pkg::NvmTop;
    cored_excl.get(nvm ? 2 : 1);
    tl_access(p_sequencer.cored_sqr, addr, tlul_pkg::PutFullData, 2, 4'hf, w0, rsp, tag, 1'b1);
    tl_access(p_sequencer.cored_sqr, addr + 4, tlul_pkg::PutFullData, 2, 4'hf, w1, rsp, tag, 1'b1);
    cored_excl.put(nvm ? 2 : 1);
  endtask

  // A capability load: W0 then W1, hinted (tag 1 on the request).
  protected task core_load_cap(bit [31:0] addr);
    tl_seq_item rsp;
    cored_excl.get(1);
    tl_access(p_sequencer.cored_sqr, addr, tlul_pkg::Get, 2, 4'hf, 0, rsp, 1'b1, 1'b1);
    tl_access(p_sequencer.cored_sqr, addr + 4, tlul_pkg::Get, 2, 4'hf, 0, rsp, 1'b1, 1'b1);
    cored_excl.put(1);
  endtask

  protected function void set_cheriot_ena(prim_mubi_pkg::mubi4_t ena);
    cfg.side_vif.cheriot_ena = ena;
  endfunction

endclass : cheriot_mem_sys_base_vseq
