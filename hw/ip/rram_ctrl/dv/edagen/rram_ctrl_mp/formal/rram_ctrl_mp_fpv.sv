// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's memory-protection unit (edagen rram_ctrl_mp.yaml formal.files), bound into
// rram_ctrl_mp_unit. Memory protection as a decision function: ref_data() below states the
// documented data-partition decision (theory_of_operation.md "Memory Protection", registers.md;
// the same reading as the reference model eg2_rram_mp_core.h, written again here), and the
// properties ask that the unit forwards a software or host request exactly when it allows, for
// every configuration. Cases the documentation leaves open (an invalid EMUL_INFO_REGION.EN, an
// invalid SCRAMBLE_EN / ECC_EN, rma_sw_dis not a valid MuBi4 value) are excluded from the
// equivalences, not assumed away. Further properties: the OTP pages, the hardware plugs, the
// life-cycle gated info pages, one answer per controller request.
//
// Assumptions: the requesters' protocol as rram_ctrl drives the unit: never a read and a write
// request at once, and a requester other than lcmgr has PhaseInvalid (rram_ctrl_arb). Every
// assumption has a cover that it leaves the checked cases reachable.
module rram_ctrl_mp_fpv
  import rram_ctrl_pkg::*;
(
  input logic         clk_i,
  input logic         rst_ni,
  input logic [3:0]   mp_lc_creator_i,
  input logic [3:0]   mp_lc_owner_i,
  input logic [3:0]   mp_lc_iso_wr_i,
  input logic [3:0]   mp_lc_iso_rd_i,
  input logic [239:0] mp_region_i,
  input logic [199:0] mp_region_cfg_i,
  input logic [15:0]  mp_default_cfg_i,
  input logic [159:0] mp_info_cfg_i,
  input logic [1:0]   mp_emul_regwen_i,
  input logic [55:0]  mp_emul_region_i,
  input logic [23:0]  mp_sub_regwen_i,
  input logic [191:0] mp_sub_top_i,
  input logic [383:0] mp_sub_cfg_i,
  input logic [3:0]   mp_rram_disable_i,
  input logic [3:0]   mp_sw_disable_i,
  input logic [2:0]   mp_if_sel_i,
  input logic [1:0]   mp_phase_i,
  input logic         mp_part_i,
  input logic         mp_rd_req_i,
  input logic [18:0]  mp_rd_addr_i,
  input logic         mp_rd_ovfl_i,
  input logic         mp_wr_req_i,
  input logic [18:0]  mp_wr_addr_i,
  input logic         mp_wr_ovfl_i,
  input logic         mp_mp_err_o,
  input logic         mp_host_req_i,
  input logic [18:0]  mp_host_addr_i,
  input logic         mp_ctrl_req_o,
  input logic         mp_ctrl_scr_o,
  input logic         mp_ctrl_ecc_o,
  input logic         mp_ctrl_xor_o,
  input logic         mp_host_fwd_o,
  input logic         mp_host_gnt_o,
  input logic         mp_host_rd_done_o,
  input logic         mp_phy_host_gnt_i,
  input logic         mp_phy_host_rd_done_i
);
  localparam logic [3:0] T = 4'h6, F = 4'h9, LcOn = 4'h5;

  // ---- The documented decision for the data partition ----
  // result: [0] decided (no open case), [1] read allowed, [2] write allowed, [3] scramble,
  // [4] ECC, [5] scramble/ECC decided.
  function automatic logic [5:0] ref_data(logic [11:0] page, logic host);
    logic [12:0] b, e, wb, we;
    logic en_t, en_inv, chain, fits, ovl, ovl_inv;
    logic [3:0] wen[2];
    logic [12:0] wbase[2], wend[2];
    logic [19:0] c;
    if (page >= 12'd4090) return 6'b000001;  // SwInitDataCfg: no access, decided
    for (int w = 0; w < 2; w++) begin
      wen[w] = mp_emul_region_i[28*w+24 +: 4];
      wbase[w] = {1'b0, mp_emul_region_i[28*w +: 12]};
      wend[w] = wbase[w] + mp_emul_region_i[28*w+12 +: 12];
    end
    ovl = wen[0] == T && wen[1] == T && wbase[0] <= wend[1] && wbase[1] <= wend[0];
    // The overlap is open when a window it involves has an invalid EN.
    ovl_inv = (wen[0] != T && wen[0] != F) || (wen[1] != T && wen[1] != F);
    if (!host) begin
      for (int i = 0; i < 24; i++) begin
        int w = i / 12;
        chain = !mp_emul_regwen_i[w];
        for (int j = 12 * w; j <= i; j++) chain &= !mp_sub_regwen_i[j];
        fits = {4'b0, mp_sub_top_i[8*i +: 8]} <= mp_emul_region_i[28*w+12 +: 12];
        b = wbase[w];
        e = wbase[w] + mp_sub_top_i[8*i +: 8];
        if (chain && fits && page >= b && page <= e) begin
          if ((wen[w] != T && wen[w] != F) || (wen[w] == T && ovl_inv)) return 6'b000000;
          if (wen[w] == T && !ovl) begin
            c = {mp_sub_cfg_i[16*i +: 16], 4'b0};
            return {(c[15:12] inside {T, F}) && (c[19:16] inside {T, F}), c[19:16] == T,
                    c[15:12] == T, c[11:8] == T, c[7:4] == T, 1'b1};
          end
        end
      end
    end
    for (int w = 0; w < 2; w++) begin
      if (page >= wbase[w] && page <= wend[w]) begin
        if (wen[w] == T) return 6'b000001;
        if (wen[w] != F) return 6'b000000;
      end
    end
    for (int i = 0; i < 10; i++) begin
      c = mp_region_cfg_i[20*i +: 20];
      b = {1'b0, mp_region_i[24*i +: 12]};
      e = b + mp_region_i[24*i+12 +: 12];
      if (c[3:0] == T && page >= b && page <= e)
        return {(c[15:12] inside {T, F}) && (c[19:16] inside {T, F}), c[19:16] == T, c[15:12] == T,
                c[11:8] == T, c[7:4] == T, 1'b1};
    end
    c = {mp_default_cfg_i, 4'h6};
    return {(c[15:12] inside {T, F}) && (c[19:16] inside {T, F}), c[19:16] == T, c[15:12] == T,
            c[11:8] == T, c[7:4] == T, 1'b1};
  endfunction

  logic rd, wr, req, sw, data, dis_f;
  logic [18:0] addr;
  logic [11:0] page, hpage;
  logic [5:0] sw_ref, host_ref;
  assign rd = mp_rd_req_i;
  assign wr = mp_wr_req_i;
  assign req = rd | wr;
  assign addr = rd ? mp_rd_addr_i : mp_wr_addr_i;
  assign page = addr[18:7];
  assign hpage = mp_host_addr_i[18:7];
  assign sw = mp_if_sel_i == 3'd1;
  assign data = mp_part_i == 1'b0;
  assign dis_f = mp_rram_disable_i == F;
  assign sw_ref = ref_data(page, 1'b0);
  assign host_ref = ref_data(hpage, 1'b1);

  default clocking @(posedge clk_i); endclocking
  default disable iff (!rst_ni);

  // ---- Assumptions: the requesters' protocol ----
  OneOp_M: assume property (!(rd && wr));
  PhaseOnlyLcMgr_M: assume property (mp_if_sel_i != 3'd3 |-> mp_phase_i == 2'd3);
  OneOpReachable_C: cover property (rd ##1 wr);
  // The host requester: at most two reads outstanding (tlul_adapter_sram, Outstanding = 2), a read
  // held until granted. The PHY: grants while it has fewer than two reads, answers only a read it
  // accepted in an earlier cycle (theory_of_operation.md "RRAM Phy").
  logic [2:0] h_out, p_out;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      h_out <= '0;
      p_out <= '0;
    end else begin
      h_out <= h_out + (mp_host_req_i && mp_host_gnt_o) - mp_host_rd_done_o;
      p_out <= p_out + (mp_host_fwd_o && mp_phy_host_gnt_i) - mp_phy_host_rd_done_i;
    end
  end
  HostOutstanding_M: assume property (mp_host_req_i |-> h_out < 3'd2);
  HostHold_M: assume property (mp_host_req_i && !mp_host_gnt_o |=> mp_host_req_i && $stable(mp_host_addr_i));
  PhyGrant_M: assume property (mp_phy_host_gnt_i |-> p_out < 3'd2);
  // rram_phy grants nothing before its initialisation is done (phy_init_done_q, a flop reset to
  // 0): never in the first cycle after reset. Without this the PHY could take a read in the cycle
  // in which rram_ctrl_mp's error FIFO is still in reset (wready low) and the unit does not grant
  // it (2026-11-07: counterexamples to IllegalBackPressure_A, HostOutstandingBound_A and
  // HostNoAnswerWithoutRead_A; see the RTL ticket on host_req_o not waiting for the FIFO).
  logic after_first;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) after_first <= 1'b0;
    else after_first <= 1'b1;
  end
  PhyNoGrantFirstCycle_M: assume property (!after_first |-> !mp_phy_host_gnt_i);
  // "rram_phy observes the same rram_disable bus and suppresses all new host and controller
  // requests" (theory_of_operation.md "RRAM Escalation & Disable"). rram_ctrl_mp still forwards a
  // read it answers with an error while RRAM is disabled (counterexample to IllegalBackPressure_A
  // and HostNoAnswerWithoutRead_A without this assumption, 2026-11-07).
  PhyDisabled_M: assume property (mp_rram_disable_i != F |-> !mp_phy_host_gnt_i);
  PhyAnswer_M: assume property (mp_phy_host_rd_done_i |-> p_out > 3'd0);
  HostTwoOutstanding_C: cover property (h_out == 3'd2);
  PhyAnswerReachable_C: cover property (mp_phy_host_rd_done_i && mp_host_rd_done_o);
  PhaseReachable_C: cover property (mp_if_sel_i == 3'd3 && mp_phase_i == 2'd1 && req);

  // ---- The decision function ----
  // A software data request is forwarded exactly when the documented rules allow it, with their
  // scramble and ECC attributes and address XOR on.
  SwDataDecision_A: assert property (sw && data && req && dis_f && !(rd ? mp_rd_ovfl_i : mp_wr_ovfl_i) &&
                                     sw_ref[0] |-> mp_ctrl_req_o == (rd ? sw_ref[1] : sw_ref[2]));
  SwDataAttrs_A: assert property (sw && data && req && dis_f && sw_ref[0] && sw_ref[5] && mp_ctrl_req_o |->
                                  mp_ctrl_scr_o == sw_ref[3] && mp_ctrl_ecc_o == sw_ref[4] && mp_ctrl_xor_o);
  // A host read is forwarded exactly when the documented rules allow it (no subregions for the host).
  HostDecision_A: assert property (mp_host_req_i && dis_f && mp_sw_disable_i == F && host_ref[0] |->
                                   mp_host_fwd_o == host_ref[1]);
  SwDataAllowedReachable_C: cover property (sw && data && wr && dis_f && sw_ref[0] && sw_ref[2]);
  SwDataSubregionReachable_C: cover property (sw && data && rd && dis_f && mp_ctrl_req_o &&
                                              mp_emul_region_i[27:24] == T && !mp_emul_regwen_i[0] &&
                                              page >= mp_emul_region_i[11:0] &&
                                              page <= mp_emul_region_i[11:0] + mp_emul_region_i[23:12]);
  HostAllowedReachable_C: cover property (mp_host_req_i && mp_host_fwd_o);

  // ---- Properties that hold whatever the open cases ----
  // The OTP pages: software and the host never; the lcmgr never; the OTP plug only there, with
  // scrambling off, ECC on, address XOR off.
  SwNoOtp_A: assert property (sw && data && req && page >= 12'd4090 |-> !mp_ctrl_req_o);
  HostNoOtp_A: assert property (mp_host_req_i && hpage >= 12'd4090 |-> !mp_host_fwd_o);
  LcMgrNoOtp_A: assert property (mp_if_sel_i == 3'd3 && data && req && page >= 12'd4090 |-> !mp_ctrl_req_o);
  OtpPlugOnlyOtp_A: assert property (mp_if_sel_i == 3'd2 && req && (!data || page < 12'd4090) |-> !mp_ctrl_req_o);
  OtpPlugAttrs_A: assert property (mp_if_sel_i == 3'd2 && req && mp_ctrl_req_o |->
                                   !mp_ctrl_scr_o && mp_ctrl_ecc_o && !mp_ctrl_xor_o);
  OtpPlugAllowed_A: assert property (mp_if_sel_i == 3'd2 && req && data && page >= 12'd4090 && dis_f &&
                                     !(rd ? mp_rd_ovfl_i : mp_wr_ovfl_i) |-> mp_ctrl_req_o);
  // An enabled window is never read by the host.
  for (genvar w = 0; w < 2; w++) begin : gen_win
    HostNoWindow_A: assert property (mp_host_req_i && mp_emul_region_i[28*w+24 +: 4] == T &&
                                     hpage >= mp_emul_region_i[28*w +: 12] &&
                                     {1'b0, hpage} <= mp_emul_region_i[28*w +: 12] + mp_emul_region_i[28*w+12 +: 12]
                                     |-> !mp_host_fwd_o);
    // An enabled, unlocked window: all of it denied to software too (subregions need the lock).
    SwUnlockedWindow_A: assert property (sw && data && req && mp_emul_region_i[28*w+24 +: 4] == T &&
                                         mp_emul_regwen_i[w] && page >= mp_emul_region_i[28*w +: 12] &&
                                         {1'b0, page} <= mp_emul_region_i[28*w +: 12] + mp_emul_region_i[28*w+12 +: 12]
                                         |-> !mp_ctrl_req_o);
  end
  // rma_sw_dis: host reads are answered with an error, never forwarded.
  SwDisHost_A: assert property (mp_host_req_i && mp_sw_disable_i == T |-> !mp_host_fwd_o);
  // Info pages 5, 6, 7: with the life-cycle enable off (steady for three cycles), software never.
  CreatorLc_A: assert property ((mp_lc_creator_i != LcOn) [*3] ##0 (sw && !data && req && page == 12'd5)
                                |-> !mp_ctrl_req_o);
  OwnerLc_A: assert property ((mp_lc_owner_i != LcOn) [*3] ##0 (sw && !data && req && page == 12'd6)
                              |-> !mp_ctrl_req_o);
  IsoRdLc_A: assert property ((mp_lc_iso_rd_i != LcOn) [*3] ##0 (sw && !data && rd && page == 12'd7)
                              |-> !mp_ctrl_req_o);
  IsoWrLc_A: assert property ((mp_lc_iso_wr_i != LcOn) [*3] ##0 (sw && !data && wr && page == 12'd7)
                              |-> !mp_ctrl_req_o);
  CreatorLcOnReachable_C: cover property ((mp_lc_creator_i == LcOn) [*3] ##0 (sw && !data && rd && page == 12'd5 && mp_ctrl_req_o));
  // An info page that does not exist, an overflowed address: never.
  NoInfoPage8_A: assert property (!data && req && page >= 12'd8 |-> !mp_ctrl_req_o);
  Overflow_A: assert property (rd && mp_rd_ovfl_i || wr && mp_wr_ovfl_i |-> !mp_ctrl_req_o);
  // A requester the documentation does not list: never.
  NoRequester_A: assert property (req && !(mp_if_sel_i inside {[3'd1:3'd4]}) |-> !mp_ctrl_req_o);
  // One answer per controller request: forwarded and no error, or not forwarded and an error the
  // next cycle; with RRAM disabled, an error.
  OneAnswer_A: assert property (req && dis_f |-> if (mp_ctrl_req_o) (##1 !mp_mp_err_o) else (##1 mp_mp_err_o));
  // Host: every granted read is answered once; at most two outstanding.
  HostOutstandingBound_A: assert property (h_out <= 3'd2);
  HostNoAnswerWithoutRead_A: assert property (mp_host_rd_done_o |-> h_out > 3'd0);
  DisabledError_A: assert property (req && !dis_f |=> mp_mp_err_o);
  NoRequestNoError_A: assert property (!req |=> !mp_mp_err_o);
  NoRequestNoForward_A: assert property (!req |-> !mp_ctrl_req_o);
  NoHostNoForward_A: assert property (!mp_host_req_i |-> !mp_host_fwd_o);

  // ---- Observations (covers): open cases the documentation does not decide ----
  // A window whose EN is neither MuBi4True nor MuBi4False (one bit off MuBi4True) and a host read
  // in it that the unit forwards: the window-deny entry fails open on a corrupted EN.
  HostInvalidWindowEnForwarded_C: cover property (mp_host_req_i && mp_host_fwd_o &&
                                                  mp_emul_region_i[27:24] inside {4'h7, 4'h4, 4'h2, 4'he} &&
                                                  hpage >= mp_emul_region_i[11:0] &&
                                                  {1'b0, hpage} <= mp_emul_region_i[11:0] + mp_emul_region_i[23:12]);
  // The loopback requester (Rewrite) reaches pages software may not: an OTP page, the creator seed
  // page with its life-cycle enable off and INFO_PAGE_CFG_5 disabled, a page of an enabled window.
  LoopbackOtpWrite_C: cover property (mp_if_sel_i == 3'd4 && wr && data && page >= 12'd4090 && mp_ctrl_req_o);
  LoopbackSeedPageWrite_C: cover property ((mp_lc_creator_i != LcOn) [*3] ##0
                                           (mp_if_sel_i == 3'd4 && wr && !data && page == 12'd5 &&
                                            mp_info_cfg_i[100 +: 4] != T && mp_ctrl_req_o));
  LoopbackWindowWrite_C: cover property (mp_if_sel_i == 3'd4 && wr && data && mp_ctrl_req_o &&
                                         mp_emul_region_i[27:24] == T && mp_emul_region_i[11:0] == page);
  // A region whose SCRAMBLE_EN is not a valid MuBi4 value: forwarded with scrambling off.
  InvalidScrambleEnOff_C: cover property (sw && data && rd && mp_ctrl_req_o && !mp_ctrl_scr_o &&
                                          mp_default_cfg_i[11:8] == 4'h7);
endmodule

bind rram_ctrl_mp_unit rram_ctrl_mp_fpv u_fpv (.*);
