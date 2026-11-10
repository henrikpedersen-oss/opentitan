// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Block-level tests of the Earl Grey 2 verification specification
// (eg2_top_verification/eg2_toplevel_verification_spec.md, "Memory Subsystem (REQ_CMS)", block
// level), included in cms_tb after cms_tests.svh:
//
//   cms_tag_sideband_mubi  REQ_CMS_01  the core tag sideband is MuBi4 in both directions
//   cms_lockstep_glitch    REQ_CMS_02  the subsystem, TBRE included, runs in lockstep
//   cms_latency            REQ_CMS_07  the cycles the subsystem adds to a core access
//   cms_trvk_fault         REQ_CMS_02  a single fault in the TBRE's load filter raises fatal_fault
//                          (REQ_CIB_03's rule for the core's filter, applied to this copy)
//
// Their checks follow the requirement rows and RFC CHERIoT Memory Subsystem, the sections the rows
// cite, never the RTL. Where the RTL lacks what a row requires, the test says so with a
// CMS_ERROR naming the ticket (eg2_top_verification status/rtl_tickets.md) and does not weaken
// the check. Each has a must-fail run in cms_regress.list: +cms_sb_corrupt=<kind>,1 breaks the
// test's first expected value of that kind (mubi, lockstep, latency), which prints the
// "CMS_CORRUPT ... corrupted <kind>" line before any error.

// ---------------------------------------------------------------------------------------------
// REQ_CMS_01: cms_tag_sideband_mubi
// ---------------------------------------------------------------------------------------------
//
// RFC "Transferring the Capability Tag Bit": the tag is routed "as an FI-hardened MUBI signal
// alongside the TLUL bus structs"; "End-To-End ECC and Lockstep Operation": the subsystem runs in
// lockstep. The row: a tag is stored only for MuBi4True; any encoding other than True or False
// sets no tag and raises fatal_fault.
//
// Stimulus: a capability store (CSC: two word writes) and a capability load (CLC: two hinted word
// reads) presenting each of the 16 request encodings on both words, and a CLC whose two responses
// carry each of the 14 invalid encodings, over a granule tagged 0 and one tagged 1.
// Checks:
//   valid encodings  MuBi4True stores and returns the tag, MuBi4False is a data access (the CSC
//                    clears the tag, the CLC returns none); no alert; the scoreboard on every
//                    response and the meta SRAM.
//   invalid request  fatal_fault; no tag set (a CSC over tag 0 leaves tag 0 in the meta SRAM; a
//                    CSC over tag 1 may keep or clear it, the row says only "sets no tag"); a CLC
//                    returns no tag; d_error either way (not stated). The CSC writes the words
//                    the granule already holds, so the data is right whether or not it is written.
//   corrupted rsp    the subsystem's response tag is forced to an invalid encoding for both
//                    words of a CLC: fatal_fault (the encoding is made inside the subsystem's
//                    lockstep domain, whose divergence the row of REQ_CMS_02 makes fatal); the
//                    meta SRAM is unchanged.
//   always (cms_tb)  with CMS_TAG_MUBI, every response carries MuBi4True or MuBi4False.
// The pinned RTL's sideband is one bit each way (TODO(#31066)): only the two valid encodings can be
// presented; the test then fails with the gap (upstream TODO(#31066)). Build with
// +define+CMS_TAG_MUBI once the RTL has MuBi4 ports; CMS_TAG_D2H_PATH names the signal the
// response corruption forces (default: the DUT's output port).

`ifndef CMS_TAG_D2H_PATH
`define CMS_TAG_D2H_PATH u_dut.cored_tag_d2h_o
`endif

int unsigned tsm_cov_enc, tsm_cov_op;  // op: 0 CSC, 1 CLC, 2 response with tag 0, 3 with tag 1
covergroup cms_tag_mubi_cg;
  option.per_instance = 1;
  cp_enc: coverpoint tsm_cov_enc { bins enc[] = {[0:15]}; }
  cp_op:  coverpoint tsm_cov_op { bins csc = {0}; bins clc = {1}; bins rsp_tag0 = {2}; bins rsp_tag1 = {3}; }
  // The response is only ever corrupted to an invalid encoding
  x_enc_op: cross cp_enc, cp_op {
    ignore_bins rsp_valid = binsof(cp_op) intersect {2, 3} && binsof(cp_enc) intersect {6, 9};
  }
endgroup
cms_tag_mubi_cg tsm_cg = new();

function automatic bit tsm_invalid(int unsigned v);
  return 4'(v) != 4'(prim_mubi_pkg::MuBi4True) && 4'(v) != 4'(prim_mubi_pkg::MuBi4False);
endfunction

// The two word accesses of one capability at granule address g, back to back as Ibex issues
// them, presenting `enc` on the request tag sideband of both. The scoreboard sees the tag the
// encoding stands for: 1 for MuBi4True only.
task automatic tsm_pair(bit store, logic [31:0] g, logic [31:0] w0, logic [31:0] w1,
                        prim_mubi_pkg::mubi4_t enc, output cms_txn_t r0, output cms_txn_t r1);
  int unsigned     id0, id1;
  longint unsigned t0;
  logic            t;
  t  = (enc == prim_mubi_pkg::MuBi4True);
  r0 = txn_new();
  r1 = txn_new();
  tag_mubi_ovr    = enc;
  tag_mubi_ovr_en = 1'b1;
  if (store) begin
    id0 = u_core.issue(mk(PutFullData, g, 2'd2, w0, t));
    id1 = u_core.issue(mk(PutFullData, g + 4, 2'd2, w1, t));
  end else begin
    id0 = u_core.issue(mk(Get, g, 2'd2, 32'h0, t));
    id1 = u_core.issue(mk(Get, g + 4, 2'd2, 32'h0, t));
  end
  t0 = cms_cycle;
  while (!(u_core.rsp_by_id.exists(id0) && u_core.rsp_by_id.exists(id1))) begin
    @(posedge clk);
    if (cms_cycle - t0 > 20000 || !rst_n) begin
      cms_error("test", $sformatf("no response to the capability %s at 0x%08x (encoding 0x%0h)",
                                  store ? "store" : "load", g, enc));
      break;
    end
  end
  if (u_core.rsp_by_id.exists(id0)) begin r0 = u_core.rsp_by_id[id0]; u_core.rsp_by_id.delete(id0); end
  if (u_core.rsp_by_id.exists(id1)) begin r1 = u_core.rsp_by_id[id1]; u_core.rsp_by_id.delete(id1); end
  tag_mubi_ovr_en = 1'b0;
  repeat (2) @(posedge clk);
endtask

// A granule of the main SRAM holding a capability with tag `tag`; its words in w0/w1.
task automatic tsm_setup(logic [31:0] g, logic tag, output logic [31:0] w0, output logic [31:0] w1);
  make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
  core_store_cap(g, w0, w1, tag);
  wait_quiet();
endtask

`ifdef CMS_TAG_MUBI
`ifndef VERILATOR
// Response corruption: while tsm_rsp_corrupt_n > 0, each core response is presented with the
// encoding tsm_rsp_bad, forced from the falling edge before the handshake to the one after.
prim_mubi_pkg::mubi4_t tsm_rsp_bad = prim_mubi_pkg::MuBi4False;
int unsigned           tsm_rsp_corrupt_n = 0;
bit                    tsm_rsp_forced = 1'b0;
always @(negedge clk) begin
  if (tsm_rsp_forced) begin
    release `CMS_TAG_D2H_PATH;
    tsm_rsp_forced = 1'b0;
  end
  if (rst_n && tsm_rsp_corrupt_n > 0 && core_d2h.d_valid && core_h2d.d_ready) begin
    force `CMS_TAG_D2H_PATH = tsm_rsp_bad;
    tsm_rsp_forced     = 1'b1;
    tag_d2h_corrupting = 1'b1;
    tsm_rsp_corrupt_n--;
  end else begin
    tag_d2h_corrupting = 1'b0;
  end
end
`endif
`endif

task automatic t_tag_sideband_mubi();
  logic [31:0]           g, w0, w1, n0, n1;
  cms_txn_t              r0, r1;
  prim_mubi_pkg::mubi4_t enc;
  logic                  got, exp;
  string                 ctx;
  int unsigned           k;
  cheriot_on();
  k = 0;

  // Valid encodings: CSC over a granule tagged 0 and one tagged 1, then CLC with MuBi4True and
  // with the same encoding.
  for (int unsigned pre = 0; pre < 2; pre++) begin
    for (int unsigned e = 0; e < 2; e++) begin
      enc = (e == 0) ? prim_mubi_pkg::MuBi4True : prim_mubi_pkg::MuBi4False;
      g   = MainSramBase + 32'h100 + 8 * k++;
      tsm_setup(g, 1'(pre), w0, w1);
      make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, n0, n1);
      tsm_pair(1'b1, g, n0, n1, enc, r0, r1);
      tsm_cov_enc = 32'(enc); tsm_cov_op = 0; tsm_cg.sample();
      wait_quiet();
      tsm_pair(1'b0, g, 0, 0, prim_mubi_pkg::MuBi4True, r0, r1);
      ctx = $sformatf("CSC with request encoding 0x%0h over tag %0d at 0x%08x, then CLC", enc, pre, g);
      exp = (enc == prim_mubi_pkg::MuBi4True);
      if (sb.corrupt("mubi", ctx)) exp = !exp;
      got = r0.rtag && r1.rtag && !r0.err && !r1.err;
      if (got != exp) cms_error("test", $sformatf("%s: tag %0d, expected %0d", ctx, got, exp));
      // The same encoding on the load: a tag only for MuBi4True (the scoreboard's check)
      tsm_pair(1'b0, g, 0, 0, enc, r0, r1);
      tsm_cov_op = 1; tsm_cg.sample();
    end
  end
  wait_quiet();
  expect_no_alert("valid request tag encodings");
  check_tags("valid request tag encodings");

`ifdef CMS_TAG_MUBI
  // Invalid request encodings: c 0 CSC over tag 0, 1 CSC over tag 1, 2 CLC of tag 1
  for (int unsigned v = 0; v < 16; v++) begin
    if (!tsm_invalid(v)) continue;
    enc = prim_mubi_pkg::mubi4_t'(v);
    for (int unsigned c = 0; c < 3; c++) begin
      g = MainSramBase + 32'h400 + 8 * c;
      tsm_setup(g, c != 0, w0, w1);
      expect_no_alert("before the invalid request encoding");
      sb.core_err_any = 2;
      tsm_pair(c != 2, g, w0, w1, enc, r0, r1);
      tsm_cov_enc = v; tsm_cov_op = (c == 2) ? 1 : 0; tsm_cg.sample();
      wait_quiet();
      sb.core_err_any = 0;
      if (c == 1) sb.set_tag(granule(g), TagAny);
      if (c == 2 && (r0.rtag || r1.rtag))
        cms_error("test", $sformatf("CLC with request encoding 0x%0h returned a tag", v));
      expect_alert($sformatf("request tag encoding 0x%0h, case %0d", v, c));
      check_tags($sformatf("request tag encoding 0x%0h, case %0d", v, c));
      do_reset();
    end
  end
