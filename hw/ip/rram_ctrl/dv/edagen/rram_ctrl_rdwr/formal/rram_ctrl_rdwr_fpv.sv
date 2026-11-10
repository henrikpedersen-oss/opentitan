// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's controller path (edagen rram_ctrl_rdwr.yaml formal.files), bound into
// rram_ctrl_rdwr_unit. Only the unit's ports are used, never a signal inside the design, and every
// property states a documented rule (rram_ctrl doc/theory_of_operation.md "Initialization",
// "RMA Handling", "Address-XOR", "RRAM Errors and Faults", "RRAM Escalation & Disable",
// "RRAM Write Data Path"; programmers_guide.md "Issuing a Controller Read / Write / Rewrite",
// "Write Alignment", "Error Encountered by Software Initiated Controller Operations"; registers.md
// CONTROL, ADDR, ERR_CODE). Henrik's FPV requirements (2026-11-08) for this unit:
//   1. every request gets exactly one response (A_*DoneOnlyForReq_A, A_*OneDone_A, liveness
//      A_*NoDeadlock_A);
//   2. arbitration has its documented priority (hardware before software: A_HwBeforeSw_A) and no
//      starvation (A_*NoDeadlock_A; with a requester that never stops: FpvFinding_*Starved*_A);
//   3. no program is reported done before it is complete: the documents have no verify step for a
//      write ("a write can always be verified with a subsequent read-back"); their obligation is
//      "done when all words have been programmed" (A_*WrDoneAllWords_A);
//   4. an error (read error, memory protection, write integrity) is never reported as good data or
//      a good completion (A_*RdErrOnes_A, A_*ErrReported_A, A_NoAccessAfter*Err_A);
//   5. nothing is accepted while the gating says no (A_NoAccessBeforePhyInit_A,
//      A_SwAccessNeedsInit_A, A_SwDisNoAccess_A, A_SwDisOpErr_A, A_DisNoAccess_A, A_RejectNoAccess_A,
//      A_RejectOpErr_A);
//   others the documents require: the address XOR removed for the requester (A_*XorRemoved_A),
//   the address sequence (A_RdAddr_A, A_WrAddr_A), the lcmgr phase (A_LcPhase_A), Rewrite data
//   hidden from software (A_RewriteHidden_A).
// FpvFinding_* state documented rules the design is expected to break (findings in
// status/rtl_tickets.md): each gives a counterexample while the finding is open. FpvMustFail_* are
// deliberately wrong versions of proven properties: each must give a counterexample. *_C covers
// show that the antecedents are reachable.
//
// Assumptions (the neighbours' documented protocols): CONTROL is held while START is set and START
// drops after done (the register clears it); a plug holds req with start, and start until done;
// the plugs issue reads and writes; memory protection / the PHY answer only a request; the write
// FIFO words of a TL-UL write are held until taken; phy_init_done, init_done and the disables are
// sticky; FIFO_CLR is not used. Liveness only: the PHY answers every request, the read FIFOs and
// the writers make progress, the plugs release req, phy_init_done and init_done come.
module rram_ctrl_rdwr_fpv
  import rram_ctrl_pkg::*;
