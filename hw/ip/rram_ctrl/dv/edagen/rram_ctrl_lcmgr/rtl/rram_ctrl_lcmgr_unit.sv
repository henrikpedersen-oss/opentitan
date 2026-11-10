// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_ctrl_lcmgr, the life-cycle manager hardware plug, for its unit bench (edagen
// rram_ctrl_lcmgr.yaml). Bench wrapper only, no logic of its own: it flattens the struct ports into
// vectors for edagen's signal-level agents, one per clock domain:
//   ok_*  the OTP key interface (otp_ctrl_pkg::nvm_otp_key_req_t / _rsp_t), clk_otp_i;
//   ct_*  everything on clk_i: INIT.VAL, the life-cycle inputs (lc_seed_hw_rd_en, rma_req, rma_seed)
//         and rma_ack, rram_disable, the controller path towards rram_ctrl_arb / rram_ctrl_rd /
//         rram_ctrl_wr (CONTROL fields, request, address, phase, done, error, the read and write bus
//         words with their TL-UL integrity), the seeds to the key manager, the scrambling keys, the
//         RMA software / access disables, the status and error outputs.
`include "prim_assert.sv"

module rram_ctrl_lcmgr_unit
  import rram_ctrl_pkg::*;
(
  input  logic          clk_i,
  input  logic          rst_ni,
  input  logic          clk_otp_i,
  input  logic          rst_otp_ni,
  // OTP key interface (clk_otp_i)
  output logic          ok_addr_req_o,
  output logic          ok_data_req_o,
  input  logic          ok_addr_ack_i,
  input  logic          ok_data_ack_i,
  input  logic [127:0]  ok_key_i,
  input  logic [127:0]  ok_rand_key_i,
  input  logic          ok_seed_valid_i,
  // INIT.VAL, life cycle, disable (clk_i)
  input  logic          ct_init_i,
  input  logic [3:0]    ct_seed_hw_rd_en_i,  // lc_tx_t
  input  logic [3:0]    ct_rma_req_i,        // lc_tx_t
  input  logic [63:0]   ct_rma_seed_i,
  output logic [3:0]    ct_rma_ack_o,        // lc_tx_t
  input  logic [3:0]    ct_rram_dis_i,       // mubi4_t (ct_disable_i would be the SV keyword)
  // controller path
  output logic          ct_start_o,
  output logic [1:0]    ct_op_o,
  output logic          ct_part_o,
  output logic [9:0]    ct_num_o,
  output logic          ct_req_o,
  output logic [20:0]   ct_addr_o,
  output logic [1:0]    ct_phase_o,          // rram_phase_e
  input  logic          ct_done_i,
  input  logic [3:0]    ct_err_i,            // {invalid_op_err, mp_err, rd_err, wr_err}
  output logic          ct_rready_o,
  input  logic          ct_rvalid_i,
  input  logic [38:0]   ct_rdata_i,          // {integrity[6:0], data[31:0]}
  output logic          ct_wvalid_o,
  input  logic          ct_wready_i,
  output logic [38:0]   ct_wdata_o,
  // seeds, keys, disables, status
  output logic [255:0]  ct_creator_seed_o,
  output logic [255:0]  ct_owner_seed_o,
  output logic [1:0]    ct_seeds_valid_o,    // {owner, creator}
  output logic [127:0]  ct_addr_key_o,
  output logic [127:0]  ct_data_key_o,
  output logic [127:0]  ct_rand_addr_key_o,
  output logic [127:0]  ct_rand_data_key_o,
  output logic [3:0]    ct_rma_dis_access_o, // lc_tx_t
  output logic [3:0]    ct_rma_sw_dis_o,     // lc_tx_t
  output logic          ct_keys_valid_o,
  output logic          ct_init_done_o,
  output logic          ct_fatal_err_o,
  output logic          ct_intg_err_o,
  output logic          ct_seed_err_o
);

  rram_ctrl_reg_pkg::rram_ctrl_reg2hw_control_reg_t ctrl;
  otp_ctrl_pkg::nvm_otp_key_req_t key_req;
  otp_ctrl_pkg::nvm_otp_key_rsp_t key_rsp;
  logic [NumSeeds-1:0][SeedWidth-1:0] seeds;
  rram_phase_e phase;
  lc_ctrl_pkg::lc_tx_t rma_ack, rma_dis_access, rma_sw_dis;

  assign key_rsp = '{data_ack:   ok_data_ack_i,
                     addr_ack:   ok_addr_ack_i,
                     key:        ok_key_i,
                     rand_key:   ok_rand_key_i,
                     seed_valid: ok_seed_valid_i};

  rram_ctrl_lcmgr u_lcmgr (
    .clk_i,
    .rst_ni,
    .clk_otp_i,
    .rst_otp_ni,
    .lc_seed_hw_rd_en_i(lc_ctrl_pkg::lc_tx_t'(ct_seed_hw_rd_en_i)),
    .lcmgr_init_i      (ct_init_i),
    .disable_i         (prim_mubi_pkg::mubi4_t'(ct_rram_dis_i)),
    .ctrl_o            (ctrl),
    .req_o             (ct_req_o),
    .addr_o            (ct_addr_o),
    .done_i            (ct_done_i),
    .err_i             (rram_ctrl_err_t'(ct_err_i)),
    .phase_o           (phase),
    .rready_o          (ct_rready_o),
    .rvalid_i          (ct_rvalid_i),
    .rdata_i           (ct_rdata_i),
    .wvalid_o          (ct_wvalid_o),
    .wready_i          (ct_wready_i),
    .wdata_o           (ct_wdata_o),
    .rma_req_i         (lc_ctrl_pkg::lc_tx_t'(ct_rma_req_i)),
    .rma_seed_i        (ct_rma_seed_i),
    .rma_ack_o         (rma_ack),
    .seeds_o           (seeds),
    .seeds_valid_o     (ct_seeds_valid_o),
    .fatal_err_o       (ct_fatal_err_o),
    .intg_err_o        (ct_intg_err_o),
    .seed_err_o        (ct_seed_err_o),
    .otp_key_req_o     (key_req),
    .otp_key_rsp_i     (key_rsp),
    .addr_key_o        (ct_addr_key_o),
    .data_key_o        (ct_data_key_o),
    .rand_addr_key_o   (ct_rand_addr_key_o),
    .rand_data_key_o   (ct_rand_data_key_o),
    .rma_dis_access_o  (rma_dis_access),
    .rma_sw_dis_o      (rma_sw_dis),
    .keys_valid_o      (ct_keys_valid_o),
    .init_done_o       (ct_init_done_o)
  );

  assign ok_addr_req_o       = key_req.addr_req;
  assign ok_data_req_o       = key_req.data_req;
  assign ct_start_o          = ctrl.start.q;
  assign ct_op_o             = ctrl.op.q;
  assign ct_part_o           = ctrl.partition.q;
  assign ct_num_o            = ctrl.num.q;
  assign ct_phase_o          = phase;
  assign ct_creator_seed_o   = seeds[0];
  assign ct_owner_seed_o     = seeds[1];
  assign ct_rma_ack_o        = rma_ack;
  assign ct_rma_dis_access_o = rma_dis_access;
  assign ct_rma_sw_dis_o     = rma_sw_dis;

  // The unit's sparse FSMs and redundant counters report their errors as fatal_err_o (the checks the
  // primitives ask their parent for; rram_ctrl.sv does the same for the whole block).
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ALERT_IN(LcmgrFsmCheck_A, u_lcmgr.u_state_regs, ct_fatal_err_o)
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ALERT_IN(LcmgrRmaFsmCheck_A, u_lcmgr.u_rma_state_regs,
                                          ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(LcmgrSeedCntCheck_A, u_lcmgr.u_seed_cnt, ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(LcmgrAddrCntCheck_A, u_lcmgr.u_addr_cnt, ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(LcmgrPageCntCheck_A, u_lcmgr.u_page_cnt, ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(LcmgrWordCntCheck_A, u_lcmgr.u_word_cnt, ct_fatal_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ALERT_IN(LcmgrWipeIdxCntCheck_A, u_lcmgr.u_wipe_idx_cnt,
                                            ct_fatal_err_o)

`ifndef SYNTHESIS
  // Simulation only, +lcmgr_trace: the two FSMs when they change.
  bit trace_on;
  initial trace_on = $test$plusargs("lcmgr_trace");
  always @(posedge clk_i) begin
    if (trace_on && rst_ni && (u_lcmgr.state_d != u_lcmgr.state_q ||
                               u_lcmgr.rma_state_d != u_lcmgr.rma_state_q))
      $display("%0t lcmgr_trace state %s -> %s rma %s -> %s", $time, u_lcmgr.state_q.name(),
               u_lcmgr.state_d.name(), u_lcmgr.rma_state_q.name(), u_lcmgr.rma_state_d.name());
  end
`endif

endmodule
