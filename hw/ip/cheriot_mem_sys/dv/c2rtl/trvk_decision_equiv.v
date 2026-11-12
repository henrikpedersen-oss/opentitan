// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Miter of the revocation decision: Bambu's Verilog of the C model's decision
// (trvk_decision_hls.c) against cheriot_trvk_core. The RTL's stored pointer and the flags of the
// decision are cut (trvk_decision.ys, expose -cut), so every pointer, every capability word 1 and
// every heap base is covered, not only the ones a load sequence reaches. The RTL is shown the
// state in which it must decide: the stored pointer valid, the second word of a tagged capability
// load answered, no lookup outstanding.
//
// bad: the RTL asks for a lookup when the C model does not, or the other way round, or, when both
//      do, the word address or the bit differ.
// done_seen: the HLS transaction finished (the comparison is not vacuous).

module trvk_decision_equiv (
  input             clock,
  input      [31:0] w0_i,
  input      [31:0] w1_i,
  input      [31:0] heap_base_i,
  output reg        bad,
  output reg        done_seen
);
  localparam [31:0] RevBitmapBaseAddr = 32'h1100_0000;

  reg [8:0]  phase;
  reg [31:0] w0, w1, heap_base;

  wire reset      = (phase == 0);
  wire start_port = (phase == 1);
  wire done_port;
  wire [31:0] hls_o;

  trvk_decision_hls u_hls (
    .clock      (clock),
    .reset      (!reset),  // Bambu resets on low
    .start_port (start_port),
    .done_port  (done_port),
    .return_port(hls_o),
    .w0         (w0),
    .w1         (w1),
    .heap_base  (heap_base)
  );

  wire        rtl_req_valid;
  wire [31:0] rtl_req_addr;
  wire [4:0]  rtl_bit;

  cheriot_trvk_core u_rtl (
    .clk_i                     (clock),
    .rst_ni                    (1'b1),
    .heap_base_addr_i          (heap_base),
    .upstream_req_valid_i      (1'b0),
    .upstream_req_ready_o      (),
    .upstream_req_misalign_i   (1'b0),
    .upstream_tag_i            (1'b1),
    .upstream_rsp_valid_o      (),
    .upstream_rsp_ready_i      (1'b1),
    .upstream_tag_o            (),
    .downstream_req_valid_o    (),
    .downstream_req_ready_i    (1'b1),
    .downstream_tag_o          (),
    .downstream_rsp_valid_i    (1'b1),
    .downstream_rsp_ready_o    (),
    .downstream_rsp_data_i     (w1),
    .downstream_tag_i          (1'b1),
    .revbm_req_valid_o         (rtl_req_valid),
    .revbm_req_ready_i         (1'b1),
    .revbm_req_addr_o          (rtl_req_addr),
    .revbm_rsp_valid_i         (1'b0),
    .revbm_rsp_ready_o         (),
    .revbm_rsp_data_i          (32'h0),
    .revbm_rsp_data_intg_i     (7'h0),
    .revbm_rsp_err_i           (1'b0),
    .revbm_data_intg_error_o   (),
    // The cut state (expose -cut): the decision's inputs other than the response
    .\ptr_storage_q.i          (w0),
    .\ptr_storage_valid_q.i    (1'b1),
    .\revbm_outstanding_q.i    (1'b0),
    .\misalign_flag_out.i      (1'b1),
    .\misalign_flag_out_valid.i (1'b1),
    // Exposed for the comparison of the bit
    .revbm_bit_select          (rtl_bit),
    .revbm_addr                ()
  );

  wire        hls_lookup = hls_o[0];
  wire [4:0]  hls_bit    = hls_o[5:1];
  wire [31:0] hls_addr   = RevBitmapBaseAddr + {16'h0, hls_o[15:6], 2'b00};

  always @(posedge clock) begin
    phase <= phase + 1'b1;
    if (reset) begin
      w0 <= w0_i;
      w1 <= w1_i;
      heap_base <= heap_base_i;
      bad <= 1'b0;
      done_seen <= 1'b0;
    end else if (done_port) begin
      bad <= bad | (rtl_req_valid != hls_lookup) |
             (hls_lookup && (rtl_req_addr != hls_addr || rtl_bit != hls_bit));
      done_seen <= 1'b1;
    end
  end
endmodule
