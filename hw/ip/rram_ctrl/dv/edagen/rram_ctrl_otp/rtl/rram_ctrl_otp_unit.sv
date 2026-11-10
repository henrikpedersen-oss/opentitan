// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl_otp, the OTP hardware plug, for its unit bench (edagen rram_ctrl_otp.yaml). Bench
// wrapper only, no logic of its own: it flattens the struct ports into vectors for edagen's
// signal-level agents, one per clock domain:
//   ot_*  the otp_ctrl macro interface (otp_ctrl_macro_req_t / otp_ctrl_macro_rsp_t), clk_otp_i;
//   ct_*  the controller path towards rram_ctrl_arb / rram_ctrl_rd (the CONTROL fields, request,
//         address, done, error, read and write bus words with their TL-UL integrity), and the
//         status inputs and fatal outputs, clk_i.
`include "prim_assert.sv"

module rram_ctrl_otp_unit
  import otp_ctrl_macro_pkg::*;
  import rram_ctrl_pkg::*;
(
  input  logic         clk_i,
  input  logic         rst_ni,
  input  logic         clk_otp_i,
  input  logic         rst_otp_ni,
  // otp_ctrl side
  input  logic         ot_valid_i,
  input  logic [6:0]   ot_cmd_i,
  input  logic [1:0]   ot_size_i,
  input  logic [10:0]  ot_addr_i,
  input  logic [63:0]  ot_wdata_i,
  output logic         ot_ready_o,
  output logic         ot_rvalid_o,
  output logic [63:0]  ot_rdata_o,
  output logic [2:0]   ot_err_o,
  output logic         ot_fatal_lc_fsm_err_o,
  output logic         ot_fatal_alert_o,
  output logic         ot_recov_alert_o,
  // controller side
  output logic         ct_start_o,
  output logic [1:0]   ct_op_o,
  output logic         ct_part_o,
  output logic [9:0]   ct_num_o,
  output logic         ct_req_o,
  output logic [20:0]  ct_addr_o,
  input  logic         ct_done_i,
  input  logic [3:0]   ct_err_i,     // {invalid_op_err, mp_err, rd_err, wr_err}
  output logic         ct_rready_o,
  input  logic         ct_rvalid_i,
  input  logic [38:0]  ct_rdata_i,   // {integrity[6:0], data[31:0]}
  output logic         ct_wvalid_o,
  input  logic         ct_wready_i,
  output logic [38:0]  ct_wdata_o,
  output logic         ct_fatal_err_o,
  output logic         ct_intg_err_o,
  input  logic         ct_init_done_i,
  input  logic         ct_wr_busy_i,
  input  logic [3:0]   ct_rram_dis_i   // disable_i (ct_disable_i would be the SV keyword "disable")
);

  otp_ctrl_macro_req_t req;
  otp_ctrl_macro_rsp_t rsp;
  rram_ctrl_reg_pkg::rram_ctrl_reg2hw_control_reg_t ctrl;
  rram_ctrl_err_t err;

  assign req = '{valid: ot_valid_i,
                 cmd:   cmd_e'(ot_cmd_i),
                 size:  ot_size_i,
                 addr:  ot_addr_i,
                 wdata: ot_wdata_i};
  assign err = rram_ctrl_err_t'(ct_err_i);

  rram_ctrl_otp u_otp (
    .clk_i,
    .rst_ni,
    .clk_otp_i,
    .rst_otp_ni,
    .otp_macro_req_i (req),
    .otp_macro_rsp_o (rsp),
    .ctrl_o          (ctrl),
    .req_o           (ct_req_o),
    .addr_o          (ct_addr_o),
    .done_i          (ct_done_i),
    .err_i           (err),
    .rready_o        (ct_rready_o),
    .rvalid_i        (ct_rvalid_i),
    .rdata_i         (ct_rdata_i),
    .wvalid_o        (ct_wvalid_o),
    .wready_i        (ct_wready_i),
    .wdata_o         (ct_wdata_o),
    .fatal_err_o     (ct_fatal_err_o),
    .intg_err_o      (ct_intg_err_o),
    .rram_init_done_i(ct_init_done_i),
    .rram_wr_busy_i  (ct_wr_busy_i),
    .disable_i       (prim_mubi_pkg::mubi4_t'(ct_rram_dis_i))
  );

  assign ot_ready_o            = rsp.ready;
  assign ot_rvalid_o           = rsp.rvalid;
  assign ot_rdata_o            = rsp.rdata;
  assign ot_err_o              = rsp.err;
  assign ot_fatal_lc_fsm_err_o = rsp.fatal_lc_fsm_err;
  assign ot_fatal_alert_o      = rsp.fatal_alert;
  assign ot_recov_alert_o      = rsp.recov_alert;
  // The plug's sparse FSM and its bus-word counter report their errors as fatal_err_o (the
  // checks the primitives ask their parent for; rram_ctrl.sv does the same for the whole block).
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ALERT_IN(OtpFsmCheck_A, u_otp.u_state_regs, ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(OtpBusCntCheck_A, u_otp.u_bus_wcnt, ct_fatal_err_o)

  assign ct_start_o            = ctrl.start.q;
  assign ct_op_o               = ctrl.op.q;
  assign ct_part_o             = ctrl.partition.q;
  assign ct_num_o              = ctrl.num.q;

`ifndef SYNTHESIS
  // Simulation only, +otp_trace: the plug's state, error and integrity registers when they change.
  bit trace_on;
  initial trace_on = $test$plusargs("otp_trace");
  always @(posedge clk_i) begin
    if (trace_on && rst_ni && (u_otp.state_d != u_otp.state_q || u_otp.err_d != u_otp.err_q))
      $display("%0t otp_trace state %s -> %s err %0d -> %0d intg_q %02h intg_ecc %02h valid_d %b",
               $time, u_otp.state_q.name(), u_otp.state_d.name(), u_otp.err_q, u_otp.err_d,
               u_otp.intg_q, u_otp.intg_ecc, u_otp.valid_d);
  end
`endif

endmodule
