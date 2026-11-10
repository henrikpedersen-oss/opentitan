// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The subsystem against its C reference model (dv/cms/cmodel/cms_core.h, written from the design
// specification, through cms_cmodel_pkg's DPI calls): every completed transaction on the core data
// port, both revocation bitmap ports and the `regs` port goes to the model, which says whether the
// response (data, tag, error) is allowed; at the end of the test the meta SRAM is compared with the
// model's image, word by word. On with +cheriot_mem_sys_cmodel=1.
//
// The model also needs what the subsystem saw from its host side: the crossbar's error answer to
// each core request (one per request, in order: the core's assumption A7), taken from the cored_tl_h
// device agent; and the tag and mode of each core request, from the sideband interface. The memory
// behind the host ports starts with the contents the sequences preload, and they give the model the
// same (cfg.cmodel_h).
class cheriot_mem_sys_cmodel_sb extends uvm_component;
  `uvm_component_utils(cheriot_mem_sys_cmodel_sb)

  cheriot_mem_sys_env_cfg cfg;

  uvm_tlm_analysis_fifo #(tl_seq_item) cored_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item) corerevbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item) revbm_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item) cored_h_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item) regs_fifo;
  uvm_tlm_analysis_fifo #(alert_agent_pkg::alert_seq_item) alert_fifo;  // fatal_fault

  protected chandle      h;
  protected bit          host_err_q[$];
  protected int unsigned n_core, n_bitmap, n_csr, n_meta_words, n_mismatch, n_reads;
  protected bit          corrupted;
  protected string       hist[$];  // the last transactions, for a mismatch report
  int unsigned           hist_len = 24;

  // The meta SRAM base of the address map in use (cms_pkg: Earl Grey 2, or the small cms_map).
  localparam bit [31:0] MetaBase = cms_pkg::MetaBase;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    cored_fifo     = new("cored_fifo", this);
    corerevbm_fifo = new("corerevbm_fifo", this);
    revbm_fifo     = new("revbm_fifo", this);
    cored_h_fifo   = new("cored_h_fifo", this);
    regs_fifo      = new("regs_fifo", this);
    alert_fifo     = new("alert_fifo", this);
`ifdef CHERIOT_MEM_SYS_CMS_MAP
    h = cms_cmodel_pkg::cms_new(1);  // the cms testbench's small map
