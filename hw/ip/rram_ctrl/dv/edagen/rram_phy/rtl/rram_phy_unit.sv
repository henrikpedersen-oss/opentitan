// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_phy with its default parameters (the package's reset keys, SecScrambleEn). Bench wrapper
// only (edagen rram_phy.yaml): it flattens the RRAM macro request and response structs into
// vectors (ph_m_*), the multibit disable into a vector, the partition enums into bits, every port
// prefixed ph_ for edagen's signal-level agent. No logic of its own.
`include "prim_assert.sv"

module rram_phy_unit
  import rram_ctrl_pkg::*;
(
  input  logic         clk_i,
  input  logic         rst_ni,
  input  logic [3:0]   ph_rram_disable_i,
  input  logic         ph_keys_valid_i,
  input  logic [127:0] ph_addr_key_i,
  input  logic [127:0] ph_data_key_i,
  input  logic [127:0] ph_rand_addr_key_i,
  input  logic [127:0] ph_rand_data_key_i,
  // controller port (rram_ctrl_rd / rram_ctrl_wr / the hardware plugs through rram_ctrl_arb)
  input  logic         ph_ctrl_req_i,
  input  logic         ph_ctrl_scramble_en_i,
  input  logic         ph_ctrl_ecc_en_i,
  input  logic         ph_ctrl_addr_xor_en_i,
  input  logic         ph_ctrl_rd_i,
  input  logic         ph_ctrl_wr_i,
  input  logic         ph_ctrl_part_i,       // rram_part_e: 0 data, 1 info
  input  logic [18:0]  ph_ctrl_addr_i,       // bus-word address (BusAddrW)
  input  logic [38:0]  ph_ctrl_wr_data_i,    // 32 data bits + 7 bus-integrity bits
  input  logic         ph_ctrl_wr_last_i,
  output logic         ph_ctrl_wr_done_o,
  output logic         ph_ctrl_rd_done_o,
  output logic [38:0]  ph_ctrl_rd_data_o,
  output logic         ph_ctrl_rd_err_o,
  // host port (host_tl through rram_ctrl's memory protection)
  input  logic         ph_host_req_i,
  output logic         ph_host_gnt_o,
  input  logic         ph_host_scramble_en_i,
  input  logic         ph_host_ecc_en_i,
  input  logic [18:0]  ph_host_addr_i,
  output logic         ph_host_rd_done_o,
  output logic [38:0]  ph_host_rd_data_o,
  output logic         ph_host_rd_err_o,
  // status and errors
  output logic         ph_wr_busy_o,
  output logic         ph_init_done_o,
  output logic         ph_ecc_corr1_err_o,
  output logic         ph_ecc_corr2_err_o,
  output logic [16:0]  ph_ecc_corr_addr_o,
  output logic         ph_ecc_corr_part_o,
  output logic         ph_host_gnt_err_o,
  output logic         ph_spurious_done_o,
  output logic         ph_fsm_err_o,
  output logic         ph_cnt_err_o,
  output logic         ph_wr_intg_err_o,
  output logic         ph_rd_intg_err_o,
  output logic         ph_rd_ctrl_err_o,
  output logic         ph_arb_err_o,
  output logic         ph_fifo_err_o,
  output logic         ph_ecc_fatal_err_o,
  // RRAM macro interface (rram_macro_req_t / rram_macro_rsp_t)
  output logic         ph_m_rd_req_o,
  output logic         ph_m_wr_req_o,
  output logic         ph_m_wr_last_o,
  output logic [16:0]  ph_m_addr_o,
  output logic [127:0] ph_m_wr_data_o,
  output logic         ph_m_part_o,
  output logic         ph_m_ecc_en_o,
  input  logic         ph_m_ack_i,
  input  logic         ph_m_done_i,
  input  logic         ph_m_err_i,
  input  logic [2:0]   ph_m_ecc_err_i,
  input  logic [127:0] ph_m_rd_data_i,
  input  logic         ph_m_init_done_i,
  input  logic         ph_m_fatal_err_i,
  input  logic         ph_m_recov_err_i
);

  rram_macro_req_t m_req;
  rram_macro_rsp_t m_rsp;
  rram_part_e      corr_part;

  assign m_rsp = '{ack:       ph_m_ack_i,
                   done:      ph_m_done_i,
                   err:       ph_m_err_i,
                   ecc_err:   ph_m_ecc_err_i,
                   rd_data:   ph_m_rd_data_i,
                   init_done: ph_m_init_done_i,
                   fatal_err: ph_m_fatal_err_i,
                   recov_err: ph_m_recov_err_i};

  rram_phy u_phy (
    .clk_i,
    .rst_ni,
    .rram_disable_i    (prim_mubi_pkg::mubi4_t'(ph_rram_disable_i)),
    .keys_valid_i      (ph_keys_valid_i),
    .addr_key_i        (ph_addr_key_i),
    .data_key_i        (ph_data_key_i),
    .rand_addr_key_i   (ph_rand_addr_key_i),
    .rand_data_key_i   (ph_rand_data_key_i),
    .ctrl_req_i        (ph_ctrl_req_i),
    .ctrl_scramble_en_i(ph_ctrl_scramble_en_i),
    .ctrl_ecc_en_i     (ph_ctrl_ecc_en_i),
    .ctrl_addr_xor_en_i(ph_ctrl_addr_xor_en_i),
    .ctrl_rd_i         (ph_ctrl_rd_i),
    .ctrl_wr_i         (ph_ctrl_wr_i),
    .ctrl_part_i       (rram_part_e'(ph_ctrl_part_i)),
    .ctrl_addr_i       (ph_ctrl_addr_i),
    .ctrl_wr_data_i    (ph_ctrl_wr_data_i),
    .ctrl_wr_last_i    (ph_ctrl_wr_last_i),
    .ctrl_wr_done_o    (ph_ctrl_wr_done_o),
    .ctrl_rd_done_o    (ph_ctrl_rd_done_o),
    .ctrl_rd_data_o    (ph_ctrl_rd_data_o),
    .ctrl_rd_err_o     (ph_ctrl_rd_err_o),
    .host_req_i        (ph_host_req_i),
    .host_gnt_o        (ph_host_gnt_o),
    .host_scramble_en_i(ph_host_scramble_en_i),
    .host_ecc_en_i     (ph_host_ecc_en_i),
    .host_addr_i       (ph_host_addr_i),
    .host_rd_done_o    (ph_host_rd_done_o),
    .host_rd_data_o    (ph_host_rd_data_o),
    .host_rd_err_o     (ph_host_rd_err_o),
    .phy_wr_busy_o     (ph_wr_busy_o),
    .phy_init_done_o   (ph_init_done_o),
    .ecc_corr1_err_o   (ph_ecc_corr1_err_o),
    .ecc_corr2_err_o   (ph_ecc_corr2_err_o),
    .ecc_corr_addr_o   (ph_ecc_corr_addr_o),
    .ecc_corr_part_o   (corr_part),
    .host_gnt_err_o    (ph_host_gnt_err_o),
    .spurious_done_o   (ph_spurious_done_o),
    .fsm_err_o         (ph_fsm_err_o),
    .cnt_err_o         (ph_cnt_err_o),
    .wr_intg_err_o     (ph_wr_intg_err_o),
    .rd_intg_err_o     (ph_rd_intg_err_o),
    .rd_ctrl_err_o     (ph_rd_ctrl_err_o),
    .arb_err_o         (ph_arb_err_o),
    .fifo_err_o        (ph_fifo_err_o),
    .ecc_fatal_err_o   (ph_ecc_fatal_err_o),
    .rram_macro_req_o  (m_req),
    .rram_macro_rsp_i  (m_rsp)
  );

  assign ph_ecc_corr_part_o = corr_part;
  assign ph_m_rd_req_o      = m_req.rd_req;
  assign ph_m_wr_req_o      = m_req.wr_req;
  assign ph_m_wr_last_o     = m_req.wr_last;
  assign ph_m_addr_o        = m_req.addr;
  assign ph_m_wr_data_o     = m_req.wr_data;
  assign ph_m_part_o        = m_req.part;
  assign ph_m_ecc_en_o      = m_req.ecc_en;

  // The security countermeasures' error outputs reach rram_phy's error outputs (rram_ctrl ties
  // them to its fatal alert, rram_ctrl.sv "rram_phy assertions"; here the alert is not there, so the
  // checks end at the PHY's outputs, which rram_ctrl turns into fault_status bits).
  `ASSERT_PRIM_FSM_ERROR_TRIGGER_ERR(PhyWrFsmCheck_A, u_phy.u_rram_phy_wr.u_state_regs, ph_fsm_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyWordCntCheck_A, u_phy.u_rram_phy_wr.u_word_cnt, ph_cnt_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyMaskFifoWptr_A,
    u_phy.u_rram_phy_rd.u_mask_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_wptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyMaskFifoRptr_A,
    u_phy.u_rram_phy_rd.u_mask_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_rptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyRdFifoWptr_A,
    u_phy.u_rram_phy_rd.u_rd_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_wptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyRdFifoRptr_A,
    u_phy.u_rram_phy_rd.u_rd_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_rptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyRdMetaFifoWptr_A,
    u_phy.u_rram_phy_rd.u_meta_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_wptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyRdMetaFifoRptr_A,
    u_phy.u_rram_phy_rd.u_meta_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_rptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyMetaFifoWptr_A,
    u_phy.u_meta_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_wptr, ph_fifo_err_o)
  `ASSERT_PRIM_COUNT_ERROR_TRIGGER_ERR(PhyMetaFifoRptr_A,
    u_phy.u_meta_fifo.gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.u_rptr, ph_fifo_err_o)

endmodule
