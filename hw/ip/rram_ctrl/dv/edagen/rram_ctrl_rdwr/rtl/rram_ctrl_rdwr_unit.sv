// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl's controller path as one unit: the arbiter (rram_ctrl_arb) between the software
// interface and the two hardware plugs (rram_ctrl_otp, rram_ctrl_lcmgr), the read engine
// (rram_ctrl_rd), the write engine (rram_ctrl_wr), and the write FIFO they share (prim_fifo_sync,
// depth WrFifoDepth = 16, Secure), wired as rram_ctrl.sv wires them. Bench wrapper only (edagen
// rram_ctrl_rdwr.yaml): the CONTROL structs are flattened into start/op/part/num, the multibit
// disables into vectors, the error structs into ERR_CODE's bit order ([0] op_err, [1] mp_err,
// [2] rd_err, [3] wr_err), every port prefixed cp_ for edagen's signal-level agent. The read and
// write engines' RRAM ports, which go to rram_ctrl_mp and rram_phy in rram_ctrl, are separate here
// (cp_rd_*, cp_wr_*); the bench plays memory protection and the PHY behind them. No logic of its
// own beyond the FIFO and the wiring.
`include "prim_assert.sv"

module rram_ctrl_rdwr_unit
  import rram_ctrl_pkg::*;
(
  input  logic         clk_i,
  input  logic         rst_ni,
  // disables: rram_disable[ArbFsmDisableIdx], rram_sw_disable[SwDisArbIdx] (RMA entry)
  input  logic [3:0]   cp_rram_dis_i,
  input  logic [3:0]   cp_sw_dis_i,
  input  logic         cp_phy_init_done_i,
  input  logic         cp_ctrl_init_done_i,   // lcmgr init_done & keys_valid
  // software: CONTROL / ADDR, OP_STATUS.done / ERR_CODE, the write and read FIFO windows
  input  logic         cp_sw_start_i,
  input  logic [1:0]   cp_sw_op_i,
  input  logic         cp_sw_part_i,
  input  logic [9:0]   cp_sw_num_i,
  input  logic [20:0]  cp_sw_addr_i,
  output logic         cp_sw_done_o,
  output logic [3:0]   cp_sw_err_o,
  input  logic         cp_sw_wvalid_i,
  output logic         cp_sw_wready_o,
  input  logic [38:0]  cp_sw_wdata_i,
  input  logic         cp_sw_rready_i,
  output logic         cp_sw_rvalid_o,
  output logic [38:0]  cp_sw_rdata_o,
  // life-cycle manager plug
  input  logic         cp_lc_req_i,
  input  logic         cp_lc_start_i,
  input  logic [1:0]   cp_lc_op_i,
  input  logic         cp_lc_part_i,
  input  logic [9:0]   cp_lc_num_i,
  input  logic [1:0]   cp_lc_phase_i,
  input  logic [20:0]  cp_lc_addr_i,
  output logic         cp_lc_done_o,
  output logic [3:0]   cp_lc_err_o,
  input  logic         cp_lc_wvalid_i,
  input  logic [38:0]  cp_lc_wdata_i,
  output logic         cp_lc_wready_o,
  input  logic         cp_lc_rready_i,
  output logic [38:0]  cp_lc_rdata_o,
  output logic         cp_lc_rvalid_o,
  // OTP plug (its read data comes from rram_phy directly in rram_ctrl: only rvalid here)
  input  logic         cp_ot_req_i,
  input  logic         cp_ot_start_i,
  input  logic [1:0]   cp_ot_op_i,
  input  logic         cp_ot_part_i,
  input  logic [9:0]   cp_ot_num_i,
  input  logic [20:0]  cp_ot_addr_i,
  output logic         cp_ot_done_o,
  output logic [3:0]   cp_ot_err_o,
  input  logic         cp_ot_wvalid_i,
  input  logic [38:0]  cp_ot_wdata_i,
  output logic         cp_ot_wready_o,
  input  logic         cp_ot_rready_i,
  output logic         cp_ot_rvalid_o,
  // to memory protection: requester, phase, partition; ERR_ADDR; FIFO_CLR.wr; the write FIFO
  output logic [2:0]   cp_if_sel_o,
  output logic [1:0]   cp_phase_o,
  output logic         cp_part_o,
  output logic [18:0]  cp_err_addr_o,
  input  logic         cp_fifo_clr_i,
  output logic [4:0]   cp_fifo_depth_o,
  // read engine <-> memory protection / PHY
  output logic         cp_rd_req_o,
  output logic [18:0]  cp_rd_addr_o,
  output logic         cp_rd_ovfl_o,
  input  logic [38:0]  cp_rd_data_i,
  input  logic         cp_rd_done_i,
  input  logic         cp_rd_err_i,
  input  logic         cp_rd_mp_err_i,
  // write engine <-> memory protection / PHY
  output logic         cp_wr_req_o,
  output logic [18:0]  cp_wr_addr_o,
  output logic         cp_wr_ovfl_o,
  output logic [38:0]  cp_wr_data_o,
  output logic         cp_wr_last_o,
  input  logic         cp_wr_done_i,
  input  logic         cp_wr_mp_err_i,
  input  logic         cp_wr_intg_err_i,
  // faults: STD_FAULT_STATUS ctrl_fsm_err, ctrl_cnt_err, ctrl_fifo_err
  output logic         cp_fsm_err_o,
  output logic         cp_cnt_err_o,
  output logic         cp_fifo_err_o
);

  rram_ctrl_reg_pkg::rram_ctrl_reg2hw_control_reg_t sw_ctrl, lc_ctrl, ot_ctrl;
  assign sw_ctrl = '{num: '{q: cp_sw_num_i}, partition: '{q: cp_sw_part_i}, op: '{q: cp_sw_op_i},
                     start: '{q: cp_sw_start_i}};
  assign lc_ctrl = '{num: '{q: cp_lc_num_i}, partition: '{q: cp_lc_part_i}, op: '{q: cp_lc_op_i},
                     start: '{q: cp_lc_start_i}};
  assign ot_ctrl = '{num: '{q: cp_ot_num_i}, partition: '{q: cp_ot_part_i}, op: '{q: cp_ot_op_i},
                     start: '{q: cp_ot_start_i}};

  function automatic logic [3:0] err_code(rram_ctrl_err_t e);
    return {e.wr_err, e.rd_err, e.mp_err, e.invalid_op_err};
  endfunction

  rram_ctrl_err_t sw_err, lc_err, ot_err, wr_op_err, rd_op_err;
  rram_part_e ctrl_part;
  logic [CtrlMaxWordsW-1:0] ctrl_num_words;
  logic wr_op_start, rd_op_start, wr_op_done, rd_op_done;
  logic [BusAddrW-1:0] wr_op_addr, rd_op_addr, wr_op_err_addr, rd_op_err_addr;
  logic [BusFullWidth-1:0] rd_ctrl_data;
  logic rd_ctrl_valid, rd_ctrl_ready;
  logic wr_fifo_wvalid, wr_fifo_wready, wr_fifo_rvalid, wr_fifo_rready, wr_fifo_clr;
  logic [BusFullWidth-1:0] wr_fifo_wdata, wr_fifo_rdata;
  rram_sel_e if_sel;
  rram_phase_e phase;
  logic arb_fsm_err, wr_fsm_err, rd_fsm_err, wr_cnt_err, rd_cnt_err;

  rram_ctrl_arb u_rram_ctrl_arb (
    .clk_i,
    .rst_ni,
    .disable_i        (prim_mubi_pkg::mubi4_t'(cp_rram_dis_i)),
    .sw_disable_i     (prim_mubi_pkg::mubi4_t'(cp_sw_dis_i)),
    .sw_ctrl_i        (sw_ctrl),
    .sw_addr_i        (cp_sw_addr_i),
    .sw_done_o        (cp_sw_done_o),
    .sw_err_o         (sw_err),
    .sw_wvalid_i      (cp_sw_wvalid_i),
    .sw_wready_o      (cp_sw_wready_o),
    .sw_wdata_i       (cp_sw_wdata_i),
    .sw_rready_i      (cp_sw_rready_i),
    .sw_rvalid_o      (cp_sw_rvalid_o),
    .sw_rdata_o       (cp_sw_rdata_o),
    .hw_lcmgr_req_i   (cp_lc_req_i),
    .hw_lcmgr_ctrl_i  (lc_ctrl),
    .hw_lcmgr_phase_i (rram_phase_e'(cp_lc_phase_i)),
    .hw_lcmgr_addr_i  (cp_lc_addr_i),
    .hw_lcmgr_done_o  (cp_lc_done_o),
    .hw_lcmgr_err_o   (lc_err),
    .hw_lcmgr_wvalid_i(cp_lc_wvalid_i),
    .hw_lcmgr_wdata_i (cp_lc_wdata_i),
    .hw_lcmgr_wready_o(cp_lc_wready_o),
    .hw_lcmgr_rready_i(cp_lc_rready_i),
    .hw_lcmgr_rdata_o (cp_lc_rdata_o),
    .hw_lcmgr_rvalid_o(cp_lc_rvalid_o),
    .hw_otp_req_i     (cp_ot_req_i),
    .hw_otp_ctrl_i    (ot_ctrl),
    .hw_otp_addr_i    (cp_ot_addr_i),
    .hw_otp_done_o    (cp_ot_done_o),
    .hw_otp_err_o     (ot_err),
    .hw_otp_wvalid_i  (cp_ot_wvalid_i),
    .hw_otp_wdata_i   (cp_ot_wdata_i),
    .hw_otp_wready_o  (cp_ot_wready_o),
    .hw_otp_rready_i  (cp_ot_rready_i),
    .hw_otp_rvalid_o  (cp_ot_rvalid_o),
    .ctrl_part_o      (ctrl_part),
    .ctrl_num_words_o (ctrl_num_words),
    .wr_op_start_o    (wr_op_start),
    .wr_op_addr_o     (wr_op_addr),
    .rd_op_start_o    (rd_op_start),
    .rd_op_addr_o     (rd_op_addr),
    .wr_done_i        (wr_op_done),
    .wr_err_i         (wr_op_err),
    .wr_err_addr_i    (wr_op_err_addr),
    .rd_done_i        (rd_op_done),
    .rd_err_i         (rd_op_err),
    .rd_err_addr_i    (rd_op_err_addr),
    .rd_ctrl_rdata_i  (rd_ctrl_data),
    .rd_ctrl_rvalid_i (rd_ctrl_valid),
    .rd_ctrl_rready_o (rd_ctrl_ready),
    .wr_fifo_wvalid_o (wr_fifo_wvalid),
    .wr_fifo_wdata_o  (wr_fifo_wdata),
    .wr_fifo_wready_i (wr_fifo_wready),
    .wr_fifo_clr_o    (wr_fifo_clr),
    .ctrl_init_done_i (cp_ctrl_init_done_i),
    .phy_init_done_i  (cp_phy_init_done_i),
    .if_sel_o         (if_sel),
    .phase_o          (phase),
    .ctrl_err_addr_o  (cp_err_addr_o),
    .fsm_err_o        (arb_fsm_err)
  );

  prim_fifo_sync #(
    .Width(BusFullWidth),
    .Depth(MaxFifoDepth),
    .Secure(1'b1)
  ) u_wr_fifo (
    .clk_i,
    .rst_ni,
    .clr_i   (cp_fifo_clr_i | wr_fifo_clr),
    .wvalid_i(wr_fifo_wvalid),
    .wready_o(wr_fifo_wready),
    .wdata_i (wr_fifo_wdata),
    .depth_o (cp_fifo_depth_o),
    .full_o  (),
    .rvalid_o(wr_fifo_rvalid),
    .rready_i(wr_fifo_rready),
    .rdata_o (wr_fifo_rdata),
    .err_o   (cp_fifo_err_o)
  );

  rram_ctrl_wr u_rram_ctrl_wr (
    .clk_i,
    .rst_ni,
    .op_start_i        (wr_op_start),
    .op_num_words_i    (ctrl_num_words),
    .op_done_o         (wr_op_done),
    .op_addr_i         (wr_op_addr),
    .op_err_o          (wr_op_err),
    .op_err_addr_o     (wr_op_err_addr),
    .cnt_err_o         (wr_cnt_err),
    .fsm_err_o         (wr_fsm_err),
    .fifo_rvalid_i     (wr_fifo_rvalid),
    .fifo_rdata_i      (wr_fifo_rdata),
    .fifo_rready_o     (wr_fifo_rready),
    .rram_req_o        (cp_wr_req_o),
    .rram_addr_o       (cp_wr_addr_o),
    .rram_ovfl_o       (cp_wr_ovfl_o),
    .rram_data_o       (cp_wr_data_o),
    .rram_last_o       (cp_wr_last_o),
    .rram_done_i       (cp_wr_done_i),
    .rram_mp_err_i     (cp_wr_mp_err_i),
    .rram_wr_intg_err_i(cp_wr_intg_err_i)
  );

  rram_ctrl_rd u_rram_ctrl_rd (
    .clk_i,
    .rst_ni,
    .op_start_i     (rd_op_start),
    .op_num_words_i (ctrl_num_words),
    .op_done_o      (rd_op_done),
    .op_addr_i      (rd_op_addr),
    .op_err_o       (rd_op_err),
    .op_err_addr_o  (rd_op_err_addr),
    .cnt_err_o      (rd_cnt_err),
    .fsm_err_o      (rd_fsm_err),
    .rd_ctrl_data_o (rd_ctrl_data),
    .rd_ctrl_valid_o(rd_ctrl_valid),
    .rd_ctrl_ready_i(rd_ctrl_ready),
    .rram_req_o     (cp_rd_req_o),
    .rram_addr_o    (cp_rd_addr_o),
    .rram_ovfl_o    (cp_rd_ovfl_o),
    .rram_data_i    (cp_rd_data_i),
    .rram_done_i    (cp_rd_done_i),
    .rram_rd_err_i  (cp_rd_err_i),
    .rram_mp_err_i  (cp_rd_mp_err_i)
  );

  assign cp_sw_err_o  = err_code(sw_err);
  assign cp_lc_err_o  = err_code(lc_err);
  assign cp_ot_err_o  = err_code(ot_err);
  assign cp_if_sel_o  = if_sel;
  assign cp_phase_o   = phase;
  assign cp_part_o    = ctrl_part;
  assign cp_fsm_err_o = arb_fsm_err | wr_fsm_err | rd_fsm_err;
  assign cp_cnt_err_o = wr_cnt_err | rd_cnt_err;

  // The security countermeasures' error outputs reach the unit's fault outputs (rram_ctrl turns
  // them into STD_FAULT_STATUS ctrl_fsm_err / ctrl_cnt_err / ctrl_fifo_err and its fatal alert;
  // here the alert is not there, so the checks end at the unit's outputs).
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ERR(ArbFsmCheck_A, u_rram_ctrl_arb.u_state_regs, cp_fsm_err_o)
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ERR(RdFsmCheck_A, u_rram_ctrl_rd.u_state_regs, cp_fsm_err_o)
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ERR(WrFsmCheck_A, u_rram_ctrl_wr.u_state_regs, cp_fsm_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(RdCntCheck_A, u_rram_ctrl_rd.u_cnt, cp_cnt_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(WrCntCheck_A, u_rram_ctrl_wr.u_cnt, cp_cnt_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(WrFifoWptr_A,
    u_wr_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_wptr, cp_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(WrFifoRptr_A,
    u_wr_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_rptr, cp_fifo_err_o)

endmodule
