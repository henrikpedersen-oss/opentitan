// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The memory-protection unit of rram_ctrl as rram_ctrl.sv connects it: rram_ctrl_region_cfg
// turns the registers and the life-cycle enables into the rule lists, rram_ctrl_mp decides. Bench
// wrapper only (edagen rram_ctrl_mp.yaml): it flattens the struct ports into vectors laid out as
// the registers are (registers.md), every port prefixed mp_ for edagen's signal-level agent. No
// logic of its own.
module rram_ctrl_mp_unit
  import rram_ctrl_pkg::*;
(
  input  logic         clk_i,
  input  logic         rst_ni,
  // Life-cycle enables (lc_tx_t): creator seed, owner seed, isolated partition write and read.
  input  logic [3:0]   mp_lc_creator_i,
  input  logic [3:0]   mp_lc_owner_i,
  input  logic [3:0]   mp_lc_iso_wr_i,
  input  logic [3:0]   mp_lc_iso_rd_i,
  // MP_REGION_0..9 (BASE 11:0, SIZE 23:12), MP_REGION_CFG_0..9 (EN, RD_EN, WR_EN, SCRAMBLE_EN,
  // ECC_EN, 4 bits each from bit 0), DEFAULT_REGION (RD_EN 3:0 .. ECC_EN 15:12), INFO_PAGE_CFG_0..7
  // (as MP_REGION_CFG), EMUL_INFO_REGWEN_0..1, EMUL_INFO_REGION_0..1 (BASE, SIZE, EN 27:24),
  // EMUL_INFO_SUBREGION_REGWEN_0..23, EMUL_INFO_SUBREGION_0..23 (TOP 7:0),
  // EMUL_INFO_SUBREGION_CFG_0..23 (RD_EN .. ECC_EN in 15:0: the register's 19:4).
  input  logic [239:0] mp_region_i,
  input  logic [199:0] mp_region_cfg_i,
  input  logic [15:0]  mp_default_cfg_i,
  input  logic [159:0] mp_info_cfg_i,
  input  logic [1:0]   mp_emul_regwen_i,
  input  logic [55:0]  mp_emul_region_i,
  input  logic [23:0]  mp_sub_regwen_i,
  input  logic [191:0] mp_sub_top_i,
  input  logic [383:0] mp_sub_cfg_i,
  // rram_ctrl_mp's own inputs.
  input  logic [3:0]   mp_rram_disable_i,
  input  logic [3:0]   mp_sw_disable_i,
  input  logic [2:0]   mp_if_sel_i,
  input  logic [3:0]   mp_hw_info_scr_dis_i,
  input  logic [3:0]   mp_hw_info_ecc_dis_i,
  input  logic [1:0]   mp_phase_i,
  input  logic         mp_part_i,
  input  logic         mp_rd_req_i,
  input  logic [18:0]  mp_rd_addr_i,
  input  logic         mp_rd_ovfl_i,
  output logic         mp_rd_done_o,
  input  logic         mp_wr_req_i,
  input  logic [18:0]  mp_wr_addr_i,
  input  logic         mp_wr_ovfl_i,
  output logic         mp_wr_done_o,
  output logic         mp_mp_err_o,
  input  logic         mp_host_req_i,
  output logic         mp_host_gnt_o,
  input  logic [18:0]  mp_host_addr_i,
  output logic         mp_host_rd_err_o,
  output logic         mp_host_rd_done_o,
  output logic [38:0]  mp_host_rd_data_o,
  output logic         mp_ctrl_req_o,
  output logic [18:0]  mp_ctrl_addr_o,
  output logic         mp_ctrl_rd_o,
  output logic         mp_ctrl_wr_o,
  output logic         mp_ctrl_scr_o,
  output logic         mp_ctrl_ecc_o,
  output logic         mp_ctrl_xor_o,
  input  logic         mp_phy_rd_done_i,
  input  logic         mp_phy_wr_done_i,
  output logic         mp_host_fwd_o,
  input  logic         mp_phy_host_gnt_i,
  output logic         mp_host_scr_o,
  output logic         mp_host_ecc_o,
  input  logic         mp_phy_host_rd_err_i,
  input  logic         mp_phy_host_rd_done_i,
  input  logic [38:0]  mp_phy_host_rd_data_i
);

  sw_region_t [MpRegions-1:0]                          region;
  sw_region_cfg_t [MpRegions-1:0]                      region_cfg;
  sw_default_cfg_t                                     default_cfg;
  sw_info_cfg_t [TotalInfoPages-1:0]                   info_cfg;
  sw_emul_info_regwen_t [EmulInfoRegions-1:0]          emul_regwen;
  sw_emul_info_region_t [EmulInfoRegions-1:0]          emul_region;
  sw_emul_info_subregion_regwen_t [EmulInfoSubregions-1:0] sub_regwen;
  sw_emul_info_subregion_t [EmulInfoSubregions-1:0]    sub_top;
  sw_emul_info_subregion_cfg_t [EmulInfoSubregions-1:0] sub_cfg;
  assign region      = mp_region_i;
  assign region_cfg  = mp_region_cfg_i;
  assign default_cfg = mp_default_cfg_i;
  assign info_cfg    = mp_info_cfg_i;
  assign emul_regwen = mp_emul_regwen_i;
  assign emul_region = mp_emul_region_i;
  assign sub_regwen  = mp_sub_regwen_i;
  assign sub_top     = mp_sub_top_i;
  assign sub_cfg     = mp_sub_cfg_i;

  mp_region_cfg_t region_cfgs[TotalMpRegions];
  mp_region_cfg_t host_region_cfgs[HostMpRegions];
  mp_info_cfg_t   info_page_cfgs[TotalInfoPages];

  rram_ctrl_region_cfg u_region_cfg (
    .clk_i,
    .rst_ni,
    .lc_creator_seed_sw_rw_en_i  (lc_ctrl_pkg::lc_tx_t'(mp_lc_creator_i)),
    .lc_owner_seed_sw_rw_en_i    (lc_ctrl_pkg::lc_tx_t'(mp_lc_owner_i)),
    .lc_iso_part_sw_wr_en_i      (lc_ctrl_pkg::lc_tx_t'(mp_lc_iso_wr_i)),
    .lc_iso_part_sw_rd_en_i      (lc_ctrl_pkg::lc_tx_t'(mp_lc_iso_rd_i)),
    .region_i                    (region),
    .region_cfg_i                (region_cfg),
    .default_cfg_i               (default_cfg),
    .info_page_cfg_i             (info_cfg),
    .emul_info_regwen_i          (emul_regwen),
    .emul_info_region_i          (emul_region),
    .emul_info_subregion_regwen_i(sub_regwen),
    .emul_info_subregion_i       (sub_top),
    .emul_info_subregion_cfg_i   (sub_cfg),
    .region_cfgs_o               (region_cfgs),
    .host_region_cfgs_o          (host_region_cfgs),
    .info_page_cfgs_o            (info_page_cfgs)
  );

  rram_ctrl_mp u_mp (
    .clk_i,
    .rst_ni,
    .rram_disable_i     (prim_mubi_pkg::mubi4_t'(mp_rram_disable_i)),
    .sw_disable_i       (prim_mubi_pkg::mubi4_t'(mp_sw_disable_i)),
    .if_sel_i           (rram_sel_e'(mp_if_sel_i)),
    .region_cfgs_i      (region_cfgs),
    .host_region_cfgs_i (host_region_cfgs),
    .info_page_cfgs_i   (info_page_cfgs),
    .hw_info_scr_dis_i  (prim_mubi_pkg::mubi4_t'(mp_hw_info_scr_dis_i)),
    .hw_info_ecc_dis_i  (prim_mubi_pkg::mubi4_t'(mp_hw_info_ecc_dis_i)),
    .ctrl_phase_i       (rram_phase_e'(mp_phase_i)),
    .ctrl_part_i        (rram_part_e'(mp_part_i)),
    .ctrl_rd_req_i      (mp_rd_req_i),
    .ctrl_rd_addr_i     (mp_rd_addr_i),
    .ctrl_rd_addr_ovfl_i(mp_rd_ovfl_i),
    .ctrl_rd_done_o     (mp_rd_done_o),
    .ctrl_wr_req_i      (mp_wr_req_i),
    .ctrl_wr_addr_i     (mp_wr_addr_i),
    .ctrl_wr_addr_ovfl_i(mp_wr_ovfl_i),
    .ctrl_wr_done_o     (mp_wr_done_o),
    .ctrl_mp_err_o      (mp_mp_err_o),
    .host_req_i         (mp_host_req_i),
    .host_gnt_o         (mp_host_gnt_o),
    .host_addr_i        (mp_host_addr_i),
    .host_rd_err_o      (mp_host_rd_err_o),
    .host_rd_done_o     (mp_host_rd_done_o),
    .host_rd_data_o     (mp_host_rd_data_o),
    .ctrl_req_o         (mp_ctrl_req_o),
    .ctrl_addr_o        (mp_ctrl_addr_o),
    .ctrl_rd_o          (mp_ctrl_rd_o),
    .ctrl_wr_o          (mp_ctrl_wr_o),
    .ctrl_scramble_en_o (mp_ctrl_scr_o),
    .ctrl_ecc_en_o      (mp_ctrl_ecc_o),
    .ctrl_addr_xor_en_o (mp_ctrl_xor_o),
    .ctrl_rd_done_i     (mp_phy_rd_done_i),
    .ctrl_wr_done_i     (mp_phy_wr_done_i),
    .host_req_o         (mp_host_fwd_o),
    .host_gnt_i         (mp_phy_host_gnt_i),
    .host_scramble_en_o (mp_host_scr_o),
    .host_ecc_en_o      (mp_host_ecc_o),
    .host_rd_err_i      (mp_phy_host_rd_err_i),
    .host_rd_done_i     (mp_phy_host_rd_done_i),
    .host_rd_data_i     (mp_phy_host_rd_data_i)
  );

endmodule
