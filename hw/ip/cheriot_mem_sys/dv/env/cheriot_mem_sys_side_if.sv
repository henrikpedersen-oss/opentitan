// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The core data port's sidebands, which the TL-UL agent does not carry: the capability tag with
// each request (`cored_tag_h2d_i`) and with each response (`cored_tag_d2h_o`), and `cheriot_ena_i`.
//
// A sequence queues the tag of its next request with push_tag() before it starts the request: the
// tag is presented with the request at the head of the queue and popped when the request is taken,
// as the core does (it holds a request with its tag until the request is accepted). Each accepted
// request's tag and the mode at that moment are kept by source ID, and each response's tag is
// recorded, for the C-model scoreboard to pair with the TL-UL monitor's items.
interface cheriot_mem_sys_side_if (
  input logic              clk,
  input logic              rst_n,
  input tlul_pkg::tl_h2d_t h2d,       // cored_tl_d_i
  input tlul_pkg::tl_d2h_t d2h,       // cored_tl_d_o
  input logic              tag_d2h    // cored_tag_d2h_o
);
  logic                  tag_h2d;
  prim_mubi_pkg::mubi4_t cheriot_ena = prim_mubi_pkg::MuBi4False;

  bit tag_q[$];
  event a_taken;  // a request was accepted on the core data port
  // Per source ID, in order: each accepted request's tag and mode, each response's tag. A source
  // answers in order, so the scoreboard pops them as it takes the monitor's items (get_*() wait if
  // the entry is not recorded yet in this time step).
  bit                    req_tag_q[int unsigned][$];
  prim_mubi_pkg::mubi4_t req_ena_q[int unsigned][$];
  bit                    rsp_tag_q[int unsigned][$];
  bit                    vseq_rsp_tag_q[int unsigned][$];  // the same, for the sequences

  assign tag_h2d = (tag_q.size() > 0) ? tag_q[0] : 1'b0;

  function automatic void push_tag(bit tag);
    tag_q.push_back(tag);
  endfunction

  task automatic get_req(int unsigned source, output bit tag, output prim_mubi_pkg::mubi4_t ena);
    wait (req_tag_q.exists(source) && req_tag_q[source].size() > 0);
    tag = req_tag_q[source].pop_front();
    ena = req_ena_q[source].pop_front();
  endtask

  task automatic get_vseq_rsp_tag(int unsigned source, output bit tag);
    wait (vseq_rsp_tag_q.exists(source) && vseq_rsp_tag_q[source].size() > 0);
    tag = vseq_rsp_tag_q[source].pop_front();
  endtask

  task automatic get_rsp_tag(int unsigned source, output bit tag);
    wait (rsp_tag_q.exists(source) && rsp_tag_q[source].size() > 0);
    tag = rsp_tag_q[source].pop_front();
  endtask

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      tag_q.delete();
      req_tag_q.delete();
      req_ena_q.delete();
      rsp_tag_q.delete();
      vseq_rsp_tag_q.delete();
    end else begin

      if (h2d.a_valid && d2h.a_ready) begin
        req_tag_q[h2d.a_source].push_back(tag_h2d);
        req_ena_q[h2d.a_source].push_back(cheriot_ena);
        if (tag_q.size() > 0) void'(tag_q.pop_front());
        -> a_taken;
      end
      if (d2h.d_valid && h2d.d_ready) begin
        rsp_tag_q[d2h.d_source].push_back(tag_d2h);
        vseq_rsp_tag_q[d2h.d_source].push_back(tag_d2h);
      end
    end
  end
endinterface