`ifndef VERILATOR
  // Corrupted response encodings, over tag 0 and tag 1
  for (int unsigned pre = 0; pre < 2; pre++) begin
    for (int unsigned v = 0; v < 16; v++) begin
      if (!tsm_invalid(v)) continue;
      g = MainSramBase + 32'h500 + 8 * pre;
      tsm_setup(g, 1'(pre), w0, w1);
      expect_no_alert("before the corrupted response encoding");
      // What the core makes of a corrupted response is its own business; the model leaves the
      // returned tag open and keeps the stored one.
      sb.set_tag(granule(g), TagAny);
      tsm_rsp_bad       = prim_mubi_pkg::mubi4_t'(v);
      tsm_rsp_corrupt_n = 2;
      tsm_pair(1'b0, g, 0, 0, prim_mubi_pkg::MuBi4True, r0, r1);
      tsm_cov_enc = v; tsm_cov_op = 2 + pre; tsm_cg.sample();
      wait_quiet();
      if (tsm_rsp_corrupt_n != 0)
        cms_error("test", $sformatf("response encoding 0x%0h: %0d response(s) not corrupted", v,
                                    tsm_rsp_corrupt_n));
      tsm_rsp_corrupt_n = 0;
      sb.set_tag(granule(g), tag_exact(1'(pre)));
      expect_alert($sformatf("corrupted response tag encoding 0x%0h over tag %0d", v, pre));
      check_tags($sformatf("corrupted response tag encoding 0x%0h", v));
      do_reset();
    end
  end
`endif
`else
  // After the checks above, so that the must-fail run's corruption comes first.
  cms_error("test", {"REQ_CMS_01: the subsystem's core tag sideband is one bit each way ",
                     "(cheriot_mem_sys cored_tag_h2d_i/cored_tag_d2h_o, TODO(#31066)), not MuBi4: ",
                     "the 14 invalid request encodings and the corrupted response encodings cannot ",
                     "be presented. Build with +define+CMS_TAG_MUBI once it is MuBi4."});
`endif
endtask

// ---------------------------------------------------------------------------------------------
// REQ_CMS_02: cms_lockstep_glitch
// ---------------------------------------------------------------------------------------------
//
// RFC "Lockstepped CHERIoT Subsystem" and "End-To-End ECC and Lockstep Operation": the subsystem
// operates in its own lockstep domain "to ensure FI attacks cannot induce malicious accesses";
// cheriot_mem_sys.hjson LOGIC.SHADOW: "including the background revocation engine". The row: a
// divergence raises fatal_fault before any meta SRAM write by the faulty copy completes.
//
// Stimulus: one-cycle upsets of one flop: its value inverted in one bit from just after a rising
// edge until just before the next, so the logic sees it at one edge and the flop then updates
// normally. Flops by hierarchy in four groups: the subsystem's own control (RMW filter, tag
// filter, WTRC, the core-write watch) and its TBRE (mover, TRVK filter, sweep control), each in
// the copy that drives the outputs and in the second copy. Each group is hit once idle and once
// under traffic (capability stores and loads, data stores, sweeps of the same range); which flop
// of the group, which bit, and when, are drawn per seed. +cms_ls_rounds=<n> repeats the set.
// Checks, per upset (an upset of one copy is a divergence of the two):
//   fatal_fault (the subsystem's alert_tx) within LsAlertWait cycles;
//   for an upset of the copy that drives the outputs, no meta SRAM write presented after the
//   upset completes (its D handshake) before fatal_fault is raised;
//   no fatal_fault before the upset.
// From the upset to the reset that follows, the subsystem's other behaviour is not judged
// (scoreboard errors are logged as CMS_MASKED, assertions killed); after the reset the bench
// starts again from cleared memories and model (as the system re-initialises the meta SRAM after
// the fatal alert's reset, REQ_CMS_15). Before and after, the scoreboard checks everything.
// The pinned RTL has one copy (TODO(#30572)): the second copy's groups need
// +define+CMS_LOCKSTEP with CMS_LS_SHADOW naming that copy (and CMS_LS_MAIN the first, default
// u_dut); without them the test fails with the gap. Xcelium only (force).

`ifndef CMS_LS_MAIN
`define CMS_LS_MAIN u_dut
`endif

localparam int unsigned LsAlertWait = 256;
localparam int unsigned LsMainCtl    = 0;
localparam int unsigned LsMainTbre   = 1;
localparam int unsigned LsShadowCtl  = 2;
localparam int unsigned LsShadowTbre = 3;
localparam int unsigned LsNumCtl  = 7;
localparam int unsigned LsNumTbre = 12;

// The flops, relative to a copy's root
string ls_ctl_names[LsNumCtl] = '{
  "u_cheriot_rmw_filter.is_read_q",
  "u_cheriot_rmw_filter.tag_q",
  "u_cheriot_rmw_filter.bit_sel_q",
  "u_cheriot_rmw_filter.meta_addr_q",
  "u_cheriot_tag_filter.tag_d_q",
  "u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc.w0_ok_q",
  "core_wr_valid_q"
};
string ls_tbre_names[LsNumTbre] = '{
  "u_cheriot_tbre.u_cheriot_tbre_mover.current_q",
  "u_cheriot_tbre.u_cheriot_tbre_mover.inflight_q",
  "u_cheriot_tbre.u_cheriot_tbre_mover.write_rtag_q",
  "u_cheriot_tbre.u_cheriot_tbre_mover.lower_hit_q",
  "u_cheriot_tbre.u_cheriot_tbre_mover.track_stale_q",
  "u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.ptr_storage_valid_q",
  "u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.revbm_outstanding_q",
  "tbre_valid_q",
  "tbre_active_q",
  "tbre_epoch_q",
  "tbre_failed_q",
  "tbre_clear_rmw_q"
};

int unsigned ls_cov_grp, ls_cov_traffic;
covergroup cms_lockstep_cg;
  option.per_instance = 1;
  cp_grp:     coverpoint ls_cov_grp { bins main_ctl = {0}; bins main_tbre = {1}; bins shadow_ctl = {2};
                                      bins shadow_tbre = {3}; }
  cp_traffic: coverpoint ls_cov_traffic { bins idle = {0}; bins traffic = {1}; }
  x_grp_traffic: cross cp_grp, cp_traffic;
endgroup
cms_lockstep_cg ls_cg = new();

// The upset monitor: fatal_fault, and the meta SRAM writes the faulty copy completes before it
bit              ls_armed = 1'b0;
bit              ls_watch_writes, ls_alert_seen, ls_skip_next;
longint unsigned ls_up_cycle, ls_alert_cycle;
bit              ls_wq[$];        // per accepted meta SRAM request: a write presented after the upset
int unsigned     ls_early_writes;
bit              ls_stop;
logic [63:0]     ls_old, ls_new;

always @(posedge clk) begin
  if (rst_n && ls_armed) begin
    // A write completing in the cycle fatal_fault rises did not wait for it
    if (meta_d2h.d_valid && meta_h2d.d_ready && ls_wq.size() > 0) begin
      if (ls_wq.pop_front() && ls_watch_writes && !ls_alert_seen) begin
        ls_early_writes++;
        if (ls_early_writes == 1)
          cms_info("lockstep", $sformatf("meta SRAM write completed %0d cycle(s) after the upset, before fatal_fault",
                                         cms_cycle - ls_up_cycle));
      end
    end
    if (!ls_alert_seen && alert_tx[0].alert_p) begin
      ls_alert_seen  = 1'b1;
      ls_alert_cycle = cms_cycle;
    end
    if (meta_h2d.a_valid && meta_d2h.a_ready) begin
      ls_wq.push_back(!ls_skip_next && meta_h2d.a_opcode != Get);
      ls_skip_next = 1'b0;
    end
  end
end

function automatic int unsigned ls_group_size(int unsigned grp);
  return (grp == LsMainCtl || grp == LsShadowCtl) ? LsNumCtl : LsNumTbre;
endfunction

function automatic string ls_flop_name(int unsigned grp, int unsigned idx);
  string root;
  root = (grp >= LsShadowCtl) ? "shadow copy" : "main copy";
  return $sformatf("%s: %s", root, (grp == LsMainCtl || grp == LsShadowCtl) ? ls_ctl_names[idx]
                                                                            : ls_tbre_names[idx]);
endfunction

// Invert one bit of flop P from now until just before the next rising edge (called just after
// one): the logic sees the upset value at one edge; the flop then loads its next value. A
// released variable keeps the forced value until it is next assigned.
`define CMS_LS_UPSET(P) begin \
  ls_old = 64'(P); \
  ls_new = ls_old ^ (64'd1 << ($urandom % $bits(P))); \
  force P = ls_new; \
  @(negedge clk); \
  #2; \
  release P; \
