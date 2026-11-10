// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// TL-UL host driver for one device port of the CHERIoT memory subsystem, with the capability tag
// sideband of the core port (tag_o with the request, tag_i with the response; tie off elsewhere).
//
// Requests are queued with issue() and driven in order, with up to MaxOutstanding in flight and a
// random gap of 0..a_gap_max cycles before each; d_ready is withheld at random (d_ready_pct). A
// request marked hold_after holds back the next one until it is answered.
// Responses must come back in order (one requester, in-order memories); each completed transaction
// goes to done_q, which cms_tb hands to the scoreboard, and to rsp_by_id for the test waiting on it.
// Command and data integrity are generated for every request (bad_cmd_intg inverts cmd_intg).
// Checks of the response itself: d_source, d_opcode, d_size; no response without a request; its
// response and data integrity, which the subsystem regenerates for every response it makes or
// changes, error responses included (chk_rsp_intg; cleared only by a test that injects an integrity
// fault the subsystem passes on unchanged).

module cms_tl_host
  import cms_pkg::*;
#(
  parameter string       Name           = "host",
  parameter port_e       Port           = PortCore,
  parameter int unsigned MaxOutstanding = 2
) (
  input  logic                  clk_i,
  input  logic                  rst_ni,
  input  prim_mubi_pkg::mubi4_t ena_i,
  output tlul_pkg::tl_h2d_t     tl_o,
  input  tlul_pkg::tl_d2h_t     tl_i,
  output logic                  tag_o,
  input  logic                  tag_i
);

  // Knobs, set by the test
  int unsigned a_gap_max    = 0;
  int unsigned d_ready_pct  = 100;
  bit          chk_rsp_intg = 1'b1;

  cms_txn_t    req_q[$];
  cms_txn_t    pend_q[$];
  cms_txn_t    done_q[$];
  cms_txn_t    rsp_by_id[int unsigned];
  int unsigned next_id = 1;
  int unsigned n_done  = 0;
  int unsigned n_flushed = 0;

  cms_txn_t    cur;
  bit          cur_valid;
  int unsigned gap;
  logic        unused_tag_i;

  assign unused_tag_i = tag_i;

  // Queue a request; returns its id.
  function automatic int unsigned issue(cms_txn_t t);
    t.id   = next_id++;
    t.port = Port;
    req_q.push_back(t);
    return t.id;
  endfunction

  function automatic bit idle();
    return req_q.size() == 0 && pend_q.size() == 0 && !cur_valid;
  endfunction

  function automatic int unsigned in_flight();
    return req_q.size() + pend_q.size() + (cur_valid ? 1 : 0);
  endfunction

  function automatic tlul_pkg::tl_h2d_t to_tl(cms_txn_t t);
    tlul_pkg::tl_h2d_t h;
    h = tlul_pkg::TL_H2D_DEFAULT;
    h.a_valid   = 1'b1;
    h.a_opcode  = t.opcode;
    h.a_param   = 3'b000;
    h.a_size    = t.size;
    h.a_source  = t.source;
    h.a_address = t.addr;
    h.a_mask    = t.mask;
    h.a_data    = (t.opcode == tlul_pkg::Get) ? 32'h0 : t.wdata;
    h.a_user    = tlul_pkg::TL_A_USER_DEFAULT;
    h.a_user.instr_type = prim_mubi_pkg::MuBi4False;
    h.a_user.cmd_intg   = tlul_pkg::get_cmd_intg(h);
    h.a_user.data_intg  = tlul_pkg::get_data_intg(h.a_data);
    if (t.bad_cmd_intg) h.a_user.cmd_intg = ~h.a_user.cmd_intg;
    h.d_ready = 1'b1;
    return h;
  endfunction

  function automatic bit holding();
    foreach (pend_q[i]) if (pend_q[i].hold_after) return 1;
    return 0;
  endfunction

  function automatic logic [7:0] free_source();
    for (int unsigned s = 0; s < MaxOutstanding; s++) begin
      bit used;
      used = 0;
      foreach (pend_q[i]) if (pend_q[i].source == 8'(s)) used = 1;
      if (!used) return 8'(s);
    end
    return 8'hff;
  endfunction

  // Outputs are only assigned here, with non-blocking assignments; internal state is blocking.
  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      // Everything in flight is lost with the reset.
      n_flushed += pend_q.size() + (cur_valid ? 1 : 0);
      pend_q.delete();
      cur_valid = 0;
      gap       = 0;
      tl_o      <= '{a_valid: 1'b0, d_ready: 1'b1, a_opcode: tlul_pkg::Get,
                     a_user: tlul_pkg::TL_A_USER_DEFAULT, default: '0};
      tag_o     <= 1'b0;
    end else begin
      // D channel (sampled before this edge's updates)
      if (tl_i.d_valid && tl_o.d_ready) begin
        if (pend_q.size() == 0) begin
          cms_error(Name, $sformatf("response (source %0d) with no request outstanding",
                                    tl_i.d_source));
        end else begin
          cms_txn_t t;
          t = pend_q.pop_front();
          if (tl_i.d_source != t.source)
            cms_error(Name, $sformatf("response source %0d, expected %0d (in order)",
                                      tl_i.d_source, t.source));
          if (chk_rsp_intg && (tl_i.d_user.rsp_intg != tlul_pkg::get_rsp_intg(tl_i) ||
                               tl_i.d_user.data_intg != tlul_pkg::get_data_intg(tl_i.d_data)))
            cms_error(Name, $sformatf({"response to %s 0x%08x (d_error %b) has bad integrity: ",
                                       "rsp_intg 0x%02x, expected 0x%02x; data_intg 0x%02x, ",
                                       "expected 0x%02x"}, t.opcode.name(), t.addr, tl_i.d_error,
                                      tl_i.d_user.rsp_intg, tlul_pkg::get_rsp_intg(tl_i),
                                      tl_i.d_user.data_intg, tlul_pkg::get_data_intg(tl_i.d_data)));
          t.rdata    = tl_i.d_data;
          t.rtag     = tag_i;
          t.err      = tl_i.d_error;
          t.d_opcode = tl_i.d_opcode;
          t.d_size   = tl_i.d_size;
          t.d_cycle  = cms_cycle;
          done_q.push_back(t);
          rsp_by_id[t.id] = t;
          n_done++;
        end
      end
      tl_o.d_ready <= ($urandom_range(99) < d_ready_pct);

      // A channel
      if (cur_valid && tl_o.a_valid && tl_i.a_ready) begin
        cur.a_cycle = cms_cycle;
        cur.ena     = ena_i;
        pend_q.push_back(cur);
        cur_valid = 0;
        gap = (a_gap_max > 0) ? $urandom_range(a_gap_max) : 0;
      end
      if (!cur_valid && gap > 0) gap--;
      else if (!cur_valid && req_q.size() > 0 && pend_q.size() < MaxOutstanding && !holding()) begin
        logic [7:0] src;
        src = free_source();
        if (src != 8'hff) begin
          cur        = req_q.pop_front();
          cur.source = src;
          cur_valid  = 1;
        end
      end
      if (cur_valid) begin
        tl_o.a_valid   <= 1'b1;
        tl_o.a_opcode  <= cur.opcode;
        tl_o.a_param   <= 3'b000;
        tl_o.a_size    <= cur.size;
        tl_o.a_source  <= cur.source;
        tl_o.a_address <= cur.addr;
        tl_o.a_mask    <= cur.mask;
        tl_o.a_data    <= to_tl(cur).a_data;
        tl_o.a_user    <= to_tl(cur).a_user;
        tag_o          <= cur.tag;
      end else begin
        tl_o.a_valid   <= 1'b0;
        tag_o          <= 1'b0;
      end
    end
  end

  // Flush the request queue as well (the test re-issues after a reset).
  function automatic void flush();
    n_flushed += req_q.size();
    req_q.delete();
  endfunction

endmodule
