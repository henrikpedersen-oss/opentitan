// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's life-cycle manager plug (edagen rram_ctrl_lcmgr.yaml formal.files), bound into
// rram_ctrl_lcmgr_unit. Only the unit's ports are used, never a signal inside rram_ctrl_lcmgr, and
// every property states a documented rule (rram_ctrl doc/theory_of_operation.md "LCMGR Hardware
// Plug", "RMA Handling", "Memory Protection for LCMGR Hardware Plug", "RRAM Escalation & Disable";
// lc_ctrl_pkg's lc_tx_t: only On enables). Henrik's FPV requirements (2026-11-07) for this unit:
//   1. the state only moves forward: a phase once left is never re-entered, no skipped phase later;
//   2. with the life-cycle inputs not On (a production / live state, or a glitched value), no
//      test-only behaviour: no seed read and no valid seed without lc_seed_hw_rd_en On, no RMA wipe
//      without rma_req On, writes only in the RMA phase, seed reads only of the seed pages;
//   3. sticky decisions stay set under arbitrary inputs, the life-cycle inputs going back to
//      "earlier" values included: rma_sw_dis (or the stronger rma_dis_access), rma_dis_access,
//      rma_ack, init_done, intg_err;
//   4. multibit values that are neither true nor false act as "not enabled" (lc_tx), resp. as
//      "disabled" for rram_disable (mubi4: any value but MuBi4False disables, the fail-safe way).
// FpvMustFail_* are deliberately wrong versions of the properties: each must give a counterexample,
// which shows that the checks they mirror can fail. *_C covers show that the antecedents are
// reachable.
//
// Assumptions (the environment's documented protocol, nothing on the life-cycle inputs or on
// rram_disable, which stay arbitrary): OTP acknowledges a key only while it is requested, for one
// cycle (prim_sync_reqack's DST rule, otp_ctrl's key interface); the controller path answers only a
// request (done with req, read data for a read); INIT.VAL is rw1s (once 1, it stays 1).
module rram_ctrl_lcmgr_fpv
  import rram_ctrl_pkg::*;