end

task automatic ls_upset(int unsigned grp, int unsigned idx);
`ifndef VERILATOR
  case (grp)
    LsMainCtl: case (idx)
      0: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_rmw_filter.is_read_q)
      1: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_rmw_filter.tag_q)
      2: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_rmw_filter.bit_sel_q)
      3: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_rmw_filter.meta_addr_q)
      4: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tag_filter.tag_d_q)
      5: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc.w0_ok_q)
      default: `CMS_LS_UPSET(`CMS_LS_MAIN.core_wr_valid_q)
    endcase
    LsMainTbre: case (idx)
      0: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_tbre_mover.current_q)
      1: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_tbre_mover.inflight_q)
      2: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_tbre_mover.write_rtag_q)
      3: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_tbre_mover.lower_hit_q)
      4: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_tbre_mover.track_stale_q)
      5: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.ptr_storage_valid_q)
      6: `CMS_LS_UPSET(`CMS_LS_MAIN.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.revbm_outstanding_q)
      7: `CMS_LS_UPSET(`CMS_LS_MAIN.tbre_valid_q)
      8: `CMS_LS_UPSET(`CMS_LS_MAIN.tbre_active_q)
      9: `CMS_LS_UPSET(`CMS_LS_MAIN.tbre_epoch_q)
      10: `CMS_LS_UPSET(`CMS_LS_MAIN.tbre_failed_q)
      default: `CMS_LS_UPSET(`CMS_LS_MAIN.tbre_clear_rmw_q)
    endcase
`ifdef CMS_LOCKSTEP
    LsShadowCtl: case (idx)
      0: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_rmw_filter.is_read_q)
      1: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_rmw_filter.tag_q)
      2: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_rmw_filter.bit_sel_q)
      3: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_rmw_filter.meta_addr_q)
      4: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tag_filter.tag_d_q)
      5: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc.w0_ok_q)
      default: `CMS_LS_UPSET(`CMS_LS_SHADOW.core_wr_valid_q)
    endcase
    LsShadowTbre: case (idx)
      0: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_tbre_mover.current_q)
      1: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_tbre_mover.inflight_q)
      2: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_tbre_mover.write_rtag_q)
      3: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_tbre_mover.lower_hit_q)
      4: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_tbre_mover.track_stale_q)
      5: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.ptr_storage_valid_q)
      6: `CMS_LS_UPSET(`CMS_LS_SHADOW.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.revbm_outstanding_q)
      7: `CMS_LS_UPSET(`CMS_LS_SHADOW.tbre_valid_q)
      8: `CMS_LS_UPSET(`CMS_LS_SHADOW.tbre_active_q)
      9: `CMS_LS_UPSET(`CMS_LS_SHADOW.tbre_epoch_q)
      10: `CMS_LS_UPSET(`CMS_LS_SHADOW.tbre_failed_q)
      default: `CMS_LS_UPSET(`CMS_LS_SHADOW.tbre_clear_rmw_q)
    endcase
`endif
    default: cms_error("test", $sformatf("lockstep: no flop group %0d in this build", grp));
  endcase
`else
  cms_error("test", "cms_lockstep_glitch needs Xcelium (force)");
`endif
endtask

`undef CMS_LS_UPSET

