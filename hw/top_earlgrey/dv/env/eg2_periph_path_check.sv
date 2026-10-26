// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 path check (status/decisions.md 2026-10-24): the per-peripheral scoreboards take their
// expected items from each peripheral's own TL-UL port, so the interconnect between the core and
// that port must be checked on its own. Every core load and store to a peripheral's register
// window, as RVFI reports it (already checked against the instruction-set model), must arrive at
// that peripheral's port, and only there, with the same word address, byte enables, write data
// and error response, and a load must return there what the core received; in order, per
// peripheral. Compiled with EG2_PERIPH_SB only; on when the RVFI agent has an interface and the
// CPU runs (not in stub-CPU mode).
//
// What a misrouted access looks like: the access is missing at the port RVFI names and an access
// nobody made appears at another one; both are reported at the end of the run (`EG2_SB path
// <instance>: ...`), a different address or data as soon as both sides are there.
// Not checked: accesses by other hosts (rv_dm's system bus access; a test that uses it should not
// run with this check). A trapping load or store is taken from its bus transactions (R14,
// 2026-10-26), not its RVFI record, which has no masks after a bus error.
//
// +sb_corrupt=path.data: the write data (or read data) of the n-th access at a peripheral port
// (+sb_corrupt_at, default 1) is corrupted before it is compared; the check must report it as
// EXPECTED (REQ_STE_03).