(
  input logic         clk_i,
  input logic         rst_ni,
  input logic [3:0]   cp_rram_dis_i,
  input logic [3:0]   cp_sw_dis_i,
  input logic         cp_phy_init_done_i,
  input logic         cp_ctrl_init_done_i,
  input logic         cp_sw_start_i,
  input logic [1:0]   cp_sw_op_i,
  input logic         cp_sw_part_i,
  input logic [9:0]   cp_sw_num_i,
  input logic [20:0]  cp_sw_addr_i,
  input logic         cp_sw_done_o,
  input logic [3:0]   cp_sw_err_o,
  input logic         cp_sw_wvalid_i,
  input logic         cp_sw_wready_o,
  input logic [38:0]  cp_sw_wdata_i,
  input logic         cp_sw_rready_i,
  input logic         cp_sw_rvalid_o,
  input logic [38:0]  cp_sw_rdata_o,
  input logic         cp_lc_req_i,
  input logic         cp_lc_start_i,
  input logic [1:0]   cp_lc_op_i,
  input logic         cp_lc_part_i,
  input logic [9:0]   cp_lc_num_i,
  input logic [1:0]   cp_lc_phase_i,
  input logic [20:0]  cp_lc_addr_i,
  input logic         cp_lc_done_o,
  input logic [3:0]   cp_lc_err_o,
  input logic         cp_lc_wvalid_i,
  input logic [38:0]  cp_lc_wdata_i,
  input logic         cp_lc_wready_o,
  input logic         cp_lc_rready_i,
  input logic [38:0]  cp_lc_rdata_o,
  input logic         cp_lc_rvalid_o,
  input logic         cp_ot_req_i,
  input logic         cp_ot_start_i,
  input logic [1:0]   cp_ot_op_i,
  input logic         cp_ot_part_i,
  input logic [9:0]   cp_ot_num_i,
  input logic [20:0]  cp_ot_addr_i,
  input logic         cp_ot_done_o,
  input logic [3:0]   cp_ot_err_o,
  input logic         cp_ot_wvalid_i,
  input logic [38:0]  cp_ot_wdata_i,
  input logic         cp_ot_wready_o,
  input logic         cp_ot_rready_i,
  input logic         cp_ot_rvalid_o,
  input logic [2:0]   cp_if_sel_o,
  input logic [1:0]   cp_phase_o,
  input logic         cp_part_o,
  input logic [18:0]  cp_err_addr_o,
  input logic         cp_fifo_clr_i,
  input logic [4:0]   cp_fifo_depth_o,
  input logic         cp_rd_req_o,
  input logic [18:0]  cp_rd_addr_o,
  input logic         cp_rd_ovfl_o,
  input logic [38:0]  cp_rd_data_i,
  input logic         cp_rd_done_i,
  input logic         cp_rd_err_i,
  input logic         cp_rd_mp_err_i,
  input logic         cp_wr_req_o,
  input logic [18:0]  cp_wr_addr_o,
  input logic         cp_wr_ovfl_o,
  input logic [38:0]  cp_wr_data_o,
  input logic         cp_wr_last_o,
  input logic         cp_wr_done_i,
  input logic         cp_wr_mp_err_i,
  input logic         cp_wr_intg_err_i,
  input logic         cp_fsm_err_o,
  input logic         cp_cnt_err_o,
  input logic         cp_fifo_err_o
);
  default clocking @(posedge clk_i); endclocking
  default disable iff (!rst_ni);

  localparam logic [1:0] OpRd = 2'd0, OpWr = 2'd1, OpRw = 2'd2;
  localparam logic [2:0] SelSw = 3'd1, SelOtp = 3'd2, SelLc = 3'd3, SelLoop = 3'd4;
  localparam logic [3:0] MuBiFalse = 4'h9;  // prim_mubi_pkg::MuBi4False
  localparam logic [3:0] EOp = 4'b0001, EMp = 4'b0010, ERd = 4'b0100, EWr = 4'b1000;
  // all ones with valid bus integrity (tlul_data_integ_enc of 32'hffffffff)
  localparam logic [38:0] Ones39 = {7'h2a, 32'hffffffff};

  logic rd_rsp, wr_rsp, rd_bad, wr_bad, any_done, sw_sel, rd_new, wr_new;
  assign rd_rsp = cp_rd_req_o && cp_rd_done_i;
  assign wr_rsp = cp_wr_req_o && cp_wr_done_i;
  assign rd_bad = rd_rsp && (cp_rd_err_i || cp_rd_mp_err_i);
  assign wr_bad = wr_rsp && (cp_wr_mp_err_i || cp_wr_intg_err_i);
  assign any_done = cp_sw_done_o || cp_lc_done_o || cp_ot_done_o;
  assign sw_sel = cp_if_sel_o inside {SelSw, SelLoop};

  // ---- auxiliary state (from ports only) ----
  logic phy_seen, dis_ever;
  logic rd_err_q, rd_first_rd_q, rd_first_mp_q, wr_err_q, wr_first_wr_q, wr_first_mp_q;
  logic [10:0] rcnt, wcnt, dcnt_sw, dcnt_lc, dcnt_ot;
  logic sw_begun, lc_begun, ot_begun;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      phy_seen <= 1'b0;
      dis_ever <= 1'b0;
      {rd_err_q, rd_first_rd_q, rd_first_mp_q, wr_err_q, wr_first_wr_q, wr_first_mp_q} <= '0;
      {rcnt, wcnt, dcnt_sw, dcnt_lc, dcnt_ot} <= '0;
      {sw_begun, lc_begun, ot_begun} <= '0;
    end else begin
      phy_seen <= phy_seen | cp_phy_init_done_i;
      dis_ever <= dis_ever | (cp_rram_dis_i != MuBiFalse);
      if (any_done) begin
        {rd_err_q, rd_first_rd_q, rd_first_mp_q, wr_err_q, wr_first_wr_q, wr_first_mp_q} <= '0;
        {rcnt, wcnt, dcnt_sw, dcnt_lc, dcnt_ot} <= '0;
      end else begin
        if (rd_bad && !rd_err_q && !wr_err_q) begin
          rd_err_q <= 1'b1;
          rd_first_rd_q <= cp_rd_err_i;
          rd_first_mp_q <= cp_rd_mp_err_i;
        end
        if (wr_bad && !wr_err_q && !rd_err_q) begin
          wr_err_q <= 1'b1;
          wr_first_wr_q <= cp_wr_intg_err_i;
          wr_first_mp_q <= cp_wr_mp_err_i;
        end
        rcnt <= rcnt + 11'(rd_rsp);
        wcnt <= wcnt + 11'(wr_rsp);
        dcnt_sw <= dcnt_sw + 11'(cp_sw_rvalid_o && cp_sw_rready_i);
        dcnt_lc <= dcnt_lc + 11'(cp_lc_rvalid_o && cp_lc_rready_i);
        dcnt_ot <= dcnt_ot + 11'(cp_ot_rvalid_o && cp_ot_rready_i);
      end
      if (cp_sw_done_o) sw_begun <= 1'b0;
      else if ((cp_rd_req_o || cp_wr_req_o) && sw_sel) sw_begun <= 1'b1;
      if (cp_lc_done_o) lc_begun <= 1'b0;
      else if ((cp_rd_req_o || cp_wr_req_o) && cp_if_sel_o == SelLc) lc_begun <= 1'b1;
      if (cp_ot_done_o) ot_begun <= 1'b0;
      else if ((cp_rd_req_o || cp_wr_req_o) && cp_if_sel_o == SelOtp) ot_begun <= 1'b1;
    end
  end
  // counts including this cycle's response / delivery
  logic [10:0] rcnt_i, wcnt_i, dsw_i, dlc_i, dot_i;
  logic err_any, rd_err_now;
  assign rcnt_i = rcnt + 11'(rd_rsp);
  assign wcnt_i = wcnt + 11'(wr_rsp);
  assign dsw_i = dcnt_sw + 11'(cp_sw_rvalid_o && cp_sw_rready_i);
  assign dlc_i = dcnt_lc + 11'(cp_lc_rvalid_o && cp_lc_rready_i);
  assign dot_i = dcnt_ot + 11'(cp_ot_rvalid_o && cp_ot_rready_i);
  assign rd_err_now = rd_err_q || rd_bad;
  assign err_any = rd_err_q || wr_err_q || rd_bad || wr_bad;

  logic sw_begin, lc_begin, ot_begin, acc;
  assign acc = cp_rd_req_o || cp_wr_req_o;
  assign sw_begin = acc && sw_sel && !sw_begun;
  assign lc_begin = acc && cp_if_sel_o == SelLc && !lc_begun;
  assign ot_begin = acc && cp_if_sel_o == SelOtp && !ot_begun;

  // a request's first cycle; what held then (plug requests, the disables); the first failing word
  logic sw_start_q, lc_start_q, ot_start_q, sw_rise, lc_rise, ot_rise;
  logic hw_blocks_q, swdis_q, dis_sw_q, dis_lc_q, dis_ot_q;
  logic [18:0] first_err_addr_q;
  assign sw_rise = cp_sw_start_i && !sw_start_q;
  assign lc_rise = cp_lc_start_i && !lc_start_q;
  assign ot_rise = cp_ot_start_i && !ot_start_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      {sw_start_q, lc_start_q, ot_start_q} <= '0;
      {hw_blocks_q, swdis_q, dis_sw_q, dis_lc_q, dis_ot_q} <= '0;
      first_err_addr_q <= '0;
    end else begin
      sw_start_q <= cp_sw_start_i;
      lc_start_q <= cp_lc_start_i;
      ot_start_q <= cp_ot_start_i;
      if (cp_sw_done_o || (!cp_lc_req_i && !cp_ot_req_i)) hw_blocks_q <= 1'b0;
      else if (sw_rise) hw_blocks_q <= 1'b1;
      if (cp_sw_done_o) swdis_q <= 1'b0;
      else if (sw_rise && cp_sw_dis_i != MuBiFalse) swdis_q <= 1'b1;
      if (cp_sw_done_o) dis_sw_q <= 1'b0;
      else if (sw_rise && cp_rram_dis_i != MuBiFalse) dis_sw_q <= 1'b1;
      if (cp_lc_done_o) dis_lc_q <= 1'b0;
      else if (lc_rise && cp_rram_dis_i != MuBiFalse) dis_lc_q <= 1'b1;
      if (cp_ot_done_o) dis_ot_q <= 1'b0;
      else if (ot_rise && cp_rram_dis_i != MuBiFalse) dis_ot_q <= 1'b1;
      if (!any_done && !rd_err_q && !wr_err_q) begin
        if (rd_bad) first_err_addr_q <= cp_rd_addr_o;
        else if (wr_bad) first_err_addr_q <= cp_wr_addr_o;
      end
    end
  end

  // the software request is one that must be rejected (undefined OP, not whole RRAM words)
  logic sw_bad;
  assign sw_bad = cp_sw_start_i &&
                  (cp_sw_op_i == 2'd3 ||
                   (cp_sw_op_i == OpWr && (cp_sw_addr_i[3:0] != 4'd0 || cp_sw_num_i[1:0] != 2'b11)) ||
                   (cp_sw_op_i == OpRw && cp_sw_addr_i[3:0] != 4'd0));

  // the operation's first bus-word address, by requester
  logic [18:0] base;
  always_comb begin
    unique case (cp_if_sel_o)
      SelOtp:  base = cp_ot_addr_i[20:2];
      SelLc:   base = cp_lc_addr_i[20:2];
      default: base = cp_sw_addr_i[20:2];
    endcase
  end
  logic [18:0] want_rd_addr, want_wr_addr;
  assign want_rd_addr = base + 19'(rcnt);
  assign want_wr_addr = base + 19'(wcnt);

  // ---- assumptions: the neighbours' protocols ----
  AsmSwHold_M: assume property (cp_sw_start_i && !cp_sw_done_o |=>
                                cp_sw_start_i && $stable({cp_sw_op_i, cp_sw_part_i, cp_sw_num_i, cp_sw_addr_i}));
  AsmSwDrop_M: assume property (cp_sw_done_o |=> !cp_sw_start_i);
  AsmLcReq_M: assume property (cp_lc_start_i |-> cp_lc_req_i);
  AsmLcHold_M: assume property (cp_lc_start_i && !cp_lc_done_o |=>
                                cp_lc_start_i && $stable({cp_lc_op_i, cp_lc_part_i, cp_lc_num_i, cp_lc_addr_i, cp_lc_phase_i}));
  AsmLcDrop_M: assume property (cp_lc_done_o |=> !cp_lc_start_i);
  AsmLcOp_M: assume property (cp_lc_op_i inside {OpRd, OpWr});
  AsmOtReq_M: assume property (cp_ot_start_i |-> cp_ot_req_i);
  AsmOtHold_M: assume property (cp_ot_start_i && !cp_ot_done_o |=>
                                cp_ot_start_i && $stable({cp_ot_op_i, cp_ot_part_i, cp_ot_num_i, cp_ot_addr_i}));
  AsmOtDrop_M: assume property (cp_ot_done_o |=> !cp_ot_start_i);
  AsmOtOp_M: assume property (cp_ot_op_i inside {OpRd, OpWr});
  AsmRdAns_M: assume property (cp_rd_done_i |-> cp_rd_req_o);
  AsmWrAns_M: assume property (cp_wr_done_i |-> cp_wr_req_o);
  AsmRdErr_M: assume property (!cp_rd_done_i |-> !cp_rd_err_i && !cp_rd_mp_err_i);
  AsmWrErr_M: assume property (!cp_wr_done_i |-> !cp_wr_mp_err_i && !cp_wr_intg_err_i);
  AsmSwWHold_M: assume property (cp_sw_wvalid_i && !cp_sw_wready_o |=> cp_sw_wvalid_i && $stable(cp_sw_wdata_i));
  AsmPhySticky_M: assume property (cp_phy_init_done_i |=> cp_phy_init_done_i);
  AsmInitSticky_M: assume property (cp_ctrl_init_done_i |=> cp_ctrl_init_done_i);
  AsmSwDisSticky_M: assume property (cp_sw_dis_i != MuBiFalse |=> $stable(cp_sw_dis_i));
  AsmDisSticky_M: assume property (cp_rram_dis_i != MuBiFalse |=> $stable(cp_rram_dis_i));
  AsmNoFifoClr_M: assume property (!cp_fifo_clr_i);
  // liveness only (fairness)
  AsmRdAnsLive_M: assume property (cp_rd_req_o |-> s_eventually cp_rd_done_i);
  AsmWrAnsLive_M: assume property (cp_wr_req_o |-> s_eventually cp_wr_done_i);
  AsmSwRdyLive_M: assume property (1'b1 |-> s_eventually cp_sw_rready_i);
  AsmLcRdyLive_M: assume property (1'b1 |-> s_eventually cp_lc_rready_i);
  AsmOtRdyLive_M: assume property (1'b1 |-> s_eventually cp_ot_rready_i);
  AsmSwWLive_M: assume property (cp_sw_start_i |-> s_eventually (cp_sw_wvalid_i || !cp_sw_start_i));
  AsmLcWLive_M: assume property (cp_lc_start_i |-> s_eventually (cp_lc_wvalid_i || !cp_lc_start_i));
  AsmOtWLive_M: assume property (cp_ot_start_i |-> s_eventually (cp_ot_wvalid_i || !cp_ot_start_i));
  AsmPhyLive_M: assume property (1'b1 |-> s_eventually cp_phy_init_done_i);
  AsmInitLive_M: assume property (1'b1 |-> s_eventually cp_ctrl_init_done_i);

  // ---- 1. one response per request ----
  A_SwDoneOnlyForReq_A: assert property (cp_sw_done_o |-> cp_sw_start_i);
  A_LcDoneOnlyForReq_A: assert property (cp_lc_done_o |-> cp_lc_req_i && cp_lc_start_i);
  A_OtDoneOnlyForReq_A: assert property (cp_ot_done_o |-> cp_ot_req_i && cp_ot_start_i);
  A_SwOneDone_A: assert property (cp_sw_done_o |=> !cp_sw_done_o);
  A_LcOneDone_A: assert property (cp_lc_done_o |=> !cp_lc_done_o);
  A_OtOneDone_A: assert property (cp_ot_done_o |=> !cp_ot_done_o);
  A_OneOwner_A: assert property ($onehot0({cp_sw_done_o, cp_lc_done_o, cp_ot_done_o}));
  // liveness, rram_disable excluded (what happens after it is not documented: finding)
  // Liveness, rram_disable excluded (what happens after it is not documented: finding). The plugs
  // may keep requesting for ever ("back to back operations"), so each requester is shown answered
  // unless a requester that goes before it or holds the arbiter is active (no deadlock of its own);
  // the starvation itself is FpvFinding_*Starved*_A.
  A_SwNoDeadlock_A: assert property (cp_sw_start_i && !dis_ever |->
                                     s_eventually (cp_sw_done_o || dis_ever || cp_lc_req_i || cp_ot_req_i));
  A_OtNoDeadlock_A: assert property (cp_ot_start_i && !dis_ever |->
                                     s_eventually (cp_ot_done_o || dis_ever || cp_lc_req_i));
  A_LcNoDeadlock_A: assert property (cp_lc_start_i && !dis_ever |->
                                     s_eventually (cp_lc_done_o || dis_ever || cp_ot_req_i));

  // ---- 2. hardware before software ----
  // A plug requesting when software sets START goes first: no software access until every plug
  // request present then has been dropped.
  A_HwBeforeSw_A: assert property (acc && sw_sel |-> !(hw_blocks_q || (sw_rise && (cp_lc_req_i || cp_ot_req_i))));

  // ---- 3. a write is done only when all its words are programmed ----
  A_SwWrDoneAllWords_A: assert property (cp_sw_done_o && cp_sw_op_i == OpWr && cp_sw_err_o == 4'd0 |->
                                         wcnt_i == 11'(cp_sw_num_i) + 11'd1);
  A_LcWrDoneAllWords_A: assert property (cp_lc_done_o && cp_lc_op_i == OpWr && cp_lc_err_o == 4'd0 |->
                                         wcnt_i == 11'(cp_lc_num_i) + 11'd1);
  A_OtWrDoneAllWords_A: assert property (cp_ot_done_o && cp_ot_op_i == OpWr && cp_ot_err_o == 4'd0 |->
                                         wcnt_i == 11'(cp_ot_num_i) + 11'd1);
  A_RwDoneAllWords_A: assert property (cp_sw_done_o && cp_sw_op_i == OpRw && cp_sw_err_o == 4'd0 |->
                                       rcnt_i == 11'd4 && wcnt_i == 11'd4);
  A_SwRdDoneAllWords_A: assert property (cp_sw_done_o && cp_sw_op_i == OpRd && !sw_bad && cp_sw_err_o != EOp |->
                                         dsw_i == 11'(cp_sw_num_i) + 11'd1);
  A_LcRdDoneAllWords_A: assert property (cp_lc_done_o && cp_lc_op_i == OpRd |-> dlc_i == 11'(cp_lc_num_i) + 11'd1);
  A_OtRdDoneAllWords_A: assert property (cp_ot_done_o && cp_ot_op_i == OpRd |-> dot_i == 11'(cp_ot_num_i) + 11'd1);

  // ---- 4. an error is never good data or a good completion ----
  A_SwRdErrOnes_A: assert property (cp_sw_rvalid_o && rd_err_now |-> cp_sw_rdata_o == Ones39);
  A_LcRdErrOnes_A: assert property (cp_lc_rvalid_o && rd_err_now |-> cp_lc_rdata_o == Ones39);
  A_NoAccessAfterRdErr_A: assert property (rd_err_q |-> !cp_rd_req_o && !cp_wr_req_o);
  A_NoAccessAfterWrErr_A: assert property (wr_err_q |-> !cp_wr_req_o);
  A_SwRdErrReported_A: assert property (cp_sw_done_o && (rd_err_q ? rd_first_rd_q : (rd_bad && cp_rd_err_i)) |->
                                        cp_sw_err_o[2]);
  A_SwMpErrReported_A: assert property (cp_sw_done_o && (rd_err_q ? rd_first_mp_q : (rd_bad && cp_rd_mp_err_i)) |->
                                        cp_sw_err_o[1]);
  A_SwWrErrReported_A: assert property (cp_sw_done_o && (wr_err_q ? wr_first_wr_q : (wr_bad && cp_wr_intg_err_i)) |->
                                        cp_sw_err_o[3]);
  A_SwWrMpReported_A: assert property (cp_sw_done_o && (wr_err_q ? wr_first_mp_q : (wr_bad && cp_wr_mp_err_i)) |->
                                       cp_sw_err_o[1]);
  A_LcErrReported_A: assert property (cp_lc_done_o && err_any |-> cp_lc_err_o != 4'd0);
  A_OtErrReported_A: assert property (cp_ot_done_o && err_any |-> cp_ot_err_o != 4'd0);
  A_SwNoFalseErr_A: assert property (cp_sw_done_o && !err_any && !sw_bad && cp_sw_err_o != EOp |-> cp_sw_err_o == 4'd0);
  A_LcNoFalseErr_A: assert property (cp_lc_done_o && !err_any |-> cp_lc_err_o == 4'd0);

  // ---- 5. gating ----
  A_NoAccessBeforePhyInit_A: assert property (acc |-> phy_seen);
  A_SwAccessNeedsInit_A: assert property (acc && sw_sel |-> cp_ctrl_init_done_i);
  A_RejectNoAccess_A: assert property (acc && sw_sel |-> !sw_bad);
  A_RejectOpErr_A: assert property (cp_sw_done_o && sw_bad |-> cp_sw_err_o == EOp);
  A_NotInitOpErr_A: assert property (cp_sw_done_o && !cp_ctrl_init_done_i |-> cp_sw_err_o == EOp);
  // RMA software disable on at START: rejected (op_err), no access. rram_disable on at a software
  // request: no access for it. A plug that holds the arbiter keeps it after rram_disable (the arbiter
  // looks at rram_disable only when idle; the PHY is the documented block for controller requests,
  // "rram_phy observes the same rram_disable bus and suppresses all new host and controller
  // requests"): C_PlugAccessAfterDis_C shows it.
  A_SwDisNoAccess_A: assert property (acc && sw_sel |-> !(swdis_q || (sw_rise && cp_sw_dis_i != MuBiFalse)));
  A_SwDisOpErr_A: assert property (cp_sw_done_o && (swdis_q || (sw_rise && cp_sw_dis_i != MuBiFalse)) |->
                                   cp_sw_err_o == EOp);
  A_DisNoAccess_A: assert property (acc && sw_sel |-> !(dis_sw_q || (sw_rise && cp_rram_dis_i != MuBiFalse)));
  C_PlugAccessAfterDis_C: cover property (acc && ((cp_if_sel_o == SelLc && dis_lc_q) || (cp_if_sel_o == SelOtp && dis_ot_q)));
  // ERR_ADDR: "the address of the first word to cause an error for the operation" (finding: stale when
  // the failing word is the operation's last, the completion coming in the same cycle)
  FpvFinding_SwErrAddr_A: assert property (cp_sw_done_o && (cp_sw_err_o & (EMp | ERd | EWr)) != 4'd0 |->
                                  cp_err_addr_o == ((rd_err_q || wr_err_q) ? first_err_addr_q :
                                                    (rd_bad ? cp_rd_addr_o : cp_wr_addr_o)));

  // the same, the failing word not the last one (the error came before the completion): holds
  A_SwErrAddrEarlier_A: assert property (cp_sw_done_o && (cp_sw_err_o & (EMp | ERd | EWr)) != 4'd0 &&
                                         (rd_err_q || wr_err_q) |-> cp_err_addr_o == first_err_addr_q);

  // ---- others ----
  A_SwXorRemoved_A: assert property (cp_sw_rvalid_o && rd_rsp && !rd_err_now |->
                                     cp_sw_rdata_o == {cp_rd_data_i[38:32], cp_rd_data_i[31:0] ^ {13'd0, cp_rd_addr_o}});
  A_LcXorRemoved_A: assert property (cp_lc_rvalid_o && rd_rsp && !rd_err_now |->
                                     cp_lc_rdata_o == {cp_rd_data_i[38:32], cp_rd_data_i[31:0] ^ {13'd0, cp_rd_addr_o}});
  A_RdAddr_A: assert property (cp_rd_req_o |-> cp_rd_addr_o == want_rd_addr);
  A_WrAddr_A: assert property (cp_wr_req_o |-> cp_wr_addr_o == want_wr_addr);
  A_WrLast_A: assert property (cp_wr_req_o && cp_if_sel_o == SelSw |->
                               cp_wr_last_o == (wcnt == 11'(cp_sw_num_i)));
  A_LcPhase_A: assert property (acc && cp_if_sel_o == SelLc |-> cp_phase_o == cp_lc_phase_i);
  A_RewriteHidden_A: assert property (cp_sw_start_i && cp_sw_op_i == OpRw |-> !cp_sw_rvalid_o);
  A_LoopHidden_A: assert property (cp_if_sel_o == SelLoop |-> !cp_sw_rvalid_o);
  A_NoFault_A: assert property (!cp_fsm_err_o && !cp_cnt_err_o && !cp_fifo_err_o);

  // ---- findings: each gives a counterexample while its finding is open ----
  // ERR_CODE / OP_STATUS.err are "set when the software operation causing the error completes".
  FpvFinding_ErrBeforeDone_A: assert property (cp_sw_err_o != 4'd0 |-> cp_sw_done_o);
  // A request made after rram_disable is completed (blocked requests are not said to hang).
  FpvFinding_SwAnsweredAfterDis_A: assert property (cp_sw_start_i && dis_ever |->
                                                    s_eventually (cp_sw_done_o || cp_lc_req_i || cp_ot_req_i));
  // A word software pushes for its write after START is kept ("Data must be pushed after the start
  // has been sent"): it reaches the FIFO (no RRAM write takes a word in that cycle).
  FpvFinding_SwWordKept_A: assert property (cp_sw_start_i && $past(cp_sw_start_i) && $past(cp_sw_start_i, 2) && cp_sw_op_i == OpWr && !sw_bad &&
                                            cp_sw_wvalid_i && cp_sw_wready_o && !cp_wr_req_o && !wr_err_q &&
                                            cp_fifo_depth_o < 5'd16 |=>
                                            cp_fifo_depth_o == $past(cp_fifo_depth_o) + 5'd1);
  // A requester that keeps requesting does not starve the others (no document gives the order
  // between the plugs or says that a plug keeps the arbiter for several commands).
  FpvFinding_SwStarvedByPlugs_A: assert property (cp_sw_start_i && !dis_ever |-> s_eventually (cp_sw_done_o || dis_ever));
  FpvFinding_OtpStarvedByLcmgr_A: assert property (cp_ot_start_i && !dis_ever |-> s_eventually (cp_ot_done_o || dis_ever));
  FpvFinding_LcStarvedByOtp_A: assert property (cp_lc_start_i && !dis_ever |-> s_eventually (cp_lc_done_o || dis_ever));

  // ---- must fail ----
  FpvMustFail_RdErrNotOnes_A: assert property (cp_sw_rvalid_o && rd_err_q |-> cp_sw_rdata_o != Ones39);
  FpvMustFail_SwNeverAccess_A: assert property (acc |-> !sw_sel);
  FpvMustFail_WrDoneShort_A: assert property (cp_sw_done_o && cp_sw_op_i == OpWr && cp_sw_err_o == 4'd0 |->
                                              wcnt_i != 11'(cp_sw_num_i) + 11'd1);
  FpvMustFail_NoReject_A: assert property (cp_sw_done_o |-> cp_sw_err_o != EOp);
  FpvMustFail_LcNeverDone_A: assert property (!cp_lc_done_o);
  FpvMustFail_OtBeforeSwNever_A: assert property (sw_begin |-> !$past(cp_ot_req_i, 3));

  // ---- covers ----
  C_SwRdErrOnes_C: cover property (cp_sw_rvalid_o && rd_err_q && cp_sw_rdata_o == Ones39);
  C_SwRdDoneErr_C: cover property (cp_sw_done_o && cp_sw_op_i == OpRd && cp_sw_err_o == ERd);
  C_SwWrDoneOk_C: cover property (cp_sw_done_o && cp_sw_op_i == OpWr && cp_sw_err_o == 4'd0 && cp_sw_num_i == 10'd7);
  C_SwWrDoneErr_C: cover property (cp_sw_done_o && cp_sw_op_i == OpWr && cp_sw_err_o == EWr);
  C_RwDoneOk_C: cover property (cp_sw_done_o && cp_sw_op_i == OpRw && cp_sw_err_o == 4'd0);
  C_Reject_C: cover property (cp_sw_done_o && cp_sw_err_o == EOp && cp_ctrl_init_done_i);
  C_SwDisReject_C: cover property (cp_sw_done_o && cp_sw_err_o == EOp && cp_sw_dis_i != MuBiFalse && !sw_bad &&
                                   cp_ctrl_init_done_i);
  C_LcWhileSwWaits_C: cover property (lc_begin && cp_sw_start_i);
  C_OtWhileLcWaits_C: cover property (ot_begin && cp_lc_req_i);
  C_LcDone_C: cover property (cp_lc_done_o && cp_lc_op_i == OpWr && cp_lc_err_o == 4'd0);
  C_OtRd_C: cover property (cp_ot_done_o && cp_ot_op_i == OpRd && cp_ot_err_o == 4'd0);
  C_Wrap_C: cover property (cp_rd_req_o && cp_rd_ovfl_o);
  C_SwRdFull_C: cover property (cp_sw_start_i && cp_sw_op_i == OpRd && !cp_sw_rready_i && sw_begun);
endmodule

bind rram_ctrl_rdwr_unit rram_ctrl_rdwr_fpv u_fpv (.*);