// Traffic over [base, base + 8n): capability stores and loads, data stores and loads, and sweeps
// of the same range, until ls_stop.
task automatic ls_traffic(logic [31:0] base, int unsigned n);
  fork
    begin
      logic [31:0] a, w0, w1;
      while (!ls_stop) begin
        a = base + 8 * $urandom_range(n - 1);
        case ($urandom_range(4))
          0, 1: begin
            make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
            core_store_cap(a, w0, w1, rbit());
          end
          2: begin
            core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1));
            core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1));
          end
          3:       core_issue(mk(PutFullData, a + 4 * rbit(), 2'd2, $urandom));
          default: core_issue(mk(Get, a + 4 * rbit(), 2'd2));
        endcase
        while (!ls_stop && u_core.in_flight() > 4) @(posedge clk);
        @(posedge clk);
      end
    end
    begin
      while (!ls_stop) begin
        tbre_sweep(base, n, 1'b1);
        wait_cycles($urandom_range(40));
      end
    end
  join
endtask

// After the reset that follows an upset: the bench's memories and the model start again from
// zero, as the system's meta SRAM does after the fatal alert's reset (REQ_CMS_15). Nothing is
// taken from the DUT.
task automatic ls_reinit();
  meta_store.mem.delete();
  data_store.mem.delete();
  sb.tagset.delete();
  sb.sdata.delete();
  sb.srevbm.delete();
  sb.recent_stores.delete();
  sb.sticky_known = 1'b0;
  if (cm != null) cms_cmodel_pkg::cms_reset(cm);
  u_core.rsp_by_id.delete();
  u_revbm.rsp_by_id.delete();
  u_corerevbm.rsp_by_id.delete();
  u_csr.rsp_by_id.delete();
endtask

task automatic ls_one(int unsigned grp, int unsigned idx, bit traffic);
  logic [31:0] base;
  string       ctx;
  bit          exp_alert;
  int unsigned masked0;
  base = MainSramBase + 32'h800;
  fill_caps(base, 32);
  expect_no_alert("before the upset");
  ls_stop = 1'b0;
  if (traffic) begin
    fork
      ls_traffic(base, 32);
    join_none
    wait_cycles($urandom_range(400, 30));
  end else begin
    wait_quiet();
    wait_cycles($urandom_range(20));
  end
  ctx = $sformatf("upset of %s (%s)", ls_flop_name(grp, idx), traffic ? "under traffic" : "idle");
  ls_cov_grp = grp; ls_cov_traffic = 32'(traffic); ls_cg.sample();

  // From here until the reset, only this test's verdicts count
  @(posedge clk);
  #1;
`ifndef VERILATOR
  $assertkill;
`endif
  masked0        = cms_err_masked_count;
  cms_err_masked = 1'b1;
  ls_wq.delete();
  repeat (u_meta.rsp_q.size()) ls_wq.push_back(1'b0);  // accepted before the upset
  ls_skip_next    = meta_h2d.a_valid;                   // presented before the upset
  ls_alert_seen   = alert_tx[0].alert_p;
  ls_alert_cycle  = cms_cycle;
  ls_early_writes = 0;
  ls_watch_writes = (grp == LsMainCtl || grp == LsMainTbre);
  ls_up_cycle     = cms_cycle;
  ls_armed        = 1'b1;
  ls_upset(grp, idx);
  for (int unsigned i = 0; i < LsAlertWait && !ls_alert_seen; i++) @(posedge clk);
  ls_armed = 1'b0;

  cms_err_masked = 1'b0;
  cms_info("test", $sformatf("%s: bit 0x%0h, fatal_fault %s", ctx, ls_old ^ ls_new,
                             ls_alert_seen ? $sformatf("%0d cycle(s) after it", ls_alert_cycle - ls_up_cycle)
                                           : "not raised"));
  exp_alert = 1'b1;
  if (sb.corrupt("lockstep", ctx)) exp_alert = 1'b0;
  if (ls_alert_seen != exp_alert)
    cms_error("test", $sformatf("%s: fatal_fault %s within %0d cycles, expected %s", ctx,
                                ls_alert_seen ? "raised" : "not raised", LsAlertWait,
                                exp_alert ? "raised" : "not raised"));
  if (ls_early_writes != 0)
    cms_error("test", $sformatf("%s: %0d meta SRAM write(s) by the faulty copy completed before fatal_fault",
                                ctx, ls_early_writes));
  cms_err_masked = 1'b1;

  ls_stop = 1'b1;
  wait_cycles(3);
  disable fork;
  do_reset();
  ls_reinit();
  cms_err_masked = 1'b0;
`ifndef VERILATOR
  $asserton;
`endif
  if (cms_err_masked_count != masked0)
    cms_info("test", $sformatf("%s: %0d scoreboard/bench message(s) masked until the reset", ctx,
                               cms_err_masked_count - masked0));
endtask

task automatic t_lockstep_glitch();
  int unsigned rounds, idx;
  int unsigned grps[$];
  rounds = 1;
  void'($value$plusargs("cms_ls_rounds=%d", rounds));
  cheriot_on();
  // The fastest meta SRAM leaves the subsystem the least time to stop a write
  u_meta.a_ready_pct = 100;
  u_meta.lat_min     = 1;
  u_meta.lat_max     = 1;
  grps = '{LsMainCtl, LsMainTbre};
`ifdef CMS_LOCKSTEP
  grps.push_back(LsShadowCtl);
  grps.push_back(LsShadowTbre);
`endif
  for (int unsigned r = 0; r < rounds; r++) begin
    foreach (grps[i]) begin
      for (int unsigned traffic = 0; traffic < 2; traffic++) begin
        idx = $urandom_range(ls_group_size(grps[i]) - 1);
        ls_one(grps[i], idx, 1'(traffic));
      end
    end
  end
  // Working again after the last reset, checked by the scoreboard
  fill_caps(MainSramBase, 64);
  load_all_caps(MainSramBase, 64);
  tbre_sweep(MainSramBase, 64);
  load_all_caps(MainSramBase, 64);
  check_tags("after the last upset");
`ifndef CMS_LOCKSTEP
  cms_error("test", {"REQ_CMS_02: the subsystem has one copy (cheriot_mem_sys LOGIC.SHADOW, ",
                     "TODO(#30572)): no second copy to upset and nothing to compare against",
                     ". Build with +define+CMS_LOCKSTEP and CMS_LS_SHADOW once it has one."});
`endif
endtask

// ---------------------------------------------------------------------------------------------
// REQ_CMS_07: cms_latency
// ---------------------------------------------------------------------------------------------
//
// The row: in CHERIoT mode the subsystem adds no cycle to data and capability reads, nor to
// writes that leave the tag unchanged, and at most 4 cycles per word to a write that changes it;
// in ePMP mode it adds none (RFC "Memory Access Latency", Table 2).
//
// Stimulus: every port at full speed (no request gaps, d_ready and a_ready always, the memories'
// shortest latency: a response the cycle after the request, as an SRAM gives it). One operation at
// a time to a random main SRAM granule whose tag was set to 0 or 1 first: a word data read, a
// byte/half read, a capability read (two hinted words), a word data write, a byte/half or
// PutPartialData write, a capability write (two words) with tag 0 or 1; each in CHERIoT mode,
// ePMP mode (MuBi4False) and with an invalid cheriot_ena encoding (not CHERIoT mode,
// REQ_CMS_06), twice.
// Check, per word: the cycles the subsystem adds = (core request first presented -> core response
// handshake) - (crossbar request first presented -> crossbar response first valid), on cored_tl_d
// and cored_tl_h; every core request reaches the crossbar once. Allowed: CHERIoT mode, a write
// that changes the granule's tag 0..4, everything else 0; any other mode 0. The total per
// capability is logged against Table 2 (stated per 64-bit capability).

localparam int unsigned LatDataRd    = 0;
localparam int unsigned LatDataRdSub = 1;
localparam int unsigned LatCapRd     = 2;
localparam int unsigned LatDataWr    = 3;
localparam int unsigned LatDataWrSub = 4;
localparam int unsigned LatCapWr     = 5;
localparam int unsigned LatNumCls    = 6;
localparam int unsigned LatMaxChange = 4;  // REQ_CMS_07: cycles per word a tag-changing write may add
// The bound lat_one applies; cms_trvk_backpressure sets REQ_CMS_07's 2 per word around its calls.
int unsigned lat_change_max = LatMaxChange;
string lat_cls_names[LatNumCls] = '{"data read", "sub-word data read", "capability read",
                                    "data write", "sub-word data write", "capability write"};

int unsigned lat_cov_cls, lat_cov_chg, lat_cov_mode;  // mode: 0 CHERIoT, 1 MuBi4False, 2 invalid
covergroup cms_latency_cg;
  option.per_instance = 1;
  cp_cls:  coverpoint lat_cov_cls { bins cls[] = {[0:LatNumCls-1]}; }
  cp_chg:  coverpoint lat_cov_chg { bins same = {0}; bins change = {1}; }
  cp_mode: coverpoint lat_cov_mode { bins cheriot = {0}; bins epmp = {1}; bins invalid = {2}; }
  x_cls_chg_mode: cross cp_cls, cp_chg, cp_mode {
    ignore_bins read_change = binsof(cp_cls) intersect {LatDataRd, LatDataRdSub, LatCapRd} &&
                              binsof(cp_chg) intersect {1};
  }
endgroup
cms_latency_cg lat_cg = new();

// The latency monitor: raw cycles of each request on both sides, in order
bit              lat_on = 1'b0;
bit              lat_core_pres, lat_mem_pres, lat_mem_dv_seen;
longint unsigned lat_core_pc, lat_mem_pc, lat_mem_dv;
longint unsigned lat_core_pq[$], lat_mem_pq[$];   // accepted, waiting for the response
longint unsigned lat_cp[$], lat_cd[$];            // core: presented, response handshake
longint unsigned lat_mp[$], lat_md[$];            // crossbar: presented, response first valid

always @(posedge clk) begin
  if (!rst_n || !lat_on) begin
    lat_core_pres   = 1'b0;
    lat_mem_pres    = 1'b0;
    lat_mem_dv_seen = 1'b0;
  end else begin
    if (core_d2h.d_valid && core_h2d.d_ready && lat_core_pq.size() > 0) begin
      lat_cp.push_back(lat_core_pq.pop_front());
      lat_cd.push_back(cms_cycle);
    end
    if (core_h2d.a_valid && !lat_core_pres) begin
      lat_core_pres = 1'b1;
      lat_core_pc   = cms_cycle;
    end
    if (core_h2d.a_valid && core_d2h.a_ready) begin
      lat_core_pq.push_back(lat_core_pc);
      lat_core_pres = 1'b0;
    end
    if (dmem_d2h.d_valid && !lat_mem_dv_seen && lat_mem_pq.size() > 0) begin
      lat_mem_dv_seen = 1'b1;
      lat_mem_dv      = cms_cycle;
    end
    if (dmem_d2h.d_valid && dmem_h2d.d_ready && lat_mem_pq.size() > 0) begin
      lat_mp.push_back(lat_mem_pq.pop_front());
      lat_md.push_back(lat_mem_dv);
      lat_mem_dv_seen = 1'b0;
    end
    if (dmem_h2d.a_valid && !lat_mem_pres) begin
      lat_mem_pres = 1'b1;
      lat_mem_pc   = cms_cycle;
    end
    if (dmem_h2d.a_valid && dmem_d2h.a_ready) begin
      lat_mem_pq.push_back(lat_mem_pc);
      lat_mem_pres = 1'b0;
    end
  end
end

// One measured operation of class cls at granule g (tag `pre` before it), mode index m; for a
// capability write, `t` is the tag it stores.
task automatic lat_one(int unsigned cls, logic [31:0] g, logic pre, logic t, int unsigned m,
                       prim_mubi_pkg::mubi4_t mode);
  cms_txn_t    tx[$];
  logic [31:0] a;
  logic [1:0]  sz;
  logic        cur, wr_tag;
  bit          chg [2];
  bit          wr, any_chg;
  int          ovh [2];
  int          lo, hi, cap_ovh;
  string       ctx;

  // The accesses
  a  = g + 4 * rbit();
  sz = 2'($urandom_range(1));
  case (cls)
    LatDataRd:    tx.push_back(mk(Get, a, 2'd2));
    LatDataRdSub: tx.push_back(mk(Get, a + $urandom_range(3), sz));
    LatCapRd: begin
      tx.push_back(mk(Get, g, 2'd2, 32'h0, 1'b1));
      tx.push_back(mk(Get, g + 4, 2'd2, 32'h0, 1'b1));
    end
    LatDataWr:    tx.push_back(mk(PutFullData, a, 2'd2, $urandom));
    LatDataWrSub: begin
      if (rbit()) tx.push_back(mk(PutFullData, a + $urandom_range(3), sz, $urandom));
      else        tx.push_back(mk(PutPartialData, a, 2'd2, $urandom, 1'b0, 4'($urandom_range(14, 1))));
    end
    default: begin
      tx.push_back(mk(PutFullData, g, 2'd2, $urandom, t));
      tx.push_back(mk(PutFullData, g + 4, 2'd2, $urandom, t));
    end
  endcase
  wr = (cls >= LatDataWr);
  if (cls == LatCapRd || cls == LatCapWr) a = g;

  // Which words change the granule's tag: a write stores its own tag (0 for anything but a
  // capability store), the second word of a capability store finds the first one's.
  cur     = pre;
  any_chg = 0;
  foreach (tx[i]) begin
    wr_tag = (cls == LatCapWr) ? t : 1'b0;
    chg[i] = wr && (cur != wr_tag);
    if (wr) cur = wr_tag;
    any_chg |= chg[i];
  end

  // Measure
  lat_cp.delete(); lat_cd.delete(); lat_mp.delete(); lat_md.delete();
  lat_core_pq.delete(); lat_mem_pq.delete();
  @(posedge clk);
  lat_on = 1'b1;
  foreach (tx[i]) core_issue(tx[i]);
  wait_quiet();
  lat_on = 1'b0;

  lat_cov_cls = cls; lat_cov_chg = 32'(any_chg); lat_cov_mode = m; lat_cg.sample();
  ctx = $sformatf("%s at 0x%08x, tag %0d before%s, cheriot_ena 0x%0h", lat_cls_names[cls], a, pre,
                  (cls == LatCapWr) ? $sformatf(", storing tag %0d", t) : "", mode);
  if (lat_cd.size() != tx.size() || lat_md.size() != tx.size()) begin
    cms_error("test", $sformatf("%s: %0d core request(s) answered, %0d reached the crossbar and were answered, expected %0d",
                                ctx, lat_cd.size(), lat_md.size(), tx.size()));
    return;
  end
  foreach (tx[i]) begin
    ovh[i] = int'(lat_cd[i] - lat_cp[i]) - int'(lat_md[i] - lat_mp[i]);
    lo = 0;
    hi = (mode == prim_mubi_pkg::MuBi4True && chg[i]) ? int'(lat_change_max) : 0;
    if (sb.corrupt("latency", $sformatf("%s, word %0d", ctx, i))) begin
      lo = hi + 1;
      hi = hi + 1;
    end
    if (ovh[i] < lo || ovh[i] > hi)
      cms_error("test", $sformatf("%s, word %0d%s: the subsystem added %0d cycle(s), REQ_CMS_07 allows %0d..%0d",
                                  ctx, i, chg[i] ? " (changes the tag)" : "", ovh[i], lo, hi));
  end
  if (tx.size() == 2) begin
    cap_ovh = int'(lat_cd[1] - lat_cp[0]) - int'(lat_md[1] - lat_mp[0]);
    cms_info("latency", $sformatf("%s: %0d + %0d cycle(s) per word, %0d per capability (RFC Table 2: %s)",
                                  ctx, ovh[0], ovh[1], cap_ovh,
                                  mode != prim_mubi_pkg::MuBi4True ? "0" :
                                  (wr && any_chg) ? "4" : "0"));
  end else begin
    cms_info("latency", $sformatf("%s: %0d cycle(s)", ctx, ovh[0]));
  end
endtask

// ---------------------------------------------------------------------------------------------
// REQ_CMS_02 (and REQ_CIB_03's placement-neutral rule): cms_trvk_fault
// ---------------------------------------------------------------------------------------------
//
// The TBRE's own copy of the load filter (cheriot_trvk_core, in cheriot_trvk_tlul) decides which
// swept capabilities are revoked. A single fault in it lets a revoked capability through tagged,
// and the engine then leaves its tag set: a use-after-free that survives the sweep. REQ_CMS_02
// puts the TBRE in the subsystem's lockstep domain, so such a fault must raise fatal_fault. The
// core's copy has the same test (core_ibex_trvk_fault_test, ibex dv/uvm/core_ibex) with the same
// sites; ibex_cheriot_verification status/rtl_tickets.md "ibex_trvk sits outside the lockstep
// pair" says why both are expected to fail today.
//
// Stimulus, per injection: the bitmap written through the software window (random words for
// verdict, tag_out and bit_select; every bit set for the "no lookup" sites, so the skipped
// lookup would have revoked); 64 tagged memory capabilities with heap bases stored and swept.
// While the sweep runs, the bench judges, on each falling edge, whether a one-cycle fault at the
// site would let a revoked capability through in the cycle about to be sampled, and forces the
// site for that one cycle at the (skip+1)-th such opportunity:
//   +cms_tf_site=verdict       revbm_revoked forced 0 on a revoked bitmap response
//   +cms_tf_site=tag_out       upstream_tag_o forced 1 on a revoked response
//   +cms_tf_site=bit_select    one bit of revbm_bit_select flipped to a clear bit of the word
//   +cms_tf_site=req_required  revbm_req_required forced 0 in a lookup's first cycle
//   +cms_tf_site=out_of_range  revbm_out_of_range forced 1 in a lookup's first cycle
//   +cms_tf_site=sealing       is_sealing_cap forced 1 in a lookup's first cycle
//   +cms_tf_site=align         misalign_flag_out forced 0 in a lookup's first cycle
//   +cms_tf_skip=<n>           opportunities let pass first (default: random 0-15, per injection)
//   +cms_tf_injections=<n>     injections per run, each followed by a reset (default 3)
//   +cms_tf_copy=shadow        inject into the second copy (needs +define+CMS_LOCKSTEP)
// Checks, per injection:
//   effective     in the forced cycle the filter hands the engine a tagged response for a tagged
//                 capability (sampled after the force, before the edge); counted. A run with no
//                 effective injection fails ("no effective injection").
//   detected      fatal_fault within LsAlertWait cycles (REQ_CMS_02's lockstep, or a countermeasure
//                 of the filter's own; either satisfies REQ_CIB_03's rule).
// From the injection to the reset after it the model is masked (CMS_MASKED) and assertions are
// killed, as in cms_lockstep_glitch. +cms_sb_corrupt=trvk,1 inverts the first injection's
// effectiveness verdict, so the run must fail on it before any other error.
// The pinned RTL has no second copy and no check of the filter's logic (upstream TODO(#30572)):
// every site is expected to fail with "undetected". Xcelium only (force).

localparam int unsigned TfCaps = 64;

typedef enum int unsigned {
  TfVerdict, TfTagOut, TfBitSelect, TfReqRequired, TfOutOfRange, TfSealing, TfAlign
} cms_tf_site_e;

// The filter's signals the bench judges a fault by, per copy (0 main, 1 shadow). Read through
// hierarchical references, so not under Verilator (lint only; the flow is Xcelium's).
wire        tf_hs        [2];  // upstream (engine) response handshake
wire        tf_tag_o     [2];  // the tag handed to the engine
wire        tf_ds_tag    [2];  // the swept word is tagged
wire        tf_rv        [2];  // bitmap response valid
wire        tf_revoked   [2];  // the filter's verdict
wire        tf_rsp_err   [2];  // the verdict comes from an error, not from the bitmap bit
wire [31:0] tf_rdata     [2];
wire [4:0]  tf_bs        [2];
wire        tf_req       [2];  // bitmap request presented
wire        tf_outst     [2];
wire        tf_rsp_ready [2];  // the engine takes a response
`define CMS_TF_NONE(I) \
  assign tf_hs[I] = 1'b0; assign tf_tag_o[I] = 1'b0; assign tf_ds_tag[I] = 1'b0; \
  assign tf_rv[I] = 1'b0; assign tf_revoked[I] = 1'b0; assign tf_rsp_err[I] = 1'b0; \
  assign tf_rdata[I] = '0; assign tf_bs[I] = '0; assign tf_req[I] = 1'b0; \
  assign tf_outst[I] = 1'b0; assign tf_rsp_ready[I] = 1'b0;
`ifndef VERILATOR
`define CMS_TF_CORE(ROOT) ROOT.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core
`define CMS_TF_OBS(I, ROOT) \
  assign tf_hs[I]      = `CMS_TF_CORE(ROOT).upstream_rsp_valid_o && `CMS_TF_CORE(ROOT).upstream_rsp_ready_i; \
  assign tf_tag_o[I]   = `CMS_TF_CORE(ROOT).upstream_tag_o; \
  assign tf_ds_tag[I]  = `CMS_TF_CORE(ROOT).downstream_tag_i; \
  assign tf_rv[I]      = `CMS_TF_CORE(ROOT).revbm_rsp_valid_i; \
  assign tf_revoked[I] = `CMS_TF_CORE(ROOT).revbm_revoked; \
  assign tf_rsp_err[I] = `CMS_TF_CORE(ROOT).revbm_rsp_err_i || (|`CMS_TF_CORE(ROOT).revbm_rsp_data_intg_error); \
  assign tf_rdata[I]   = `CMS_TF_CORE(ROOT).revbm_rsp_data_i; \
  assign tf_bs[I]      = `CMS_TF_CORE(ROOT).revbm_bit_select; \
  assign tf_req[I]     = `CMS_TF_CORE(ROOT).revbm_req_valid_o; \
  assign tf_outst[I]   = `CMS_TF_CORE(ROOT).revbm_outstanding_q; \
  assign tf_rsp_ready[I] = `CMS_TF_CORE(ROOT).upstream_rsp_ready_i;
`CMS_TF_OBS(0, `CMS_LS_MAIN)
`ifdef CMS_LOCKSTEP
`CMS_TF_OBS(1, `CMS_LS_SHADOW)
`else
`CMS_TF_NONE(1)
`endif
`undef CMS_TF_OBS
`else
`CMS_TF_NONE(0)
`CMS_TF_NONE(1)
`endif
`undef CMS_TF_NONE

int unsigned tf_cov_site, tf_cov_detected;
covergroup cms_trvk_fault_cg;
  option.per_instance = 1;
  cp_site:     coverpoint tf_cov_site { bins site[] = {[TfVerdict:TfAlign]}; }
  cp_detected: coverpoint tf_cov_detected { bins undetected = {0}; bins detected = {1}; }
  x_site_detected: cross cp_site, cp_detected;
endgroup
cms_trvk_fault_cg tf_cg = new();

function automatic string tf_signal(cms_tf_site_e s);
  case (s)
    TfVerdict:     return "revbm_revoked";
    TfTagOut:      return "upstream_tag_o";
    TfBitSelect:   return "revbm_bit_select";
    TfReqRequired: return "revbm_req_required";
    TfOutOfRange:  return "revbm_out_of_range";
    TfSealing:     return "is_sealing_cap";
    default:       return "misalign_flag_out";
  endcase
endfunction

// In the cycle about to be sampled: would a fault at `s` in copy `c` let a revoked capability
// through, and with which value? `prev_req`: the bitmap request was presented in the cycle before.
function automatic bit tf_effective(cms_tf_site_e s, int unsigned c, bit prev_req,
                                    output logic [31:0] value);
`ifndef VERILATOR
  bit revoked_rsp, first_req;
  revoked_rsp = tf_hs[c] && tf_rv[c] && tf_revoked[c] && tf_ds_tag[c];
  // A lookup's first cycle, with the engine ready for the response it would then get untested
  first_req   = tf_req[c] && !tf_outst[c] && !prev_req && tf_rsp_ready[c];
  value = '0;
  case (s)
    TfVerdict:  begin value = 0; return revoked_rsp; end
    TfTagOut:   begin value = 1; return revoked_rsp; end
    TfBitSelect: begin
      if (!revoked_rsp || tf_rsp_err[c]) return 0;
      for (int k = 0; k < 5; k++) begin
        if (!tf_rdata[c][tf_bs[c] ^ 5'(1 << k)]) begin value = 32'(tf_bs[c] ^ 5'(1 << k)); return 1; end
      end
      return 0;
    end
    TfReqRequired: begin value = 0; return first_req; end
    TfOutOfRange:  begin value = 1; return first_req; end
    TfSealing:     begin value = 1; return first_req; end
    default:       begin value = 0; return first_req; end
  endcase
`else
  value = '0;
  return 0;
`endif
endfunction

// The forced value: a module variable, as a force cannot name an automatic one
logic [31:0] tf_val;

// Force site `s` of copy `c` to `value` from now (a falling edge) to the next falling edge.
// Returns, sampled after the force and before the rising edge, whether the filter then hands the
// engine a tagged response for a tagged capability.
`define CMS_TF_FORCE(ROOT) \
  case (s) \
    TfVerdict:     force `CMS_TF_CORE(ROOT).revbm_revoked      = tf_val[0]; \
    TfTagOut:      force `CMS_TF_CORE(ROOT).upstream_tag_o     = tf_val[0]; \
    TfBitSelect:   force `CMS_TF_CORE(ROOT).revbm_bit_select   = tf_val[4:0]; \
    TfReqRequired: force `CMS_TF_CORE(ROOT).revbm_req_required = tf_val[0]; \
    TfOutOfRange:  force `CMS_TF_CORE(ROOT).revbm_out_of_range = tf_val[0]; \
    TfSealing:     force `CMS_TF_CORE(ROOT).is_sealing_cap     = tf_val[0]; \
    default:       force `CMS_TF_CORE(ROOT).misalign_flag_out  = tf_val[0]; \
  endcase
`define CMS_TF_RELEASE(ROOT) \
  case (s) \
    TfVerdict:     release `CMS_TF_CORE(ROOT).revbm_revoked; \
    TfTagOut:      release `CMS_TF_CORE(ROOT).upstream_tag_o; \
    TfBitSelect:   release `CMS_TF_CORE(ROOT).revbm_bit_select; \
    TfReqRequired: release `CMS_TF_CORE(ROOT).revbm_req_required; \
    TfOutOfRange:  release `CMS_TF_CORE(ROOT).revbm_out_of_range; \
    TfSealing:     release `CMS_TF_CORE(ROOT).is_sealing_cap; \
    default:       release `CMS_TF_CORE(ROOT).misalign_flag_out; \
  endcase

task automatic tf_inject(cms_tf_site_e s, int unsigned c, logic [31:0] value, output bit through);
`ifndef VERILATOR
  tf_val = value;
  if (c == 0) begin
    `CMS_TF_FORCE(`CMS_LS_MAIN)
  end else begin
`ifdef CMS_LOCKSTEP
    `CMS_TF_FORCE(`CMS_LS_SHADOW)
`endif
  end
  #1;
  through = tf_hs[c] && tf_tag_o[c] && tf_ds_tag[c];
  @(negedge clk);
  if (c == 0) begin
    `CMS_TF_RELEASE(`CMS_LS_MAIN)
  end else begin
`ifdef CMS_LOCKSTEP
    `CMS_TF_RELEASE(`CMS_LS_SHADOW)
`endif
  end
`else
  through = 0;
  cms_error("test", "cms_trvk_fault needs Xcelium (force)");
`endif
endtask

`undef CMS_TF_FORCE
`undef CMS_TF_RELEASE

// The bitmap (every bit set, or random words) and TfCaps tagged memory capabilities with heap
// bases at `slot`, through the ports, so the model knows them.
task automatic tf_prepare(logic [31:0] slot, bit all_revoked);
  logic [31:0] w0, w1;
  for (int unsigned w = 0; w < RevbmBytes / 4; w++)
    revbm_write(w, all_revoked ? 32'hffff_ffff : $urandom);
  for (int unsigned i = 0; i < TfCaps; i++) begin
    make_kind_cap(rand_granule(MainSramBase, MainSramTop), 0, 0, w0, w1);
    core_store_cap(slot + 8 * i, w0, w1, 1'b1);
    throttle();
  end
  wait_quiet();
endtask

// One injection: sweeps until the (skip+1)-th effective opportunity, injects, waits for
// fatal_fault, resets. `effective` and `detected` are its outcome.
task automatic tf_one(cms_tf_site_e s, int unsigned c, int unsigned skip, int unsigned n,
                      output bit effective, output bit detected);
  logic [31:0]     slot, value;
  bit              no_lookup, sweep_done, prev_req, injected, through;
  int unsigned     seen, masked0;
  longint unsigned t_inj;
  string           ctx;
  slot      = MainSramBase + SramBytes / 2;
  no_lookup = s inside {TfReqRequired, TfOutOfRange, TfSealing, TfAlign};
  effective = 0;
  detected  = 0;
  injected  = 0;
  seen      = 0;
  masked0   = cms_err_masked_count;
  for (int unsigned round = 0; round < 8 && !injected; round++) begin
    tf_prepare(slot, no_lookup);
    expect_no_alert("before the TRVK fault");
    sweep_done = 0;
    prev_req   = 0;
    fork
      begin
        tbre_sweep(slot, TfCaps, 1'b1);
        sweep_done = 1;
      end
    join_none
    while (!sweep_done) begin
      @(negedge clk);
      if (tf_effective(s, c, prev_req, value)) begin
        if (seen == skip) begin
          injected = 1;
          break;
        end
        seen++;
      end
      prev_req = tf_req[c];
    end
    if (!injected) begin
      wait fork;
      wait_quiet();
    end
  end
  if (!injected) begin
    cms_info("trvk_fault", $sformatf("injection %0d at %s: %0d opportunities in 8 sweeps, fewer than %0d",
                                     n, s.name(), seen, skip + 1));
    return;
  end

  ctx = $sformatf("TRVK fault %0d at %s (%s copy: %s forced to 'h%0h for one cycle, opportunity %0d)",
                  n, s.name(), c != 0 ? "shadow" : "main", tf_signal(s), value, seen);
`ifndef VERILATOR
  $assertkill;
`endif
  t_inj = cms_cycle;
  tf_inject(s, c, value, through);
  // The first injection's effectiveness verdict is the one +cms_sb_corrupt=trvk inverts
  if (n == 0 && sb.corrupt("trvk", ctx)) through = !through;
  if (!through)
    cms_error("test", $sformatf("%s: in the forced cycle the filter did not hand the engine a tagged response",
                                ctx));
  effective = through;
  cms_err_masked = 1'b1;
  for (int unsigned i = 0; i < LsAlertWait && alert_cnt == 0; i++) @(posedge clk);
  detected = (alert_cnt != 0);
  cms_err_masked = 1'b0;
  tf_cov_site = s; tf_cov_detected = 32'(detected); tf_cg.sample();
  if (detected)
    cms_info("trvk_fault", $sformatf("%s: detected, fatal_fault %0d cycle(s) after it", ctx,
                                     cms_cycle - t_inj));
  else if (effective)
    cms_error("test", $sformatf({"%s undetected: a revoked capability reached the engine tagged and ",
                                 "fatal_fault stayed low for %0d cycles (REQ_CMS_02, REQ_CIB_03)"},
                                ctx, LsAlertWait));
  cms_err_masked = 1'b1;
  disable fork;
  do_reset();
  ls_reinit();
  cms_err_masked = 1'b0;
`ifndef VERILATOR
  $asserton;
`endif
  if (cms_err_masked_count != masked0)
    cms_info("trvk_fault", $sformatf("%s: %0d scoreboard/bench message(s) masked until the reset", ctx,
                                     cms_err_masked_count - masked0));
endtask

task automatic t_trvk_fault();
  string        site_name, copy_name;
  cms_tf_site_e s;
  int unsigned  c, injections, skip, n_eff, n_det;
  bit           eff, det, fixed_skip;
  site_name  = "verdict";
  copy_name  = "main";
  injections = 3;
  void'($value$plusargs("cms_tf_site=%s", site_name));
  void'($value$plusargs("cms_tf_copy=%s", copy_name));
  void'($value$plusargs("cms_tf_injections=%d", injections));
  fixed_skip = $value$plusargs("cms_tf_skip=%d", skip);
  case (site_name)
    "verdict":      s = TfVerdict;
    "tag_out":      s = TfTagOut;
    "bit_select":   s = TfBitSelect;
    "req_required": s = TfReqRequired;
    "out_of_range": s = TfOutOfRange;
    "sealing":      s = TfSealing;
    "align":        s = TfAlign;
    default: begin
      cms_error("test", $sformatf("unknown +cms_tf_site=%s", site_name));
      return;
    end
  endcase
  c = (copy_name == "shadow") ? 1 : 0;
`ifndef CMS_LOCKSTEP
  if (c == 1) begin
    cms_error("test", {"cms_trvk_fault +cms_tf_copy=shadow: the subsystem has one copy (TODO(#30572)); ",
                       "build with +define+CMS_LOCKSTEP and CMS_LS_SHADOW"});
    return;
  end
`endif
  cheriot_on();
  n_eff = 0;
  n_det = 0;
  for (int unsigned n = 0; n < injections; n++) begin
    if (!fixed_skip) skip = $urandom_range(15);
    tf_one(s, c, skip, n, eff, det);
    n_eff += eff;
    n_det += (eff && det);
  end
  cms_info("trvk_fault", $sformatf("site %s, %s copy: %0d effective injection(s) of %0d, %0d detected",
                                   site_name, copy_name, n_eff, injections, n_det));
  if (n_eff == 0)
    cms_error("test", $sformatf("site %s: no effective injection (the test shows nothing)", site_name));
endtask

`ifndef VERILATOR
`undef CMS_TF_CORE
`endif

task automatic t_latency();
  prim_mubi_pkg::mubi4_t modes[3];
  logic [31:0]           g, w0, w1;
  int unsigned           v;
  modes[0] = prim_mubi_pkg::MuBi4True;
  modes[1] = prim_mubi_pkg::MuBi4False;
  do v = $urandom_range(15); while (!tsm_invalid(v));
  modes[2] = prim_mubi_pkg::mubi4_t'(v);
  u_core.a_gap_max   = 0;
  u_core.d_ready_pct = 100;
  u_dmem.a_ready_pct = 100;
  u_dmem.lat_min     = 1;
  u_dmem.lat_max     = 1;
  u_meta.a_ready_pct = 100;
  u_meta.lat_min     = 1;
  u_meta.lat_max     = 1;
  for (int unsigned m = 0; m < 3; m++) begin
    for (int unsigned cls = 0; cls < LatNumCls; cls++) begin
      for (int unsigned pre = 0; pre < 2; pre++) begin
        for (int unsigned t = 0; t < ((cls == LatCapWr) ? 2 : 1); t++) begin
          for (int unsigned rep = 0; rep < 2; rep++) begin
            // The granule's tag first, in CHERIoT mode
            g = rand_granule(MainSramBase, MainSramTop);
            cheriot_on();
            make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
            core_store_cap(g, w0, w1, 1'(pre));
            wait_quiet();
            set_ena(modes[m]);
            lat_one(cls, g, 1'(pre), 1'(t), m, modes[m]);
          end
        end
      end
    end
  end
  cheriot_on();
  expect_no_alert("latency");
endtask

// ---------------------------------------------------------------------------------------------
// REQ_CMS_07, REQ_CMS_08: cms_trvk_backpressure
// ---------------------------------------------------------------------------------------------
//
// The EG2 specification's revocation corner case (spec "Revocation", "Corner cases"): the TBRE's
// load filter (cheriot_trvk_core) under back pressure. Its data port is tbre_tl_h (u_tbre_mem),
// its bitmap port the lookups it sends to the meta SRAM through its own access checker
// (tbre_revbm_tl, u_meta).
//
// Stimulus, phase A (+cms_bp_sweeps=<n> sweeps, default 6): per sweep, long random latencies on
// the TBRE's data memory, the meta SRAM and the core's data memory (1..lat_max, lat_max 16-160),
// a_ready withheld at random, and on top of that a staller that holds a_ready of one or all of them
// at 0, and the core's d_ready, for stretches of up to stall_max cycles (32-400); both ports'
// responses arrive in either order relative to each other. A sweep of 32-128 capabilities of
// every class (fill_caps) runs with core traffic beside it: capability loads and stores, bitmap
// writes through the window and bitmap reads on the core's bitmap port.
// Phase B: the bitmap answers at once (meta SRAM latency 1, a_ready always), the data port at
// 1-8 cycles; one sweep; then core accesses one at a time with every memory at full speed.
// Checks:
//   verdict      the scoreboard on every response, and the meta SRAM against the reference after
//                each sweep (check_tags): every capability's tag as the load barrier gives
//                (REQ_CMS_08: the same verdict as ibex_trvk for every capability, bitmap word and
//                bitmap error; the scoreboard is the reference).
//   outstanding  at most 2 reads in flight on tbre_tl_h (theory_of_operation.md "Revocation
//                Engine": "Up to two words are in flight"), and at most 2 bitmap lookups in flight
//                (one per capability whose words are in flight, never more than the words).
//   progress     while the engine is active, some response on its data or bitmap port at least
//                every 4 * (lat_max + stall_max) + 128 cycles, and every sweep ends (no deadlock).
//   added cycle  phase B: each verdict is presented to the engine (upstream_rsp_valid_o with the
//                bitmap response) in the cycle the later of its data word and its bitmap word is
//                there: the filter adds no cycle when the bitmap answers at once (the row). The
//                same is measured and logged in phase A.
//   core path    phase B: REQ_CMS_07 on core data and capability reads and writes: no cycle added
//                to a read or to a write that keeps the tag, at most 2 per 32-bit word to one that
//                changes it (cms_latency allows 4 per word: see its row).
//   stimulus     at least one verdict measured in phase B (else the measurement saw nothing); the
//                orders in which the data and bitmap words came are logged and covered.
// +cms_sb_corrupt=backpressure,1 expects the first phase-B verdict one cycle late, so the run must
// fail on it. Xcelium only (hierarchical references into the filter).

localparam int unsigned BpTbreInFlight   = 2;
localparam int unsigned BpLookupInFlight = 2;
localparam int unsigned BpChangeMax      = 2;  // REQ_CMS_07: per 32-bit word, a tag-changing write

int unsigned bp_lat_max = 1, bp_stall_max = 1, bp_progress_bound = 1000;
bit          bp_on = 1'b0, bp_phase_b = 1'b0;
int unsigned bp_tbre_out, bp_tbre_out_max, bp_lk_out, bp_lk_out_max, bp_gap_max;
int unsigned bp_verdicts, bp_verdicts_b, bp_data_first, bp_bitmap_first, bp_same_cycle;
int unsigned bp_added_max, bp_added_bad, bp_out_bad, bp_gap_bad;
bit          bp_ds_seen, bp_bm_seen, bp_verdict_done;
longint unsigned bp_ds_first, bp_bm_first, bp_last_progress;

int unsigned bp_cov_order, bp_cov_phase;  // order: 0 data first, 1 bitmap first, 2 same cycle
covergroup cms_trvk_bp_cg;
  option.per_instance = 1;
  cp_order: coverpoint bp_cov_order { bins data_first = {0}; bins bitmap_first = {1}; bins same = {2}; }
  cp_phase: coverpoint bp_cov_phase { bins backpressure = {0}; bins bitmap_at_once = {1}; }
  x_order_phase: cross cp_order, cp_phase;
endgroup
cms_trvk_bp_cg bp_cg = new();

wire bp_ds_v, bp_ds_hs, bp_bm_v, bp_bm_hs, bp_up_v, bp_lk_a, bp_lk_d, bp_active;
`ifndef VERILATOR
`define CMS_BP_TRVK u_dut.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core
assign bp_ds_v   = `CMS_BP_TRVK.downstream_rsp_valid_i;
assign bp_ds_hs  = `CMS_BP_TRVK.downstream_rsp_valid_i && `CMS_BP_TRVK.downstream_rsp_ready_o;
assign bp_bm_v   = `CMS_BP_TRVK.revbm_rsp_valid_i;
assign bp_bm_hs  = `CMS_BP_TRVK.revbm_rsp_valid_i && `CMS_BP_TRVK.revbm_rsp_ready_o;
assign bp_up_v   = `CMS_BP_TRVK.upstream_rsp_valid_o;
assign bp_lk_a   = u_dut.tbre_revbm_tl_h2d.a_valid && u_dut.tbre_revbm_tl_d2h.a_ready;
assign bp_lk_d   = u_dut.tbre_revbm_tl_d2h.d_valid && u_dut.tbre_revbm_tl_h2d.d_ready;
assign bp_active = u_dut.tbre_active;
`undef CMS_BP_TRVK
`else
assign {bp_ds_v, bp_ds_hs, bp_bm_v, bp_bm_hs, bp_up_v, bp_lk_a, bp_lk_d, bp_active} = '0;
`endif

always @(posedge clk) begin
  if (!rst_n || !bp_on) begin
    bp_tbre_out      = 0;
    bp_lk_out        = 0;
    bp_ds_seen       = 0;
    bp_bm_seen       = 0;
    bp_verdict_done  = 0;
    bp_last_progress = cms_cycle;
  end else begin
    int unsigned added;
    int          lo;
    // Outstanding requests (a response frees its slot in the cycle it is taken)
    bp_tbre_out = bp_tbre_out + 32'(tbre_h2d.a_valid && tbre_d2h.a_ready)
                              - 32'(tbre_d2h.d_valid && tbre_h2d.d_ready);
    bp_lk_out   = bp_lk_out + 32'(bp_lk_a) - 32'(bp_lk_d);
    if (bp_tbre_out > bp_tbre_out_max) bp_tbre_out_max = bp_tbre_out;
    if (bp_lk_out > bp_lk_out_max) bp_lk_out_max = bp_lk_out;
    if (bp_tbre_out > BpTbreInFlight && bp_out_bad++ < 4)
      cms_error("test", $sformatf("trvk_backpressure: %0d reads in flight on tbre_tl_h, at most %0d (theory_of_operation.md)",
                                  bp_tbre_out, BpTbreInFlight));
    if (bp_lk_out > BpLookupInFlight && bp_out_bad++ < 4)
      cms_error("test", $sformatf("trvk_backpressure: %0d bitmap lookups in flight, at most %0d",
                                  bp_lk_out, BpLookupInFlight));
    // Forward progress while the engine is active
    if (!bp_active || (tbre_d2h.d_valid && tbre_h2d.d_ready) || bp_lk_d) bp_last_progress = cms_cycle;
    if (int'(cms_cycle - bp_last_progress) > int'(bp_gap_max)) bp_gap_max = 32'(cms_cycle - bp_last_progress);
    if (cms_cycle - bp_last_progress == longint'(bp_progress_bound) && bp_gap_bad++ < 4)
      cms_error("test", $sformatf("trvk_backpressure: the engine is active and neither of its ports answered for %0d cycles (lat_max %0d, stall_max %0d)",
                                  bp_progress_bound, bp_lat_max, bp_stall_max));
    // The verdict: the cycle it is presented against the later of its two words
    if (bp_ds_v && !bp_ds_seen) begin bp_ds_seen = 1; bp_ds_first = cms_cycle; end
    if (bp_bm_v && !bp_bm_seen) begin bp_bm_seen = 1; bp_bm_first = cms_cycle; end
    if (bp_bm_v && bp_up_v && bp_ds_seen && !bp_verdict_done) begin
      bp_verdict_done = 1;
      added = 32'(cms_cycle - ((bp_ds_first > bp_bm_first) ? bp_ds_first : bp_bm_first));
      bp_cov_order = (bp_ds_first < bp_bm_first) ? 0 : ((bp_bm_first < bp_ds_first) ? 1 : 2);
      bp_cov_phase = 32'(bp_phase_b);
      bp_cg.sample();
      case (bp_cov_order)
        0: bp_data_first++;
        1: bp_bitmap_first++;
        default: bp_same_cycle++;
      endcase
      bp_verdicts++;
      if (added > bp_added_max) bp_added_max = added;
      if (bp_phase_b) begin
        bp_verdicts_b++;
        lo = 0;
        if (sb.corrupt("backpressure", $sformatf("verdict at cycle %0d", cms_cycle))) lo = 1;
        if (int'(added) != lo && bp_added_bad++ < 4)
          cms_error("test", $sformatf("trvk_backpressure: the bitmap answered at once and the filter presented its verdict %0d cycle(s) after the later of its data word (cycle %0d) and its bitmap word (cycle %0d); the row allows %0d",
                                      added, bp_ds_first, bp_bm_first, lo));
      end
    end
    if (bp_ds_hs) bp_ds_seen = 0;
    if (bp_bm_hs) begin
      bp_bm_seen      = 0;
      bp_verdict_done = 0;
    end
  end
end

// Holds a_ready of one or all memories, or the core's d_ready, at 0 for stretches.
task automatic bp_staller(ref bit stop);
  int unsigned which, len, p_tbre, p_meta, p_dmem, p_core;
  while (!stop) begin
    wait_cycles($urandom_range(bp_stall_max, 1));
    if (stop) break;
    which  = $urandom_range(4);
    len    = $urandom_range(bp_stall_max, bp_stall_max / 4 + 1);
    p_tbre = u_tbre_mem.a_ready_pct;
    p_meta = u_meta.a_ready_pct;
    p_dmem = u_dmem.a_ready_pct;
    p_core = u_core.d_ready_pct;
    if (which == 0 || which == 4) u_tbre_mem.a_ready_pct = 0;
    if (which == 1 || which == 4) u_meta.a_ready_pct = 0;
    if (which == 2 || which == 4) u_dmem.a_ready_pct = 0;
    if (which == 3) u_core.d_ready_pct = 0;
    wait_cycles(len);
    u_tbre_mem.a_ready_pct = p_tbre;
    u_meta.a_ready_pct     = p_meta;
    u_dmem.a_ready_pct     = p_dmem;
    u_core.d_ready_pct     = p_core;
  end
endtask

// Core traffic beside a sweep, until `stop`.
task automatic bp_core_traffic(ref bit stop);
  logic [31:0] a, w0, w1, cb;
  cms_txn_t    r;
  while (!stop) begin
    a = rand_granule(MainSramBase, MainSramTop);
    case ($urandom_range(7))
      0, 1: begin
        core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1));
        core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1));
      end
      2: begin
        make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
        core_store_cap(a, w0, w1, 1'b1);
      end
      3: core_issue(mk(PutFullData, a + $urandom_range(7), 2'd0, $urandom));
      4: heap_base(rbit(), cb);
      5: run(PortCoreRevbm, mk(Get, MetaRevbmBase + 4 * $urandom_range(RevbmBytes / 4 - 1)), r);
      default: core_issue(mk(Get, UntaggedBase + 4 * $urandom_range(255), 2'd2));
    endcase
    throttle(2);
    wait_cycles($urandom_range(20));
  end
endtask

task automatic bp_set_latency(int unsigned lat_max, int unsigned ready_lo);
  u_tbre_mem.lat_min = 1; u_tbre_mem.lat_max = lat_max; u_tbre_mem.a_ready_pct = $urandom_range(100, ready_lo);
  u_meta.lat_min     = 1; u_meta.lat_max     = lat_max; u_meta.a_ready_pct     = $urandom_range(100, ready_lo);
  u_dmem.lat_min     = 1; u_dmem.lat_max     = lat_max; u_dmem.a_ready_pct     = $urandom_range(100, ready_lo);
  u_core.a_gap_max   = $urandom_range(3);
  u_core.d_ready_pct = $urandom_range(100, ready_lo);
endtask

task automatic t_trvk_backpressure();
  int unsigned sweeps, n;
  logic [31:0] b, g, w0, w1;
  bit          stop;
`ifdef VERILATOR
  cms_error("test", "cms_trvk_backpressure needs Xcelium (hierarchical references into the filter)");
  return;
`endif
  sweeps = 6;
  void'($value$plusargs("cms_bp_sweeps=%d", sweeps));
  cheriot_on();
  bp_on = 1'b1;

  // Phase A: back pressure
  for (int unsigned s = 0; s < sweeps; s++) begin
    bp_lat_max        = $urandom_range(160, 16);
    bp_stall_max      = $urandom_range(400, 32);
    bp_progress_bound = 4 * (bp_lat_max + bp_stall_max) + 128;
    bp_set_latency(bp_lat_max, 30);
    n = $urandom_range(128, 32);
    b = MainSramBase + 8 * $urandom_range(SramCaps - n);
    fill_caps(b, n);
    cms_info("trvk_backpressure", $sformatf("sweep %0d: %0d capabilities from 0x%08x, latency 1..%0d, stalls up to %0d cycles",
                                            s, n, b, bp_lat_max, bp_stall_max));
    stop = 0;
    fork
      bp_staller(stop);
      bp_core_traffic(stop);
      begin
        tbre_sweep(b, n, 1'b1);
        stop = 1;
      end
    join
    wait_quiet();
    check_tags($sformatf("back-pressure sweep %0d", s));
  end
  cms_info("trvk_backpressure", $sformatf("phase A: %0d verdicts (data first %0d, bitmap first %0d, same cycle %0d), at most %0d cycle(s) added; in flight at most %0d reads, %0d lookups; longest wait %0d cycles",
                                          bp_verdicts, bp_data_first, bp_bitmap_first, bp_same_cycle,
                                          bp_added_max, bp_tbre_out_max, bp_lk_out_max, bp_gap_max));

  // Phase B: the bitmap answers at once
  bp_phase_b         = 1'b1;
  bp_lat_max         = 8;
  bp_stall_max       = 1;
  bp_progress_bound  = 4 * (bp_lat_max + bp_stall_max) + 128;
  randomize_timing(1'b1);
  u_meta.lat_min     = 1; u_meta.lat_max = 1; u_meta.a_ready_pct = 100;
  u_tbre_mem.lat_min = 1; u_tbre_mem.lat_max = $urandom_range(8, 1); u_tbre_mem.a_ready_pct = 100;
  n = $urandom_range(128, 32);
  b = MainSramBase + 8 * $urandom_range(SramCaps - n);
  fill_caps(b, n);
  tbre_sweep(b, n, 1'b1);
  wait_quiet();
  check_tags("bitmap-at-once sweep");
  if (bp_verdicts_b == 0)
    cms_error("test", "trvk_backpressure: no verdict measured with the bitmap answering at once (the check saw nothing)");
  bp_on = 1'b0;

  // Phase B, core path (REQ_CMS_07), every memory at full speed
  u_core.a_gap_max   = 0;
  u_core.d_ready_pct = 100;
  u_dmem.a_ready_pct = 100; u_dmem.lat_min = 1; u_dmem.lat_max = 1;
  u_meta.a_ready_pct = 100; u_meta.lat_min = 1; u_meta.lat_max = 1;
  lat_change_max = BpChangeMax;
  foreach (lat_cls_names[cls]) begin
    for (int unsigned pre = 0; pre < 2; pre++) begin
      for (int unsigned t = 0; t < ((cls == LatCapWr) ? 2 : 1); t++) begin
        g = rand_granule(MainSramBase, MainSramTop);
        make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
        core_store_cap(g, w0, w1, 1'(pre));
        wait_quiet();
        lat_one(cls, g, 1'(pre), 1'(t), 0, prim_mubi_pkg::MuBi4True);
      end
    end
  end
  lat_change_max = LatMaxChange;
  cms_info("trvk_backpressure", $sformatf("all: %0d verdicts (%0d with the bitmap at once), data first %0d, bitmap first %0d, same cycle %0d; at most %0d cycle(s) added",
                                          bp_verdicts, bp_verdicts_b, bp_data_first, bp_bitmap_first,
                                          bp_same_cycle, bp_added_max));
  expect_no_alert("trvk_backpressure");
endtask