class eg2_periph_path_check extends uvm_scoreboard;
  `uvm_component_utils(eg2_periph_path_check)

  typedef struct {
    bit          write;
    bit [31:0]   addr;   // word address
    bit [3:0]    mask;   // byte enables (write) or the bytes the load uses (read)
    bit [31:0]   data;   // write data or read data, in byte lanes
    bit          err;
    bit          addr_only;  // a capability access: address and direction only
    realtime     t;
    bit [63:0]   order;
    bit          corrupted;
    eg2_tl_err_e cls;    // the port's access against the documents' error prediction
  } acc_t;

  uvm_tlm_analysis_fifo #(eg2_rvfi_item) rvfi_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item)   tl_fifo[string];

  bit enabled;
  protected string       names[$];
  protected bit [31:0]   base[string], size[string];
  protected acc_t        core_q[string][$];
  protected acc_t        port_q[string][$];
  protected int unsigned n_core[string], n_port[string], n_match[string], n_mis[string];
  // Error responses the core saw, by the port's classification (eg2_tl_err).
  protected int unsigned n_err_seen[string];
  protected string       sb_corrupt;
  protected int unsigned corrupt_at = 1, n_port_all;
  protected bit          errors;

  `uvm_component_new

  // A peripheral port to check: its TL items go to tl_fifo[name].
  function void add_port(string name, bit [31:0] b, bit [31:0] s);
    names.push_back(name);
    base[name] = b;
    size[name] = s;
    n_core[name] = 0;
    n_port[name] = 0;
    n_match[name] = 0;
    n_mis[name] = 0;
    n_err_seen[name] = 0;
    tl_fifo[name] = new($sformatf("tl_fifo_%s", name), this);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    rvfi_fifo = new("rvfi_fifo", this);
    void'($value$plusargs("sb_corrupt=%s", sb_corrupt));
    void'($value$plusargs("sb_corrupt_at=%d", corrupt_at));
    if (corrupt_at == 0) corrupt_at = 1;
  endfunction

  function void start_of_simulation_phase(uvm_phase phase);
    super.start_of_simulation_phase(phase);
    if (enabled) eg2_inject_registry::register_check("path");
  endfunction

  virtual task run_phase(uvm_phase phase);
    if (!enabled) return;
    fork
      forever begin
        eg2_rvfi_item it;
        rvfi_fifo.get(it);
        take_core(it);
      end
    join_none
    foreach (names[i]) begin
      automatic string n = names[i];
      fork
        forever begin
          tl_seq_item t;
          tl_fifo[n].get(t);
          take_port(n, t);
        end
      join_none
    end
  endtask

  protected function string inst_of(bit [31:0] addr);
    foreach (names[i]) begin
      if (addr >= base[names[i]] && addr - base[names[i]] < size[names[i]]) return names[i];
    end
    return "";
  endfunction

  protected function void take_core(eg2_rvfi_item it);
    bit        store;
    bit [3:0]  m;
    bit [31:0] data;
    int        n, off, n0;
    string     inst;
    if (it.reset || it.irq_only) return;
    // A trapping load or store: its bus transactions as the core made them (it.mem_acc), each with
    // its own error response; none for a fault the core raised itself (PMP, CHERI), a half PMP
    // stopped absent. Its RVFI masks are 0 when the fault was a bus error (Ibex with the writeback
    // stage), so it was skipped before and its error charged to the next access. Review R14,
    // 2026-10-26.
    if (it.trap) begin
      foreach (it.mem_acc[i]) begin
        acc_t a;
        a.write     = it.mem_acc[i].store;
        a.addr      = it.mem_acc[i].addr;
        a.mask      = it.mem_acc[i].be;
        a.data      = a.write ? it.mem_acc[i].wdata : it.mem_acc[i].rdata;
        a.err       = it.mem_acc[i].err;
        a.addr_only = it.mem_is_cap;
        a.t         = $realtime;
        a.order     = it.order;
        push_core(inst_of(a.addr), a);
      end
      return;
    end
    store = it.mem_wmask != 0;
    m = store ? it.mem_wmask : it.mem_rmask;
    if (m == 0) return;
    inst = inst_of(it.mem_addr);
    if (inst == "") return;
    data = store ? it.mem_wdata : it.mem_rdata;
    if (it.mem_is_cap) begin
      // A capability load or store: two word accesses.
      for (int w = 0; w < 2; w++) begin
        acc_t a;
        a.write = store;
        a.addr = {it.mem_addr[31:2], 2'b00} + 4 * w;
        a.addr_only = 1'b1;
        a.err = it.mem_error;
        a.t = $realtime;
        a.order = it.order;
        push_core(inst_of(a.addr), a);
      end
      return;
    end
    n   = $countones(m);
    off = it.mem_addr[1:0];
    n0  = (off + n > 4) ? 4 - off : n;
    begin
      acc_t a;
      a.write = store;
      a.addr  = {it.mem_addr[31:2], 2'b00};
      a.mask  = 4'((1 << n0) - 1) << off;
      a.data  = data << (8 * off);
      a.err   = it.mem_error;
      a.t     = $realtime;
      a.order = it.order;
      push_core(inst, a);
    end
    if (n0 < n) begin
      acc_t a;
      a.write = store;
      a.addr  = {it.mem_addr[31:2], 2'b00} + 4;
      a.mask  = 4'((1 << (n - n0)) - 1);
      a.data  = data >> (8 * n0);
      a.err   = it.mem_error;
      a.t     = $realtime;
      a.order = it.order;
      push_core(inst_of(a.addr), a);
    end
  endfunction

  protected function void push_core(string inst, acc_t a);
    if (inst == "") return;
    n_core[inst]++;
    core_q[inst].push_back(a);
    match(inst);
  endfunction

  protected function void take_port(string inst, tl_seq_item t);
    acc_t a;
    a.write = t.is_write();
    a.addr  = {t.a_addr[31:2], 2'b00};
    a.mask  = t.a_mask;
    a.data  = a.write ? t.a_data : t.d_data;
    a.err   = t.d_error;
    a.t     = $realtime;
    a.cls   = eg2_tl_err::pure_class(inst, t);
    n_port[inst]++;
    n_port_all++;
    if (sb_corrupt == "path.data" && n_port_all == corrupt_at) begin
      int lane = 0;
      for (int b = 3; b >= 0; b--) if (a.mask[b]) lane = b;
      a.data[8 * lane] = ~a.data[8 * lane];
      a.corrupted = 1'b1;
      void'(eg2_inject_registry::announce("sb_corrupt path.data", "path",
                                          $sformatf("data of access %0d at %s's port corrupted",
                                                    n_port_all, inst), $realtime, $realtime));
    end
    port_q[inst].push_back(a);
    match(inst);
  endfunction

  protected function void match(string inst);
    while (core_q[inst].size() > 0 && port_q[inst].size() > 0) begin
      acc_t  c = core_q[inst].pop_front();
      acc_t  p = port_q[inst].pop_front();
      string why;
      bit [31:0] lanes;
      for (int b = 0; b < 4; b++) if (c.mask[b]) lanes[8 * b +: 8] = 8'hff;
      if (c.write != p.write) why = "direction";
      else if (c.addr != p.addr) why = "address";
      else if (c.err != p.err) why = "error response";
      else if (!c.addr_only && c.write && c.mask != p.mask) why = "byte enables";
      else if (!c.addr_only && !c.err && (c.data & lanes) != (p.data & lanes)) why = "data";
      if (why == "") begin
        n_match[inst]++;
        if (c.err) n_err_seen[inst]++;
        continue;
      end
      n_mis[inst]++;
      // The core must see the error response the port returned (its class: EXPECTED_ERR,
      // UNEXPECTED_ERR, MISSING_ERR or OK, as the peripheral's scoreboard reports it).
      mismatch(inst, $sformatf({" %s: the core's %s (order %0d) [%08h/%h]=%08h err %0d, at the ",
                                "port %s [%08h/%h]=%08h err %0d (TL %s) (%s differs)"}, inst,
                               c.write ? "store" : "load", c.order, c.addr, c.mask,
                               c.data & lanes, c.err, p.write ? "write" : "read", p.addr, p.mask,
                               p.data & lanes, p.err, p.cls.name(), why), p.corrupted ? p.t : c.t);
    end
  endfunction

  protected function void mismatch(string inst, string what, realtime t);
    string src = eg2_inject_registry::consult("path", t);
    if (src != "") begin
      `uvm_info(`gfn, $sformatf("EG2_SB path EXPECTED mismatch (injected by %s):%s", src, what),
                UVM_LOW)
    end else begin
      errors = 1'b1;
      `uvm_error(`gfn, $sformatf("EG2_SB path%s", what))
    end
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    if (!enabled) return;
    foreach (names[i]) begin
      string inst = names[i];
      if (core_q[inst].size() > 0) begin
        acc_t c = core_q[inst][0];
        mismatch(inst, $sformatf({" %s: %0d core accesses never arrived at its port (first: %s ",
                                  "[%08h] at order %0d)"}, inst, core_q[inst].size(),
                                 c.write ? "store" : "load", c.addr, c.order), c.t);
      end
      if (port_q[inst].size() > 0) begin
        acc_t p = port_q[inst][0];
        mismatch(inst, $sformatf({" %s: %0d accesses arrived at its port that the core did not ",
                                  "make to it (first: %s [%08h]=%08h)"}, inst,
                                 port_q[inst].size(), p.write ? "write" : "read", p.addr, p.data),
                 p.t);
      end
    end
  endfunction

  function void report_phase(uvm_phase phase);
    string s;
    super.report_phase(phase);
    if (!enabled) begin
      `uvm_info(`gfn, "EG2_SB path: off (no RVFI in this build, or stub-CPU mode)", UVM_LOW)
      return;
    end
    foreach (names[i]) begin
      string n = names[i];
      if (n_core[n] == 0 && n_port[n] == 0) continue;
      s = {s, $sformatf({"\n  %-10s %6d core accesses, %6d at the port, %6d matched, %0d ",
                         "mismatched, %0d error responses the core saw"},
                        n, n_core[n], n_port[n], n_match[n], n_mis[n], n_err_seen[n])};
    end
    `uvm_info(`gfn, {"EG2_SB path counts:", s}, UVM_LOW)
  endfunction
endclass
