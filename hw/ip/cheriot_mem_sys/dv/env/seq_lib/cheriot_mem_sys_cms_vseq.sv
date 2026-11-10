// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The directed tests of the cms testbench (../../cms/tb/cms_tests.svh), ported to this UVM
// testbench: +cms_test=<name> picks one. Each test's body is the cms one; the helpers it calls are
// re-made below with the same names on top of the agents, and the C model (cheriot_mem_sys_cmodel_sb,
// +cheriot_mem_sys_cmodel=1) is the checker where the cms testbench used its scoreboard; check_tags()
// compares the meta SRAM with the model's image. The tests run with the cms testbench's small
// address map (build mode cms_map), as they fill and sweep whole regions.
class cheriot_mem_sys_cms_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_cms_vseq)

  typedef enum int {PortCore, PortRevbm, PortCoreRevbm, PortCsr} port_e;

  string       cms_test = "cms_smoke";
  int unsigned core_inflight, other_inflight;
  // Core requests are presented in the order the test issues them: a ticket each, taken when the
  // test issues it, served in order.
  protected int unsigned issue_ticket, issue_turn;

  `uvm_object_new

  task body();
    void'($value$plusargs("cms_test=%s", cms_test));
    `uvm_info(`gfn, $sformatf("cms test %s", cms_test), UVM_LOW)
    case (cms_test)
      "cms_smoke":                   t_smoke();
      "cms_tag_store_load":          t_tag_store_load();
      "cms_tag_clear_subword":       t_tag_clear_subword();
      "cms_cap_load_hint":           t_cap_load_hint();
      "cms_access_check":            t_access_check();
      "cms_mode_gating":             t_mode_gating();
      "cms_rmw_same_word":           t_rmw_same_word();
      "cms_rmw_core_tbre_same_word": t_rmw_core_tbre_same_word();
      "cms_tbre_sweep":              t_tbre_sweep();
      "cms_tbre_base_decode":        t_tbre_base_decode();
      "cms_tbre_csr":                t_tbre_csr();
      "cms_tbre_concurrent":         t_tbre_concurrent();
      "cms_tbre_snoop_window":       t_tbre_snoop_window();
      "cms_random":                  t_random();
      default: `uvm_fatal(`gfn, $sformatf("unknown cms test '%s'", cms_test))
    endcase
    wait_quiet();
    tbre_wait_idle();
  endtask

  // ---- reporting, as the cms testbench's ----
  protected function void cms_error(string src, string msg);
    `uvm_error(src, msg)
  endfunction

  protected function void cms_info(string src, string msg);
    `uvm_info(src, msg, UVM_LOW)
  endfunction

  protected task wait_cycles(int unsigned n);
    cfg.clk_rst_vif.wait_clks(n);
  endtask

  // ---- transactions ----
  protected function cms_txn_t mk(tlul_pkg::tl_a_op_e op, logic [31:0] addr, logic [1:0] size = 2'd2,
                                  logic [31:0] data = 32'h0, logic tag = 1'b0,
                                  logic [3:0] mask = 4'h0);
    cms_txn_t t;
    t = txn_new();
    t.opcode = op;
    t.addr   = (size == 2'd2) ? {addr[31:2], 2'b00} : (size == 2'd1 ? {addr[31:1], 1'b0} : addr);
    t.size   = size;
    t.mask   = (mask != 4'h0) ? mask : size_mask(t.addr, size);
    // Byte lanes follow the address (TL-UL): place sub-word data in its lanes.
    t.wdata  = (size == 2'd2) ? data : (data << (8 * t.addr[1:0]));
    t.tag    = tag;
    return t;
  endfunction

  // One request on a port, blocking until its response; a core request takes its turn first.
  protected task run(port_e p, cms_txn_t t, output cms_txn_t r);
    int unsigned tk;
    if (p == PortCore) tk = issue_ticket++;
    run_ticket(p, t, tk, r);
  endtask

  protected task run_ticket(port_e p, cms_txn_t t, int unsigned tk, output cms_txn_t r);
    tl_seq_item  rsp;
    tl_sequencer sqr;
    bit [31:0]   addr = t.addr;
    case (p)
      PortCore:      sqr = p_sequencer.cored_sqr;
      PortRevbm:     sqr = p_sequencer.revbm_sqr;
      PortCoreRevbm: sqr = p_sequencer.corerevbm_sqr;
      default: begin
        sqr = p_sequencer.tl_sequencer_h;
        addr = ral.default_map.get_base_addr() + t.addr[5:0];
      end
    endcase
    if (p == PortCore) begin
      core_inflight++;
      wait (issue_turn == tk);
      fork
        tl_access(sqr, addr, t.opcode, t.size, t.mask, t.wdata, rsp, t.tag, 1'b1);
        begin
          @(cfg.side_vif.a_taken);
          issue_turn++;
        end
      join
    end else begin
      other_inflight++;
      tl_access(sqr, addr, t.opcode, t.size, t.mask, t.wdata, rsp);
    end
    r = t;
    r.rdata = rsp.d_data;
    r.err   = rsp.d_error;
    r.rtag  = 1'b0;
    if (p == PortCore) begin
      bit rtag;
      cfg.side_vif.get_vseq_rsp_tag(rsp.d_source, rtag);
      r.rtag = rtag;
      core_inflight--;
    end else begin
      other_inflight--;
    end
  endtask

  // Non-blocking core request (its turn is taken now, in issue order).
  protected task core_issue(cms_txn_t t);
    int unsigned tk = issue_ticket++;
    fork
      begin
        cms_txn_t r;
        run_ticket(PortCore, t, tk, r);
      end
    join_none
    #0;
  endtask

  // Every port idle and every response handed to the C model.
  protected task wait_quiet();
    wait (core_inflight == 0 && other_inflight == 0);
    wait (m_cored_h_seq.req_q.size() == 0 && m_tbre_h_seq.req_q.size() == 0 &&
          m_meta_seq.req_q.size() == 0);
    wait_cycles(3);
  endtask

  protected task wait_core_quiet();
    wait (core_inflight == 0);
    wait_cycles(2);
  endtask

  protected task set_ena(prim_mubi_pkg::mubi4_t v);
    wait_cycles(1);
    set_cheriot_ena(v);
    wait_cycles(1);
  endtask

  protected task cheriot_on();
    set_ena(prim_mubi_pkg::MuBi4True);
  endtask

  // Core requests in flight no more than max_inflight.
  protected task throttle(int unsigned max_inflight = 8);
    wait (core_inflight <= max_inflight);
  endtask

  // Handshake timing of every port.
  protected task randomize_timing(bit aggressive);
    cfg.m_cored_agent_cfg.a_valid_delay_max     = aggressive ? 0 : $urandom_range(3);
    cfg.m_cored_agent_cfg.d_ready_delay_max     = aggressive ? 0 : $urandom_range(5);
    cfg.m_revbm_agent_cfg.a_valid_delay_max     = $urandom_range(4);
    cfg.m_revbm_agent_cfg.d_ready_delay_max     = $urandom_range(4);
    cfg.m_corerevbm_agent_cfg.a_valid_delay_max = $urandom_range(4);
    cfg.m_corerevbm_agent_cfg.d_ready_delay_max = $urandom_range(4);
    m_cored_h_seq.max_rsp_delay = aggressive ? 0 : $urandom_range(4);
    m_tbre_h_seq.max_rsp_delay  = $urandom_range(4);
    m_meta_seq.max_rsp_delay    = aggressive ? 0 : $urandom_range(3);
  endtask

  // ---- core ----
  protected task core_store(logic [31:0] addr, logic [1:0] size, logic [31:0] data,
                            output logic err);
    cms_txn_t r;
    run(PortCore, mk(tlul_pkg::PutFullData, addr, size, data), r);
    err = r.err;
  endtask

  protected task core_load(logic [31:0] addr, logic [1:0] size, logic hint,
                           output logic [31:0] data, output logic tag, output logic err);
    cms_txn_t r;
    run(PortCore, mk(tlul_pkg::Get, addr, size, 32'h0, hint), r);
    data = r.rdata; tag = r.rtag; err = r.err;
  endtask

  // A capability store: two word writes back to back, both carrying the tag; to the NVM, nothing
  // else is issued until the second word is answered (as Ibex does; the WTRC relies on it).
  protected task core_store_cap(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, logic tag);
    cms_txn_t t, r;
    core_issue(mk(tlul_pkg::PutFullData, {addr[31:3], 3'b000}, 2'd2, w0, tag));
    t = mk(tlul_pkg::PutFullData, {addr[31:3], 3'b100}, 2'd2, w1, tag);
    if (tag && in_nvm(addr)) run(PortCore, t, r);
    else core_issue(t);
  endtask

  // A capability's two words programmed into the NVM through the backdoor, as software does through
  // the NVM controller before it stores the capability (programmers_guide.md "Storing Capabilities
  // in the NVM"); skipped while a sweep runs.
  protected task nvm_program(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, output bit done);
    logic [31:0] ga = {addr[31:3], 3'b000};
    done = 0;
    if (!in_nvm(ga)) return;
    wait_core_quiet();
    if (cfg.cmodel_h != null && cms_cmodel_pkg::cms_sweep_active(cfg.cmodel_h) != 0) return;
    preload(ga, w0);
    preload(ga + 4, w1);
    done = 1;
  endtask

  protected task store_cap(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, logic tag,
                           bit prog = 1'b1);
    bit done;
    if (prog && tag && in_nvm(addr)) nvm_program(addr, w0, w1, done);
    core_store_cap(addr, w0, w1, tag);
  endtask

  // A capability load: two hinted word reads back to back; the tag is valid if both carry it.
  protected task core_load_cap(logic [31:0] addr, output logic tag);
    cms_txn_t r0, r1;
    int unsigned tk0 = issue_ticket++;
    int unsigned tk1 = issue_ticket++;
    fork
      run_ticket(PortCore, mk(tlul_pkg::Get, {addr[31:3], 3'b000}, 2'd2, 32'h0, 1'b1), tk0, r0);
      run_ticket(PortCore, mk(tlul_pkg::Get, {addr[31:3], 3'b100}, 2'd2, 32'h0, 1'b1), tk1, r1);
    join
    tag = r0.rtag && r1.rtag && !r0.err && !r1.err;
  endtask

  // A capability with base `base` (aligned down to 2^e) and a random address that decodes back to it.
  protected task make_cap(logic [31:0] base, int unsigned e, logic [5:0] perms,
                          output logic [31:0] w0, output logic [31:0] w1);
    longint unsigned span, off, b;
    int unsigned     ee;
    ee   = (e >= 24) ? 24 : e;
    b    = 64'(base) & ~((64'd1 << ee) - 1);
    span = 64'd1 << (ee + 9);
    off  = {$urandom, $urandom} % span;
    if (b + off > 64'hffff_ffff) off = 0;
    w0 = 32'(b + off);
    w1 = cap_meta(32'(b), ee, perms, 3'($urandom_range(7)), 9'($urandom));
    if (cap_base(w0, w1) != 32'(b))
      cms_error("tb", $sformatf("make_cap: base 0x%08x decodes as 0x%08x", 32'(b), cap_base(w0, w1)));
  endtask

  protected task make_kind_cap(logic [31:0] base, int unsigned kind, int unsigned e,
                               output logic [31:0] w0, output logic [31:0] w1);
    logic [5:0] p;
    case (kind)
      0: p = PermsMemRw;
      1: p = PermsMemRo;
      2: p = PermsExec;
      3: p = PermsSealing;
      4: p = PermsNone;
      default: p = PermsMemRw;
    endcase
    make_cap(base, e, p, w0, w1);
    if (kind == 5) w1[24:22] = 3'(1 + $urandom_range(6));
    else if (kind != 3) w1[24:22] = 3'b000;
  endtask

  // ---- revocation bitmap (through the software window) ----
  protected task revbm_write(int unsigned w, logic [31:0] data);
    cms_txn_t r;
    run(PortRevbm, mk(tlul_pkg::PutFullData, MetaRevbmBase + w * 4, 2'd2, data), r);
  endtask

  protected task revbm_set_base(logic [31:0] base, bit value);
    int unsigned b;
    logic [31:0] d;
    if (!revbm_index(base, b)) return;
    d = cfg.meta_mem.read(MetaRevbmBase + (b / 32) * 4);
    d[b % 32] = value;
    revbm_write(b / 32, d);
  endtask

  // ---- registers ----
  protected task csr_write(logic [5:0] off, logic [31:0] data, output logic err);
    cms_txn_t r;
    run(PortCsr, mk(tlul_pkg::PutFullData, 32'(off), 2'd2, data), r);
    err = r.err;
  endtask

  protected task csr_read(logic [5:0] off, output logic [31:0] data);
    cms_txn_t r;
    run(PortCsr, mk(tlul_pkg::Get, 32'(off), 2'd2), r);
    data = r.rdata;
  endtask

  // Poll TBRE_STATUS.busy until the sweep is over (programmers_guide.md step 5).
  protected task tbre_wait_idle(int unsigned max_polls = 100000);
    logic [31:0] status;
    for (int unsigned i = 0; i < max_polls; i++) begin
      csr_read(CsrTbreStatus, status);
      if (!status[StatusBusy]) return;
      wait_cycles($urandom_range(8));
    end
    cms_error("tb", "TBRE_STATUS.busy never cleared");
  endtask

  protected task tbre_sweep(logic [31:0] base, logic [31:0] num, bit wait_done = 1'b1);
    logic e;
    tbre_wait_idle();
    csr_write(CsrTbreBase, base, e);
    csr_write(CsrTbreNum, num, e);
    csr_write(CsrTbreStart, 32'h1, e);
    if (wait_done) tbre_wait_idle();
  endtask

  // The end of the running sweep by its tbre_done interrupt (programmers_guide.md "Tracking
  // Completed Sweeps"), with busy re-checked as a wake can be stale.
  protected task tbre_wait_intr(int unsigned max_cycles = 200000);
    logic [31:0] d;
    logic        e;
    csr_write(CsrIntrEnable, 32'h1, e);
    for (int unsigned w = 0; w < 4; w++) begin
      for (int unsigned i = 0; i < max_cycles && !cfg.intr_vif.pins[0]; i++) wait_cycles(1);
      if (!cfg.intr_vif.pins[0]) begin
        cms_error("tb", "tbre_done interrupt never raised");
        return;
      end
      csr_read(CsrTbreStatus, d);
      csr_write(CsrIntrState, 32'h1, e);
      if (!d[StatusBusy]) return;
      csr_read(CsrTbreStatus, d);
      if (!d[StatusBusy]) return;
    end
    cms_error("tb", "tbre_done interrupt raised four times with the engine still busy");
  endtask

  // ---- checks ----
  // The meta SRAM against the C model's image (bits a race left open are not compared).
  protected task check_tags(string why);
    int unsigned n;
    if (cfg.cmodel_h == null) return;
    wait_quiet();
    for (int unsigned off = 0; off < cms_cmodel_pkg::cms_meta_top(cfg.cmodel_h); off += 4) begin
      if (cms_cmodel_pkg::cms_check_meta_word(cfg.cmodel_h, off, cfg.meta_mem.read(MetaBase + off)) != 0) begin
        if (n++ < 8) cms_error("check_tags", $sformatf("%s: %s", why, cms_cmodel_pkg::cms_last_messages()));
      end
    end
  endtask

  protected task expect_no_alert(string why);
    if (cfg.fatal_alert_cnt != 0) cms_error("alert", $sformatf("%s: unexpected fatal_fault alert", why));
  endtask

  // ---- the cms tests' own helpers (cms_tests.svh) ----
  protected function bit rbit();
    return 1'($urandom_range(1));
  endfunction

  protected function logic [31:0] rand_granule(logic [31:0] base, logic [31:0] top);
    return base + 8 * $urandom_range((top - base) / 8 - 1);
  endfunction

  protected task heap_base(bit revoked, output logic [31:0] base);
    base = rand_granule(MainSramBase, MainSramTop);
    revbm_set_base(base, revoked);
  endtask

  protected task heap_base_peek(output logic [31:0] base);
    base = rand_granule(MainSramBase, MainSramTop);
  endtask

  protected task fill_caps(logic [31:0] base, int unsigned n);
    logic [31:0] w0, w1, cb;
    int unsigned cls;
    for (int unsigned i = 0; i < n; i++) begin
      cls = $urandom_range(9);
      case (cls)
        0: begin heap_base(1, cb); make_kind_cap(cb, 0, 0, w0, w1); end           // revoked
        1: begin heap_base(0, cb); make_kind_cap(cb, 0, 0, w0, w1); end           // live
        2: begin heap_base(1, cb); make_kind_cap(cb, 3, 0, w0, w1); end           // sealing: exempt
        3: begin heap_base(1, cb); make_kind_cap(cb, 5, 0, w0, w1); end           // sealed: revoked
        4: begin make_kind_cap(32'h0800_0000 + 8 * $urandom_range(255), 0, 0, w0, w1); end  // below
        5: begin make_kind_cap(NvmBase + 8 * $urandom_range(255), 0, 0, w0, w1); end         // above
        6: begin heap_base(1, cb); make_kind_cap(cb, $urandom_range(3), $urandom_range(3), w0, w1); end
        7: begin heap_base(1, cb); make_kind_cap(cb, 2, 0, w0, w1); end           // executable
        default: begin heap_base(1, cb); make_kind_cap(cb, 0, 0, w0, w1); end
      endcase
      // One in eight is data that merely looks like a revoked capability: tag 0.
      store_cap(base + 8 * i, w0, w1, ($urandom_range(7) != 0));
      throttle();
    end
    wait_quiet();
  endtask

  protected task load_all_caps(logic [31:0] base, int unsigned n);
    logic t;
    for (int unsigned i = 0; i < n; i++) core_load_cap(base + 8 * i, t);
  endtask

  // ---- the tests (cms_tests.svh, unchanged but for the testbench's handles) ----
  protected task t_smoke();
    logic [31:0] d, w0, w1;
    logic        t, e;
    // Reset values and quiet ePMP mode
    csr_read(CsrRegwen, d);
    if (d != 32'h1) cms_error("test", $sformatf("TBRE_REGWEN reset value 0x%08x", d));
    csr_read(CsrTbreStatus, d);
    if (d != 32'h0) cms_error("test", $sformatf("TBRE_STATUS reset value 0x%08x", d));
    csr_read(CsrTbreEpoch, d);
    if (d != 32'h0) cms_error("test", $sformatf("TBRE_EPOCH reset value 0x%08x", d));
    csr_read(CsrIntrState, d);
    if (d != 32'h0) cms_error("test", $sformatf("INTR_STATE reset value 0x%08x", d));
    csr_read(CsrIntrEnable, d);
    if (d != 32'h0) cms_error("test", $sformatf("INTR_ENABLE reset value 0x%08x", d));
    if (cfg.intr_vif.pins[0]) cms_error("test", "cfg.intr_vif.pins[0]_o high out of reset");
    csr_read(CsrTbreBase, d);
    csr_read(CsrTbreNum, d);
    make_cap(MainSramBase + 32'h100, 0, PermsMemRw, w0, w1);
    core_store_cap(MainSramBase, w0, w1, 1'b1);
    core_load_cap(MainSramBase, t);
    wait_quiet();
    if (m_meta_seq.n_reads + m_meta_seq.n_writes != 0)
      cms_error("test", "meta SRAM accessed in ePMP mode");
    if (t) cms_error("test", "tag returned in ePMP mode");
    // CHERIoT mode: tags stored, loaded, cleared by a data store
    cheriot_on();
    core_store_cap(MainSramBase + 8, w0, w1, 1'b1);
    wait_quiet();
    core_load_cap(MainSramBase + 8, t);
    if (!t) cms_error("test", "capability stored with tag 1 loaded with tag 0");
    core_store(MainSramBase + 12, 2'd2, 32'h1234_5678, e);
    core_load_cap(MainSramBase + 8, t);
    if (t) cms_error("test", "data store did not clear the tag");
    expect_no_alert("smoke");
  endtask
  protected task t_tag_store_load();
    logic [31:0] base[3], top[3], a, w0, w1;
    logic        t;
    int unsigned m0;
    cheriot_on();
    randomize_timing(0);
    base = '{MainSramBase, NvmBase, UntaggedBase};
    top  = '{MainSramTop, NvmTop, DataErrBase};
    for (int r = 0; r < 3; r++) begin
      m0 = m_meta_seq.n_reads + m_meta_seq.n_writes;
      // Boundaries and random granules. In the NVM a tagged store is verified against the NVM
      // contents: half of them are programmed first and set the tag, the others are answered with
      // d_error and leave it.
      for (int unsigned i = 0; i < 80; i++) begin
        a = (i == 0) ? base[r] : (i == 1) ? top[r] - 8 : rand_granule(base[r], top[r]);
        make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
        store_cap(a, w0, w1, rbit(), rbit() || i < 2);
        throttle();
      end
      wait_quiet();
      for (int unsigned i = 0; i < 80; i++) core_load_cap(rand_granule(base[r], top[r]), t);
      core_load_cap(base[r], t);
      core_load_cap(top[r] - 8, t);
      if (r == 2 && m_meta_seq.n_reads + m_meta_seq.n_writes != m0)
        cms_error("test", "untagged memory accesses reached the meta SRAM");
    end
    // Just outside the tagged regions: no tag
    core_store_cap(MainSramTop, w0, w1, 1'b1);
    core_store_cap(MainSramBase - 8, w0, w1, 1'b1);
    core_load_cap(MainSramTop, t);
    core_load_cap(MainSramBase - 8, t);
    check_tags("tag_store_load");
  endtask
  protected task t_tag_clear_subword();
    logic [31:0] g, w0, w1;
    logic        t, e;
    cheriot_on();
    for (int unsigned k = 0; k < 24; k++) begin
      g = MainSramBase + 32'h200 + 8 * 4 * k;
      make_cap(MainSramBase + 8 * k, 0, PermsMemRw, w0, w1);
      // The granule and both neighbours tagged
      core_store_cap(g - 8, w0, w1, 1'b1);
      core_store_cap(g,     w0, w1, 1'b1);
      core_store_cap(g + 8, w0, w1, 1'b1);
      wait_quiet();
      if (k < 8)       core_store(g + k, 2'd0, 32'h5a, e);                    // byte at each offset
      else if (k < 12) core_store(g + 2 * (k - 8), 2'd1, 32'h5a5a, e);        // half at each offset
      else if (k < 14) core_store(g + 4 * (k - 12), 2'd2, 32'h5a5a_5a5a, e);  // word
      else if (k < 20) begin                                                   // tlul_pkg::PutPartialData
        cms_txn_t r;
        logic [3:0] m;
        m = 4'($urandom_range(14, 1));
        run(PortCore, mk(tlul_pkg::PutPartialData, g + 4 * (k % 2), 2'd2, $urandom, 1'b0, m), r);
      end else begin
        // A store carrying tag 1 on one word only: the subsystem trusts the core's tag (the tag of
        // a capability is the tag of its last store), so the granule keeps tag 1 -- or gets it.
        cms_txn_t r;
        run(PortCore, mk(tlul_pkg::PutFullData, g + 4 * (k % 2), 2'd2, $urandom, 1'b1), r);
      end
      wait_quiet();
      core_load_cap(g - 8, t);
      if (!t) cms_error("test", $sformatf("store to 0x%08x cleared the tag of its neighbour below", g));
      core_load_cap(g + 8, t);
      if (!t) cms_error("test", $sformatf("store to 0x%08x cleared the tag of its neighbour above", g));
      core_load_cap(g, t);
      if (k < 20 && t) cms_error("test", $sformatf("sub-word/non-capability store kind %0d kept the tag", k));
    end
    check_tags("tag_clear_subword");
  endtask
  protected task t_cap_load_hint();
    logic [31:0] a, w0, w1, d;
    logic        t, e;
    cheriot_on();
    for (int unsigned i = 0; i < 64; i++) begin
      make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
      core_store_cap(MainSramBase + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    for (int unsigned pass = 0; pass < 3; pass++) begin
      randomize_timing(pass == 0);
      // Back-to-back capability loads, both words outstanding
      for (int unsigned i = 0; i < 40; i++) begin
        a = MainSramBase + 8 * $urandom_range(63);
        core_issue(mk(tlul_pkg::Get, a, 2'd2, 32'h0, 1'b1));
        core_issue(mk(tlul_pkg::Get, a + 4, 2'd2, 32'h0, 1'b1));
        // Interleaved non-capability loads: no tag, whatever the granule holds
        if (rbit()) core_issue(mk(tlul_pkg::Get, MainSramBase + 8 * $urandom_range(63), 2'd2));
        if (rbit()) core_issue(mk(tlul_pkg::Get, MainSramBase + $urandom_range(511), 2'd0));
        if ($urandom_range(3) == 0) core_issue(mk(tlul_pkg::Get, MainSramBase + 2 * $urandom_range(255), 2'd1));
        throttle(4);
      end
      wait_quiet();
    end
    // A hinted read of the first word alone, and of untagged memory
    core_load(MainSramBase, 2'd2, 1'b1, d, t, e);
    core_load(UntaggedBase, 2'd2, 1'b1, d, t, e);
    core_load_cap(UntaggedBase + 8, t);
    check_tags("cap_load_hint");
  endtask
  protected task t_access_check();
    cms_txn_t    t, r;
    logic [31:0] a;
    logic [1:0]  sz;
    port_e       p;
    cheriot_on();
    randomize_timing(0);
    for (int unsigned i = 0; i < 600; i++) begin
      // Mostly CHERIoT mode; sometimes another encoding
      if ($urandom_range(15) == 0) set_ena(prim_mubi_pkg::mubi4_t'($urandom_range(15)));
      else if (cfg.side_vif.cheriot_ena != prim_mubi_pkg::MuBi4True) cheriot_on();
      p  = rbit() ? PortRevbm : PortCoreRevbm;
      sz = 2'($urandom_range(2));
      case ($urandom_range(5))
        0:       a = MetaBase - 32'h40 + $urandom_range(32'h3f);
        1:       a = MetaNvmTagBase + $urandom_range(MetaTop - MetaNvmTagBase + 32'h3f);
        2:       a = MetaNvmTagBase - 4;
        default: a = MetaRevbmBase + $urandom_range(RevbmBytes - 1);
      endcase
      t = mk(tlul_pkg::Get, a, sz);
      case ($urandom_range(3))
        0:       t = mk(tlul_pkg::PutFullData, a, sz, $urandom);
        1:       t = mk(tlul_pkg::PutPartialData, a, sz, $urandom, 1'b0,
                        size_mask(t.addr, sz) & 4'($urandom_range(15, 1)));
        default: ;
      endcase
      if (t.opcode == tlul_pkg::PutPartialData && t.mask == 4'h0) t.mask = size_mask(t.addr, sz);
      run(p, t, r);
    end
    cheriot_on();
    check_tags("access_check: the tag regions are unreachable from the windows");
    expect_no_alert("access check (software-reachable errors raise no alert)");
  endtask
  protected task t_mode_gating();
    logic [31:0] w0, w1, d;
    logic        t, e;
    int unsigned m0, tr0;
    cms_txn_t    r;
    cheriot_on();
    for (int unsigned i = 0; i < 8; i++) begin
      make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
      core_store_cap(MainSramBase + 8 * i, w0, w1, 1'(i % 2));
    end
    revbm_write(0, 32'hdead_beef);
    wait_quiet();
    // Every value other than MuBi4True, including MuBi4False and the invalid encodings
    for (int unsigned v = 0; v < 16; v++) begin
      if (4'(v) == 4'(prim_mubi_pkg::MuBi4True)) continue;
      set_ena(prim_mubi_pkg::mubi4_t'(v));
      m0  = m_meta_seq.n_reads + m_meta_seq.n_writes;
      tr0 = m_tbre_h_seq.n_reads;
      for (int unsigned i = 0; i < 8; i++) core_load_cap(MainSramBase + 8 * i, t);
      core_store_cap(MainSramBase + 8 * 8, w0, w1, 1'b1);  // no tag set outside CHERIoT mode
      core_store(MainSramBase + 8, 2'd2, 32'h1, e);        // no tag cleared either
      run(PortRevbm, mk(tlul_pkg::Get, MetaRevbmBase), r);
      run(PortRevbm, mk(tlul_pkg::PutFullData, MetaRevbmBase + 4, 2'd2, 32'h1), r);
      run(PortCoreRevbm, mk(tlul_pkg::Get, MetaRevbmBase), r);
      tbre_sweep(MainSramBase, 16, 1'b1);                  // ignored
      wait_quiet();
      wait_cycles(50);
      if (m_meta_seq.n_reads + m_meta_seq.n_writes != m0)
        cms_error("test", $sformatf("meta SRAM accessed with cheriot_ena_i = %0h", v));
      if (m_tbre_h_seq.n_reads != tr0)
        cms_error("test", $sformatf("sweep started with cheriot_ena_i = %0h", v));
    end
    cheriot_on();
    run(PortRevbm, mk(tlul_pkg::Get, MetaRevbmBase), r);
    for (int unsigned i = 0; i < 9; i++) core_load_cap(MainSramBase + 8 * i, t);
    check_tags("mode_gating");
    expect_no_alert("mode gating (software-reachable errors raise no alert)");
  endtask
  protected task t_rmw_same_word();
    logic [31:0] blk, a, w0, w1;
    logic        t;
    int unsigned w_before;
    cheriot_on();
    for (int unsigned rep = 0; rep < 4; rep++) begin
      randomize_timing(rep < 2);
      blk = (rep < 3) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
      for (int unsigned i = 0; i < 300; i++) begin
        a = blk + 8 * $urandom_range(31);
        case ($urandom_range(5))
          0, 1: begin
            make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
            store_cap(a, w0, w1, rbit(), rbit());
          end
          2: core_issue(mk(tlul_pkg::PutFullData, a + 4 * rbit(), 2'd2, $urandom));
          3: core_issue(mk(tlul_pkg::PutFullData, a + $urandom_range(7), 2'd0, $urandom));
          default: begin
            core_issue(mk(tlul_pkg::Get, a, 2'd2, 32'h0, 1'b1));
            core_issue(mk(tlul_pkg::Get, a + 4, 2'd2, 32'h0, 1'b1));
          end
        endcase
        throttle(rep < 2 ? 4 : 8);
      end
      wait_quiet();
      check_tags($sformatf("rmw_same_word block 0x%08x", blk));
    end
    // The write-back is skipped when the tag already has its value.
    a = MainSramBase + 32'h40;
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    core_store_cap(a, w0, w1, 1'b1);
    wait_quiet();
    w_before = m_meta_seq.n_writes;
    core_store_cap(a, w0, w1, 1'b1);
    wait_quiet();
    if (m_meta_seq.n_writes != w_before)
      cms_error("test", $sformatf("re-storing tag 1 wrote the meta SRAM %0d time(s)",
                                  m_meta_seq.n_writes - w_before));
    core_issue(mk(tlul_pkg::PutFullData, a, 2'd2, 32'h0));
    wait_quiet();
    if (m_meta_seq.n_writes != w_before + 1)
      cms_error("test", $sformatf("clearing a tag wrote the meta SRAM %0d time(s), expected 1",
                                  m_meta_seq.n_writes - w_before));
    core_load_cap(a, t);
  endtask
  protected task t_rmw_core_tbre_same_word();
    logic [31:0] blk, cb, w0, w1;
    bit          done;
    cheriot_on();
    for (int unsigned rep = 0; rep < 8; rep++) begin
      randomize_timing(1'(rep % 2));
      blk = MainSramBase + 256 * (1 + $urandom_range(SramCaps / 32 - 2));
      // Lower half: tagged, revoked; upper half: tagged, never revoked (base in NVM)
      for (int unsigned i = 0; i < 16; i++) begin
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        core_store_cap(blk + 8 * i, w0, w1, 1'b1);
      end
      wait_quiet();
      done = 0;
      fork
        begin
          tbre_sweep(blk, 16, 1'b1);
          done = 1;
        end
        begin
          while (!done) begin
            make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
            core_store_cap(blk + 128 + 8 * $urandom_range(15), w0, w1, $urandom_range(3) != 0);
            throttle(2);
          end
        end
      join
      wait_quiet();
      check_tags($sformatf("core and TBRE on tag word 0x%08x", blk));
    end
  endtask
  protected task t_tbre_sweep();
    logic [31:0] b;
    int unsigned n;
    cheriot_on();
    randomize_timing(0);
    fill_caps(MainSramBase, SramCaps);
    // The whole SRAM, then random sub-ranges, then single capabilities at both ends
    tbre_sweep(MainSramBase, SramCaps);
    check_tags("whole-SRAM sweep");
    for (int unsigned i = 0; i < 6; i++) begin
      b = rand_granule(MainSramBase, MainSramTop);
      n = 1 + $urandom_range((MainSramTop - b) / 8 - 1);
      fill_caps(b, n);
      tbre_sweep(b, n);
      check_tags($sformatf("sweep of %0d from 0x%08x", n, b));
    end
    fill_caps(MainSramBase, 1);
    tbre_sweep(MainSramBase, 1);
    fill_caps(MainSramTop - 8, 1);
    tbre_sweep(MainSramTop - 8, 1);
    check_tags("single-capability sweeps");
    load_all_caps(MainSramBase, 128);
    // The NVM: the whole of it, a random sub-range, and single capabilities at both ends
    fill_caps(NvmBase, NvmCaps);
    tbre_sweep(NvmBase, NvmCaps);
    check_tags("whole-NVM sweep");
    b = rand_granule(NvmBase, NvmTop);
    n = 1 + $urandom_range((NvmTop - b) / 8 - 1);
    fill_caps(b, n);
    tbre_sweep(b, n);
    fill_caps(NvmBase, 1);
    tbre_sweep(NvmBase, 1);
    fill_caps(NvmTop - 8, 1);
    tbre_sweep(NvmTop - 8, 1);
    check_tags("NVM sweeps");
    load_all_caps(NvmBase, 64);
  endtask
  protected task t_tbre_base_decode();
    logic [31:0] slot, base, w0, w1;
    int unsigned exps[16], e, k;
    cheriot_on();
    exps = '{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 24};
    slot = MainSramBase + SramBytes / 2;  // capabilities stored in the upper half
    k = 0;
    foreach (exps[i]) begin
      e = exps[i];
      for (int unsigned v = 0; v < 4; v++) begin
        if (e >= 14) base = MainSramBase;
        else         base = MainSramBase + ((32'($urandom_range(((SramBytes / 2) >> e) - 1))) << e);
        case (v)
          0: revbm_set_base(base, 1);                                    // own bit set
          1: begin revbm_set_base(base, 0); revbm_set_base(base + 8, 1); end  // only the next bit
          2: begin revbm_set_base(base, 0); if (base > MainSramBase) revbm_set_base(base - 8, 1); end
          default: revbm_set_base(base, rbit());
        endcase
        make_cap(base, e, PermsMemRw, w0, w1);
        core_store_cap(slot + 8 * k, w0, w1, 1'b1);
        k++;
        throttle();
      end
    end
    // Bit selects 0 and 31 of bitmap words, and the last heap granule
    for (int unsigned w = 0; w < 4; w++) begin
      base = MainSramBase + 256 * $urandom_range(RevbmBytes / 4 - 1) + (1'(w % 2) ? 31 * 8 : 0);
      revbm_set_base(base, w < 2);
      make_cap(base, 0, PermsMemRw, w0, w1);
      core_store_cap(slot + 8 * k, w0, w1, 1'b1);
      k++;
    end
    revbm_set_base(MainSramTop - 8, 1);
    make_cap(MainSramTop - 8, 0, PermsMemRw, w0, w1);
    core_store_cap(slot + 8 * k, w0, w1, 1'b1); k++;
    make_cap(MainSramTop, 0, PermsMemRw, w0, w1);     // first base past the bitmap: out of range
    core_store_cap(slot + 8 * k, w0, w1, 1'b1); k++;
    wait_quiet();
    tbre_sweep(slot, k);
    check_tags("base decode");
    load_all_caps(slot, k);
  endtask
  protected task t_tbre_csr();
    logic [31:0] d, tr0;
    logic        e;
    cms_txn_t    r;
    cheriot_on();
    // Reset values
    csr_read(CsrRegwen, d);     if (d != 1) cms_error("test", "TBRE_REGWEN reset value");
    csr_read(CsrTbreStatus, d); if (d != 0) cms_error("test", "TBRE_STATUS reset value");
    csr_read(CsrTbreBase, d);   if (d != 0) cms_error("test", "TBRE_BASE_ADDR reset value");
    csr_read(CsrTbreNum, d);    if (d != 0) cms_error("test", "TBRE_NUM_CAPS reset value");
    csr_read(CsrTbreEpoch, d);  if (d != 0) cms_error("test", "TBRE_EPOCH reset value");
    csr_read(CsrTbreStart, d);
    csr_read(CsrAlertTest, d);  if (d != 32'h8000_0000) cms_error("test", "ALERT_TEST reset value");
    csr_read(CsrIntrTest, d);
    // Field widths (the scoreboard checks the read-back)
    csr_write(CsrTbreBase, 32'hffff_ffff, e); csr_read(CsrTbreBase, d);
    csr_write(CsrTbreNum, 32'hffff_ffff, e);  csr_read(CsrTbreNum, d);
    csr_write(CsrIntrEnable, 32'hffff_ffff, e); csr_read(CsrIntrEnable, d);
    csr_write(CsrIntrEnable, 32'h0, e);
    // Bus errors: writes that leave out a byte holding a field, unmapped offsets; writes to RO
    // registers (ignored)
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrTbreBase), 2'd2, 32'h0, 1'b0, 4'b0011), r);
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrTbreNum), 2'd1, 32'h0), r);
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrTbreStatus), 2'd2, 32'h300, 1'b0, 4'b0001), r);
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrTbreEpoch), 2'd2, 32'h0, 1'b0, 4'b0111), r);
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrIntrEnable), 2'd2, 32'h1, 1'b0, 4'b1110), r);
    // ALERT_TEST without the byte of regwen (bit 31): refused, so regwen stays set
    run(PortCsr, mk(tlul_pkg::PutPartialData, 32'(CsrAlertTest), 2'd2, 32'h0, 1'b0, 4'b0001), r);
    csr_read(CsrAlertTest, d);
    run(PortCsr, mk(tlul_pkg::Get, 32'h28), r);
    run(PortCsr, mk(tlul_pkg::PutFullData, 32'h3c, 2'd2, 32'h1), r);
    run(PortCsr, mk(tlul_pkg::PutFullData, 32'(CsrTbreStatus), 2'd2, 32'h1), r);
    run(PortCsr, mk(tlul_pkg::PutFullData, 32'(CsrTbreEpoch), 2'd2, 32'h5), r);
    run(PortCsr, mk(tlul_pkg::PutFullData, 32'(CsrRegwen), 2'd2, 32'h0), r);
    csr_read(CsrRegwen, d); if (d != 1) cms_error("test", "TBRE_REGWEN is writable");
    csr_read(CsrTbreEpoch, d); if (d != 0) cms_error("test", "TBRE_EPOCH is writable");
    fill_caps(MainSramBase, SramCaps);
    // Ignored starts: no sweep, busy stays low, start_err set (the scoreboard checks it on every
    // TBRE_STATUS read) until cleared
    tr0 = m_tbre_h_seq.n_reads;
    tbre_sweep(MainSramBase, 0);
    csr_read(CsrTbreStatus, d);
    if (d != 32'h100) cms_error("test", $sformatf("TBRE_STATUS 0x%08x after an ignored start", d));
    csr_write(CsrTbreStatus, 32'h100, e);
    csr_read(CsrTbreStatus, d); if (d != 0) cms_error("test", "TBRE_STATUS.start_err not cleared");
    tbre_sweep(MainSramBase - 8, 4);
    tbre_sweep(MainSramTop, 4);
    tbre_sweep(NvmBase - 8, 4);
    tbre_sweep(NvmTop, 4);
    tbre_sweep(32'h0, 4);
    set_ena(prim_mubi_pkg::MuBi4False);
    tbre_sweep(MainSramBase, 4);
    tbre_sweep(NvmBase, 4);
    cheriot_on();
    csr_write(CsrTbreStatus, 32'h100, e);
    csr_write(CsrTbreBase, MainSramBase, e);
    csr_write(CsrTbreNum, 4, e);
    csr_write(CsrTbreStart, 32'h0, e);
    csr_write(CsrTbreStart, 32'h2, e);
    wait_cycles(100);
    tbre_wait_idle();
    if (m_tbre_h_seq.n_reads != tr0) cms_error("test", "an ignored TBRE_START started a sweep");
    // A sweep past the top of its region ends there; TBRE_NUM_CAPS keeps the value written
    tbre_sweep(MainSramTop - 8 * 5, 1000);
    csr_read(CsrTbreNum, d); if (d != 1000) cms_error("test", "TBRE_NUM_CAPS changed by the clamp");
    tbre_sweep(MainSramBase + 8, 32'h7fff_ffff);
    fill_caps(NvmTop - 8 * 5, 5);
    tbre_sweep(NvmTop - 8 * 5, 1000);
    tbre_sweep(NvmBase + 8, 32'h7fff_ffff);
    check_tags("clamped sweeps");
    // While a long sweep runs: busy high, REGWEN low, epoch odd, the three registers locked
    fill_caps(MainSramBase, SramCaps);
    tbre_sweep(MainSramBase, SramCaps, 1'b0);
    csr_read(CsrTbreStatus, d); if (!d[0]) cms_error("test", "TBRE_STATUS.busy low during a sweep");
    csr_read(CsrRegwen, d);     if (d != 0) cms_error("test", "TBRE_REGWEN high during a sweep");
    csr_read(CsrTbreEpoch, d);  if (!d[0]) cms_error("test", "TBRE_EPOCH even during a sweep");
    csr_write(CsrTbreBase, MainSramBase + 32'h800, e);
    csr_write(CsrTbreNum, 3, e);
    csr_write(CsrTbreStart, 32'h1, e);                 // ignored: a second sweep would show
    csr_read(CsrTbreStatus, d);
    if (d != 32'h1) cms_error("test", $sformatf("TBRE_STATUS 0x%08x during a sweep (2)", d));
    tbre_wait_idle();
    csr_read(CsrTbreBase, d);
    if (d != MainSramBase) cms_error("test", $sformatf("TBRE_BASE_ADDR written while locked: 0x%08x", d));
    csr_read(CsrTbreNum, d);
    if (d != SramCaps) cms_error("test", $sformatf("TBRE_NUM_CAPS written while locked: %0d", d));
    csr_read(CsrRegwen, d); if (d != 1) cms_error("test", "TBRE_REGWEN low after the sweep");
    // The scoreboard marked BASE/NUM unknown; make them known again
    csr_write(CsrTbreBase, MainSramBase, e);
    csr_write(CsrTbreNum, SramCaps, e);
    wait_cycles(200);
    check_tags("locked registers");
    expect_no_alert("CSR test");
  endtask
  protected task t_tbre_concurrent();
    logic [31:0] b, a, w0, w1, cb;
    int unsigned n;
    bit          done;
    cheriot_on();
    for (int unsigned rep = 0; rep < 4; rep++) begin
      randomize_timing(1'(rep % 2));
      n = SramCaps / 4;
      b = MainSramBase + 8 * $urandom_range(SramCaps - n);
      fill_caps(b, n);
      done = 0;
      fork
        begin
          tbre_sweep(b, n, 1'b1);
          done = 1;
        end
        begin
          while (!done) begin
            a = rand_granule(MainSramBase, MainSramTop);
            case ($urandom_range(7))
              0, 1: begin core_issue(mk(tlul_pkg::Get, a, 2'd2, 32'h0, 1'b1)); core_issue(mk(tlul_pkg::Get, a + 4, 2'd2, 32'h0, 1'b1)); end
              2: begin   // a live capability, anywhere (strict: it must keep its tag)
                make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
                core_store_cap(a, w0, w1, 1'b1);
              end
              3: core_issue(mk(tlul_pkg::PutFullData, a + $urandom_range(7), 2'd0, $urandom));
              4: core_issue(mk(tlul_pkg::Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2'd2, 32'h0, 1'b1));
              5: begin heap_base(rbit(), cb); end   // bitmap write during the sweep
              default: core_issue(mk(tlul_pkg::Get, UntaggedBase + 4 * $urandom_range(255), 2'd2));
            endcase
            throttle(4);
          end
        end
      join
      wait_quiet();
      check_tags($sformatf("concurrent sweep %0d", rep));
    end
  endtask
  protected task t_tbre_snoop_window();
    logic [31:0] b, g, cb, w0, w1, f0, f1;
    logic        t;
    int unsigned k, n, kept_nvm, failed_nvm;
    bit          nvm;
    cheriot_on();
    kept_nvm   = 0;
    failed_nvm = 0;
    for (int unsigned rep = 0; rep < 64; rep++) begin
      randomize_timing(rep % 3 == 0);
      nvm = rep % 4 == 3;
      n   = 2 + $urandom_range(6);
      b   = nvm ? NvmBase + 8 * $urandom_range(NvmCaps - n)
                : MainSramBase + 8 * $urandom_range(SramCaps - n);
      // Revoked, tagged capabilities
      for (int unsigned i = 0; i < n; i++) begin
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        store_cap(b + 8 * i, w0, w1, 1'b1);
        throttle(2);
      end
      wait_quiet();
      k = $urandom_range(n - 1);
      g = b + 8 * k;
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, f0, f1);   // revoked as well
      tbre_sweep(b, n, 1'b0);
      wait_cycles($urandom_range(8 * k + 12));
      if (nvm) begin
        core_store_cap(g, f0, f1, 1'b1);   // differs from the NVM: d_error, nothing written
        failed_nvm++;
      end else begin
        case ($urandom_range(3))
          0:       core_store_cap(g, f0, f1, 1'b1);
          1:       core_issue(mk(tlul_pkg::PutFullData, g + 4, 2'd2, f1, 1'b1));   // upper word only, tag 1
          2:       core_issue(mk(tlul_pkg::PutFullData, g, 2'd2, f0, 1'b1));       // lower word only, tag 1
          default: core_issue(mk(tlul_pkg::PutFullData, g + 4 * rbit(), 2'd2, $urandom));
        endcase
      end
      tbre_wait_idle();
      wait_quiet();
      check_tags($sformatf("snoop window, sweep %0d", rep));
      if (nvm) begin
        core_load_cap(g, t);
        if (t) kept_nvm++;
      end
    end
    // Not judged here (the scoreboard does, from the document): a capability store to the NVM
    // that fails writes nothing, yet as a watched write it stops the clear.
    cms_info("test", $sformatf({"snoop window: %0d failed capability store(s) to the NVM, %0d ",
                                "revoked capability(ies) there still tagged after the sweep"},
                               failed_nvm, kept_nvm));
  endtask
  protected task t_random();
    int unsigned num_ops;
    logic [31:0] hot[5], a, w0, w1, cb, d;
    logic        e;
    bit          stop, accepted;
    int unsigned k;
    cms_txn_t    r;
    num_ops = 4000;
    void'($value$plusargs("num_ops=%d", num_ops));
    cheriot_on();
    foreach (hot[i]) hot[i] = (i < 4) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
    stop = 0;
    fork
      // Sweeps of random ranges (including ignored starts), with random gaps
      begin
        while (!stop) begin
          wait_cycles($urandom_range(3000, 200));
          if (stop) break;
          accepted = 1;
          case ($urandom_range(11))
            0:       begin tbre_sweep(MainSramBase, 0, 1'b0); accepted = 0; end
            1:       begin
                       // k = 0 starts at MainSramTop, outside the tagged regions: an ignored
                       // start, so no completion interrupt will come
                       k = $urandom_range(8);
                       tbre_sweep(MainSramTop - 8 * k, 64, 1'b0);
                       accepted = (k != 0);
                     end
            2, 3:    tbre_sweep(hot[4] + 8 * $urandom_range(31), 32 * (1 + $urandom_range(3)), 1'b0);
            default: tbre_sweep(hot[$urandom_range(3)], 32 * (1 + $urandom_range(3)), 1'b0);
          endcase
          // Wait for the end by polling or by the interrupt; read the epoch; now and then clear
          // the sticky start_err
          if (accepted && rbit()) tbre_wait_intr();
          else                    tbre_wait_idle();
          csr_read(CsrTbreEpoch, d);
          if ($urandom_range(3) == 0) csr_write(CsrTbreStatus, 32'h1 << StatusStartErr, e);
        end
      end
      begin
        for (int unsigned i = 0; i < num_ops; i++) begin
          if (i % 500 == 0) randomize_timing($urandom_range(3) == 0);
          a = ($urandom_range(3) != 0) ? hot[$urandom_range(4)] + 8 * $urandom_range(31)
                                       : rand_granule(MainSramBase, MainSramTop);
          case ($urandom_range(15))
            0, 1, 2: begin
              if (rbit()) heap_base_peek(cb); else cb = NvmBase + 8 * $urandom_range(255);
              make_kind_cap(cb, $urandom_range(5), $urandom_range(3), w0, w1);
              // To the NVM, programmed first half of the time: verified, or answered with d_error
              store_cap(a, w0, w1, $urandom_range(3) != 0, rbit());
            end
            3, 4, 5: begin
              core_issue(mk(tlul_pkg::Get, a, 2'd2, 32'h0, 1'b1));
              core_issue(mk(tlul_pkg::Get, a + 4, 2'd2, 32'h0, 1'b1));
            end
            6:  core_issue(mk(tlul_pkg::PutFullData, a + $urandom_range(7), 2'($urandom_range(2)), $urandom));
            7:  core_issue(mk(tlul_pkg::PutPartialData, a + 4 * rbit(), 2'd2, $urandom, 1'b0,
                              4'($urandom_range(15, 1))));
            8:  core_issue(mk(tlul_pkg::Get, a + $urandom_range(7), 2'($urandom_range(2))));
            9:  core_issue(mk(tlul_pkg::Get, UntaggedBase + 4 * $urandom_range(1023), 2'd2, 32'h0, rbit()));
            10: core_issue(mk(tlul_pkg::PutFullData, UntaggedBase + 4 * $urandom_range(1023), 2'd2, $urandom, rbit()));
            11: begin heap_base(rbit(), cb); end
            12: run(PortCoreRevbm, mk(tlul_pkg::Get, MetaRevbmBase + 4 * $urandom_range(RevbmBytes / 4 - 1)), r);
            13: run(PortRevbm, mk(tlul_pkg::Get, MetaBase + 4 * $urandom_range((MetaTop - MetaBase) / 4)), r);
            default: begin
              core_issue(mk(tlul_pkg::Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2'd2, 32'h0, 1'b1));
            end
          endcase
          throttle($urandom_range(8, 1));
        end
        stop = 1;
      end
    join
    wait_quiet();
    tbre_wait_idle();
    check_tags("random");
  endtask

endclass
