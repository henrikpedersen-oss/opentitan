// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

`include "prim_assert.sv"

/**
 * Register slice for an Ibex-protocol memory port (req/gnt request phase, rvalid response phase),
 * for timing: every output of either side comes from a flop or from the other outputs of its own
 * side, so no input of one side reaches an output of the other in the same cycle.
 *
 * - Requests go through a 2-entry FIFO. The upstream grant is the FIFO not being full, a flop, and a
 *   request is presented downstream from the cycle after it was granted upstream. Two entries keep
 *   back-to-back requests at one per cycle while the downstream grants at once.
 * - Responses are registered: upstream sees each response one cycle after it arrives downstream.
 *
 * Order and spacing are kept: upstream sees the same responses in the same order, each one cycle
 * later, so a consumer that relies on two responses arriving in consecutive cycles still sees that.
 * The upstream grant can come before the downstream one, so the number of requests granted upstream
 * and not yet answered is at least the number outstanding downstream: an upstream limit on
 * outstanding requests also holds downstream.
 */
module ibex_mem_if_reg #(
  parameter int unsigned ReqWidth = 32,  // request payload: address, write data, byte enables, ...
  parameter int unsigned RspWidth = 32   // response payload: read data, error, ...
) (
  input  logic                clk_i,
  input  logic                rst_ni,

  // Upstream: the requester (core side)
  input  logic                up_req_i,
  output logic                up_gnt_o,
  input  logic [ReqWidth-1:0] up_req_data_i,
  output logic                up_rvalid_o,
  output logic [RspWidth-1:0] up_rsp_data_o,

  // Downstream: the memory (bus side)
  output logic                dn_req_o,
  input  logic                dn_gnt_i,
  output logic [ReqWidth-1:0] dn_req_data_o,
  input  logic                dn_rvalid_i,
  input  logic [RspWidth-1:0] dn_rsp_data_i
);

  logic fifo_wready;

  // Pass = 0: a request never goes straight through an empty FIFO, so dn_req_o and dn_req_data_o
  // come from its storage and occupancy flops. OutputZeroIfEmpty keeps the payload 0, not X, while
  // no request is presented.
  prim_fifo_sync #(
    .Width            (ReqWidth),
    .Pass             (1'b0),
    .Depth            (2),
    .OutputZeroIfEmpty(1'b1)
  ) u_req_fifo (
    .clk_i,
    .rst_ni,
    .clr_i   (1'b0),
    .wvalid_i(up_req_i),
    .wready_o(fifo_wready),
    .wdata_i (up_req_data_i),
    .rvalid_o(dn_req_o),
    .rready_i(dn_gnt_i),
    .rdata_o (dn_req_data_o),
    .full_o  (),
    .depth_o (),
    .err_o   ()
  );

  // The grant only with a request, as the memory protocol has it (ibex/doc/02_user/integration.rst).
  assign up_gnt_o = up_req_i & fifo_wready;

  logic                rvalid_q;
  logic [RspWidth-1:0] rsp_data_q;

  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rvalid_q   <= 1'b0;
      rsp_data_q <= '0;
    end else begin
      rvalid_q <= dn_rvalid_i;
      if (dn_rvalid_i) begin
        rsp_data_q <= dn_rsp_data_i;
      end
    end
  end

  assign up_rvalid_o   = rvalid_q;
  assign up_rsp_data_o = rsp_data_q;

  // A request presented downstream stays, with its payload, until it is granted.
  `ASSERT(IbexMemIfRegReqStable, dn_req_o && !dn_gnt_i |=> dn_req_o && $stable(dn_req_data_o))
  // The upstream port holds a request with its payload until granted (the memory protocol); the
  // FIFO takes it in the cycle it is granted.
  `ASSERT(IbexMemIfRegUpReqStable, up_req_i && !up_gnt_o |=> up_req_i && $stable(up_req_data_i))

endmodule