(
  input logic          clk_i,
  input logic          rst_ni,
  input logic          clk_otp_i,
  input logic          rst_otp_ni,
  input logic          ok_addr_req_o,
  input logic          ok_data_req_o,
  input logic          ok_addr_ack_i,
  input logic          ok_data_ack_i,
  input logic [127:0]  ok_key_i,
  input logic [127:0]  ok_rand_key_i,
  input logic          ok_seed_valid_i,
  input logic          ct_init_i,
  input logic [3:0]    ct_seed_hw_rd_en_i,
  input logic [3:0]    ct_rma_req_i,
  input logic [63:0]   ct_rma_seed_i,
  input logic [3:0]    ct_rma_ack_o,
  input logic [3:0]    ct_rram_dis_i,
  input logic          ct_start_o,
  input logic [1:0]    ct_op_o,
  input logic          ct_part_o,
  input logic [9:0]    ct_num_o,
  input logic          ct_req_o,
  input logic [20:0]   ct_addr_o,
  input logic [1:0]    ct_phase_o,
  input logic          ct_done_i,
  input logic [3:0]    ct_err_i,
  input logic          ct_rready_o,
  input logic          ct_rvalid_i,
  input logic [38:0]   ct_rdata_i,
  input logic          ct_wvalid_o,
  input logic          ct_wready_i,
  input logic [38:0]   ct_wdata_o,
  input logic [255:0]  ct_creator_seed_o,
  input logic [255:0]  ct_owner_seed_o,
  input logic [1:0]    ct_seeds_valid_o,
  input logic [127:0]  ct_addr_key_o,
  input logic [127:0]  ct_data_key_o,
  input logic [127:0]  ct_rand_addr_key_o,
  input logic [127:0]  ct_rand_data_key_o,
  input logic [3:0]    ct_rma_dis_access_o,
  input logic [3:0]    ct_rma_sw_dis_o,
  input logic          ct_keys_valid_o,
  input logic          ct_init_done_o,
  input logic          ct_fatal_err_o,
  input logic          ct_intg_err_o,
  input logic          ct_seed_err_o
);
  localparam logic [3:0] LcOn = 4'h5, LcOff = 4'ha, MuBi4False = 4'h9;
  localparam logic [1:0] PhSeed = 2'd0, PhRma = 2'd1, PhNone = 2'd2;
  localparam logic [1:0] OpRead = 2'd0, OpWrite = 2'd1;

  logic [11:0] page;
  assign page = ct_addr_o[20:9];  // 512-byte pages (32 RRAM words of 128 bits)

  // ---- auxiliary state: what has been seen since reset (only from the ports) ----
  logic seed_en_ever_on, rma_req_ever_on, dis_ever, seen_seed, left_seed, seen_rma, left_rma;
  logic sw_dis_ever, rma_start_ever;
  logic [1:0] phase_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      seed_en_ever_on <= 1'b0;
      rma_req_ever_on <= 1'b0;
      dis_ever        <= 1'b0;
      seen_seed       <= 1'b0;
      left_seed       <= 1'b0;
      seen_rma        <= 1'b0;
      left_rma        <= 1'b0;
      sw_dis_ever     <= 1'b0;
      phase_q         <= PhNone;
    end else begin
      seed_en_ever_on <= seed_en_ever_on | (ct_seed_hw_rd_en_i == LcOn);
      rma_req_ever_on <= rma_req_ever_on | (ct_rma_req_i == LcOn);
      dis_ever        <= dis_ever | (ct_rram_dis_i != MuBi4False);
      seen_seed       <= seen_seed | (ct_phase_o == PhSeed);
      left_seed       <= left_seed | (phase_q == PhSeed && ct_phase_o != PhSeed);
      seen_rma        <= seen_rma | (ct_phase_o == PhRma);
      left_rma        <= left_rma | (phase_q == PhRma && ct_phase_o != PhRma);
      sw_dis_ever     <= sw_dis_ever | (ct_rma_sw_dis_o == LcOn);
      phase_q         <= ct_phase_o;
    end
  end
  assign rma_start_ever = sw_dis_ever | seen_rma;

  // ---- assumptions ----
  AddrAckOnlyWithReq_M: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
                                         ok_addr_ack_i |-> ok_addr_req_o);
  DataAckOnlyWithReq_M: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
                                         ok_data_ack_i |-> ok_data_req_o);
  AddrAckOneCycle_M: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
                                      ok_addr_ack_i |=> !ok_addr_ack_i);
  DataAckOneCycle_M: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
                                      ok_data_ack_i |=> !ok_data_ack_i);
  DoneOnlyWithReq_M: assume property (@(posedge clk_i) disable iff (!rst_ni) ct_done_i |-> ct_req_o);
  RvalidOnlyForRead_M: assume property (@(posedge clk_i) disable iff (!rst_ni)
                                        ct_rvalid_i |-> ct_req_o && ct_op_o == OpRead);
  InitIsRw1s_M: assume property (@(posedge clk_i) disable iff (!rst_ni) ct_init_i |=> ct_init_i);

  default clocking @(posedge clk_i); endclocking
  default disable iff (!rst_ni);

  // ---- 1. forward only ----
  // A phase once left is never re-entered; no seed phase after the RMA phase.
  FpvSeedPhaseOnce_A: assert property (left_seed |-> ct_phase_o != PhSeed);
  FpvRmaPhaseOnce_A:  assert property (left_rma |-> ct_phase_o != PhRma);
  FpvNoSeedAfterRma_A: assert property (seen_rma |-> ct_phase_o != PhSeed);
  // init_done (StWait reached) and keys_valid only rise; init_done is never followed by seed reads.
  FpvInitDoneSticky_A: assert property (ct_init_done_o |=> ct_init_done_o);
  FpvNoSeedAfterInitDone_A: assert property (ct_init_done_o |-> ct_phase_o != PhSeed);

  // ---- 2. no test-only behaviour without the life-cycle enables ----
  // Seeds: read and valid only after lc_seed_hw_rd_en was On (strictly; a glitched value is Off).
  FpvSeedReadNeedsEnable_A: assert property (ct_req_o && ct_phase_o == PhSeed |-> seed_en_ever_on);
  FpvSeedValidNeedsEnable_A: assert property (ct_seeds_valid_o != 2'b00 |-> seed_en_ever_on);
  // Seed reads: read only, the creator and owner info pages only (PhaseSeed rules).
  // A request is req with start (req stays high through the whole phase, also between operations,
  // with the address at 0: 2026-11-07, without start this gave a counterexample in StReadEval).
  FpvSeedReadScope_A: assert property (ct_req_o && ct_start_o && ct_phase_o == PhSeed |->
                                       ct_op_o == OpRead && ct_part_o == 1'b1 &&
                                       (page == 12'd5 || page == 12'd6));
  // RMA: only after rma_req was On (strictly), only in PhaseRma, only the wipe entries.
  FpvRmaNeedsRequest_A: assert property (rma_start_ever |-> rma_req_ever_on);
  FpvWriteOnlyInRma_A: assert property (ct_req_o && ct_op_o == OpWrite |-> ct_phase_o == PhRma);
  FpvRmaScope_A: assert property (ct_req_o && ct_start_o && ct_phase_o == PhRma |->
                                  (ct_part_o == 1'b1 && page inside {12'd5, 12'd6, 12'd7}) ||
                                  (ct_part_o == 1'b0 && page < 12'd4090));
  // No controller request at all outside the seed and RMA phases.
  FpvReqOnlyInPhase_A: assert property (ct_req_o |-> ct_phase_o inside {PhSeed, PhRma});
  // Once the RMA entry started it never returns to normal operation without reset: software stays
  // locked out (rma_sw_dis, or the stronger rma_dis_access) and no seed phase comes back.
  FpvRmaNoReturn_A: assert property (rma_start_ever |-> ct_rma_sw_dis_o == LcOn ||
                                                        ct_rma_dis_access_o == LcOn);

  // ---- 3. sticky decisions, whatever the inputs do afterwards ----
  FpvSwDisSticky_A: assert property (ct_rma_sw_dis_o == LcOn |=>
                                     ct_rma_sw_dis_o == LcOn || ct_rma_dis_access_o == LcOn);
  FpvDisAccessSticky_A: assert property (ct_rma_dis_access_o == LcOn |=> ct_rma_dis_access_o == LcOn);
  FpvAckSticky_A: assert property (ct_rma_ack_o == LcOn |=> ct_rma_ack_o == LcOn);
  FpvAckWithSwDis_A: assert property (ct_rma_ack_o == LcOn |-> ct_rma_sw_dis_o == LcOn);
  FpvAckNotWithDisAccess_A: assert property (ct_rma_ack_o == LcOn |-> ct_rma_dis_access_o != LcOn);
  FpvIntgSticky_A: assert property (ct_intg_err_o |=> ct_intg_err_o);
  // rram_disable: the seeds are cleared ("The seed_valid bits are cleared again when the RRAM
  // controller is disabled"), and stay cleared, also if rram_disable goes back to MuBi4False.
  FpvSeedsClearedAfterDisable_A: assert property (dis_ever |=> ct_seeds_valid_o == 2'b00);
  // Without a completed RMA, a disable locks every access (rma_dis_access On) one cycle later.
  FpvDisableLocks_A: assert property (dis_ever && ct_rma_ack_o != LcOn |=>
                                      ct_rma_dis_access_o == LcOn || ct_rma_ack_o == LcOn);
  // No key request and no controller request once disabled.
  FpvNoReqAfterDisable_A: assert property (dis_ever |=> !ct_req_o);

  // ---- 4. multibit outputs are only On or Off ----
  FpvAckValid_A: assert property (ct_rma_ack_o inside {LcOn, LcOff});
  FpvSwDisValid_A: assert property (ct_rma_sw_dis_o inside {LcOn, LcOff});
  FpvDisAccessValid_A: assert property (ct_rma_dis_access_o inside {LcOn, LcOff});

  // ---- must fail: wrong versions, each must give a counterexample ----
  // (a) the phase after the seeds is not None (it is): shows FpvSeedPhaseOnce_A watches real exits.
  FpvMustFail_SeedPhaseNotNone_A: assert property (left_seed |-> ct_phase_o != PhNone);
  // (b) the RMA never wipes the creator page (it does: the first wipe entry).
  FpvMustFail_RmaNoCreatorPage_A: assert property (ct_req_o && ct_start_o && ct_phase_o == PhRma |->
                                                   !(ct_part_o == 1'b1 && page == 12'd5));
  // (c) seeds never become valid although enabled (they do): shows FpvSeedValidNeedsEnable_A's
  //     antecedent is reachable with the enable On.
  FpvMustFail_SeedsNeverValid_A: assert property (seed_en_ever_on |-> ct_seeds_valid_o == 2'b00);
  // (d) rma_sw_dis stays On by itself (it does not: a disable during the wipe moves to the disabled
  //     state, where rma_dis_access takes over): why FpvSwDisSticky_A allows rma_dis_access.
  FpvMustFail_SwDisAlone_A: assert property (ct_rma_sw_dis_o == LcOn |=> ct_rma_sw_dis_o == LcOn);
  // (e) the RMA starts without its request: with rma_req On it does start.
  FpvMustFail_RmaNeverStarts_A: assert property (rma_req_ever_on |-> !rma_start_ever);

  // ---- covers: the antecedents are reachable ----
  FpvBothSeedsValid_C: cover property (ct_seeds_valid_o == 2'b11);
  FpvSeedValidThenLcOff_C: cover property (ct_seeds_valid_o != 2'b00 && ct_seed_hw_rd_en_i == LcOff);
  FpvGlitchedEnableInitDone_C: cover property (!(ct_seed_hw_rd_en_i inside {LcOn, LcOff}) &&
                                               !seed_en_ever_on && ct_init_done_o);
  FpvRmaWipe_C: cover property (ct_req_o && ct_phase_o == PhRma && ct_op_o == OpWrite);
  FpvRmaThenReqOff_C: cover property (rma_start_ever && ct_rma_req_i == LcOff);
  FpvDisabled_C: cover property (ct_rma_dis_access_o == LcOn);
  FpvDisableDuringRma_C: cover property (seen_rma && dis_ever && ct_rma_dis_access_o == LcOn);
  FpvDisableGoesBack_C: cover property (dis_ever && ct_rram_dis_i == MuBi4False);
  FpvSeedErr_C: cover property (ct_seed_err_o);
  FpvIntgErr_C: cover property (ct_intg_err_o);
endmodule

bind rram_ctrl_lcmgr_unit rram_ctrl_lcmgr_fpv u_fpv (.*);