`else
    h = cms_cmodel_pkg::cms_new(0);  // Earl Grey 2
`endif
    cfg.cmodel_h = h;  // the sequences give the model what they preload (cms_backdoor_data)
  endfunction

  protected function void note(string what);
    hist.push_back($sformatf("%0t %s", $time, what));
    if (hist.size() > hist_len) void'(hist.pop_front());
  endfunction

  protected function string join_hist();
    string s;
    foreach (hist[i]) s = {s, (i > 0) ? "\n    " : "", hist[i]};
    return s;
  endfunction

  protected function string op_name(bit [2:0] op);
    tlul_pkg::tl_a_op_e e = tlul_pkg::tl_a_op_e'(op);
    return e.name();
  endfunction

  protected function void report_mismatch(int n, string what);
    if (n == 0) return;
    n_mismatch += n;
    `uvm_error(`gfn, $sformatf("C model: %s: %s\n  the last %0d transactions, oldest first:\n    %s",
                               what, cms_cmodel_pkg::cms_last_messages(), hist.size(),
                               join_hist()))
  endfunction

  task run_phase(uvm_phase phase);
    fork
      forever begin
        tl_seq_item it;
        cored_h_fifo.get(it);
        host_err_q.push_back(it.d_error);
        note($sformatf("host  %s 0x%08x mask %h data %08x -> d_data %08x err %0d", op_name(it.a_opcode),
                       it.a_addr, it.a_mask, it.a_data, it.d_data, it.d_error));
      end
      forever begin
        tl_seq_item it;
        cored_fifo.get(it);
        check_core(it);
      end
      forever begin
        tl_seq_item it;
        revbm_fifo.get(it);
        check_bitmap(it, 1'b0);
      end
      forever begin
        tl_seq_item it;
        corerevbm_fifo.get(it);
        check_bitmap(it, 1'b1);
      end
      forever begin
        tl_seq_item it;
        regs_fifo.get(it);
        check_csr(it);
      end
      // A real alert, not the answer to the agent's ping (as cip_base_scoreboard counts them).
      forever begin
        alert_agent_pkg::alert_seq_item it;
        alert_fifo.get(it);
        if (it.m_trans_type == alert_agent_pkg::AlertSigTrans && !it.m_ping_timeout &&
            it.m_alert_handshake_sta == alert_agent_pkg::AlertReceived) begin
          cfg.fatal_alert_cnt++;
        end
      end
    join
  endtask

  protected task check_core(tl_seq_item it);
    bit                    tag, rtag, host_err;
    prim_mubi_pkg::mubi4_t ena;
    bit [31:0]             rdata = it.d_data;
    cfg.side_vif.get_req(it.a_source, tag, ena);
    cfg.side_vif.get_rsp_tag(it.d_source, rtag);
    wait (host_err_q.size() > 0);
    host_err = host_err_q.pop_front();
    if (it.a_opcode == tlul_pkg::Get && !it.d_error && it.a_mask != 0) begin
      n_reads++;
      if (cfg.cmodel_corrupt != 0 && n_reads == cfg.cmodel_corrupt) begin
        rdata[0] = !rdata[0];
        corrupted = 1'b1;
        `uvm_info(`gfn, $sformatf("C model: read %0d at 0x%08x handed to the model corrupted",
                                  n_reads, it.a_addr), UVM_LOW)
      end
    end
    n_core++;
    note($sformatf("core  %s 0x%08x size %0d mask %h data %08x tag %0d ena %h host_err %0d -> d_data %08x err %0d rtag %0d",
                   op_name(it.a_opcode), it.a_addr, it.a_size, it.a_mask, it.a_data, tag, ena, host_err,
                   rdata, it.d_error, rtag));
    report_mismatch(cms_cmodel_pkg::cms_core(h, int'(it.a_opcode), it.a_addr, int'(it.a_size),
                                    int'(it.a_mask), it.a_data, int'(tag), int'(ena),
                                    int'(host_err), 0, int'(it.d_error), rdata, int'(rtag)),
           $sformatf("core %s 0x%08x", op_name(it.a_opcode), it.a_addr));
  endtask

  protected function void check_bitmap(tl_seq_item it, bit core_port);
    n_bitmap++;
    report_mismatch(cms_cmodel_pkg::cms_bitmap(h, int'(core_port), int'(it.a_opcode), it.a_addr - MetaBase,
                                      int'(it.a_size), int'(it.a_mask), it.a_data,
                                      int'(cfg.side_vif.cheriot_ena), int'(it.d_error), it.d_data),
           $sformatf("%s %s 0x%08x", core_port ? "corerevbm" : "revbm", op_name(it.a_opcode),
                     it.a_addr));
  endfunction

  protected function void check_csr(tl_seq_item it);
    // The register's offset: the RAL agent reads with random low address bits.
    bit [5:0] off = {it.a_addr[5:2], 2'b00};
    bit       write = it.a_opcode != tlul_pkg::Get;
    cms_cmodel_pkg::cms_set_mode(h, int'(cfg.side_vif.cheriot_ena));
    // The end of a sweep, as the bench sees it: TBRE_STATUS.busy or TBRE_EPOCH.active read 0, or
    // TBRE_REGWEN read 1, while the model's sweep runs.
    if (!write && cms_cmodel_pkg::cms_sweep_active(h) != 0 &&
        ((off == cms_cmodel_pkg::CmsTbreStatus && !it.d_data[0]) ||
         (off == cms_cmodel_pkg::CmsTbreEpoch && !it.d_data[0]) ||
         (off == cms_cmodel_pkg::CmsTbreRegwen && it.d_data[0]))) begin
      cms_cmodel_pkg::cms_sweep_end(h);
    end
    if (it.d_error) return;  // refused (unmapped, partial write, integrity): not a register access
    n_csr++;
    note($sformatf("csr   %s 0x%02x data %08x -> %08x", write ? "write" : "read", off, it.a_data, it.d_data));
    report_mismatch(cms_cmodel_pkg::cms_csr(h, int'(write), 32'(off), it.a_data, it.d_data),
           $sformatf("csr %s 0x%02x", write ? "write" : "read", off));
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    // The meta SRAM against the model's image (bits a race left open are not compared).
    for (int unsigned off = 0; off < cms_cmodel_pkg::cms_meta_top(h); off += 4) begin
      if (!cfg.meta_mem.addr_exists(MetaBase + off)) continue;
      n_meta_words++;
      report_mismatch(cms_cmodel_pkg::cms_check_meta_word(h, off, cfg.meta_mem.read(MetaBase + off)),
             "meta SRAM at the end of the test");
    end
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"C model: %0d core, %0d bitmap, %0d CSR transactions and %0d meta SRAM ",
                              "words compared, %0d mismatches%s"}, n_core, n_bitmap, n_csr,
                              n_meta_words, n_mismatch, corrupted ? " (one read corrupted)" : ""),
              UVM_LOW)
    if (cfg.cmodel_corrupt != 0 && !corrupted) begin
      `uvm_error(`gfn, $sformatf("C model: the corruption at read %0d never happened (%0d reads)",
                                 cfg.cmodel_corrupt, n_reads))
    end
  endfunction

endclass
