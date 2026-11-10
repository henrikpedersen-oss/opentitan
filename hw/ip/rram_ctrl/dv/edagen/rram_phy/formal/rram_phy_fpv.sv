// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's PHY unit (edagen rram_phy.yaml formal.files), bound into rram_phy_unit. From
// theory_of_operation.md "RRAM Data Path", "RRAM Read Integrity", "RRAM ECC Error Handling",
// "RRAM Phy", "RRAM Read Pipeline", "RRAM Read Buffer" and the macro interface (rram_ctrl README):
//   - at most NumOutstandingRdReq (2) host reads in flight; a host response only for a granted
//     read, a controller response only for a requested access; every granted host read answered
//     (liveness);
//   - no macro request before init_done ("rram_phy does not issue requests before"); a macro request
//     and its attributes held stable until ack; never a read and a write request at once;
//   - read-after-write: after a commit to the page of a word S, a host read of S granted later is
//     never answered from the read buffer without a new macro read of S ("Any read-buffer entry
//     matching the written page is invalidated, so subsequent reads re-fetch");
//   - an uncorrectable ECC error is never returned as good data: once the macro has answered a read
//     of S with ecc_err 100, every host response for S (until S's page is rewritten) has the error
//     flag ("The macro marks the data as uncorrectable, and the ECC decoder cannot recover the
//     original data");
//   - a corrupted macro read (the shadow read's data differing from the first read's) raises
//     rd_intg_err ("If the two reads return different data ... rd_intg_err is raised"; liveness).
// S is a symbolic RRAM word address (data partition), constant over a trace, so each property holds
// for every word.
// Assumptions: the macro follows its documented interface (ack only for a request, one request at a
// time: done after ack, exactly once, init_done stays up once raised, ecc_err one-hot0, no err) and
// responds eventually; the macro's contents of S do not change until S's page is committed (its
// data and ECC status are the same for every read of S), apart from at most one corrupted read;
// rram_ctrl holds a controller request and its attributes until it completes, and a host request
// until granted; scrambling off and ECC on (the data path abstracted: the scrambler has its own
// proofs in the rram_scramble unit; ECC off would let the macro return raw data); no disable; the
// requesters leave the PHY idle now and then (the shadow read runs "when the RRAM is idle").
// Each assumption has a cover.
module rram_phy_fpv
  import rram_ctrl_pkg::*;
(
  input logic         clk_i,
  input logic         rst_ni,
  input logic [3:0]   ph_rram_disable_i,
  input logic         ph_ctrl_req_i,
  input logic         ph_ctrl_scramble_en_i,
  input logic         ph_ctrl_ecc_en_i,
  input logic         ph_ctrl_addr_xor_en_i,
  input logic         ph_ctrl_rd_i,
  input logic         ph_ctrl_wr_i,
  input logic         ph_ctrl_part_i,
  input logic [18:0]  ph_ctrl_addr_i,
  input logic [38:0]  ph_ctrl_wr_data_i,
  input logic         ph_ctrl_wr_last_i,
  input logic         ph_ctrl_wr_done_o,
  input logic         ph_ctrl_rd_done_o,
  input logic         ph_host_req_i,
  input logic         ph_host_gnt_o,
  input logic         ph_host_scramble_en_i,
  input logic         ph_host_ecc_en_i,
  input logic [18:0]  ph_host_addr_i,
  input logic         ph_host_rd_done_o,
  input logic         ph_host_rd_err_o,
  input logic         ph_rd_intg_err_o,
  input logic         ph_m_rd_req_o,
  input logic         ph_m_wr_req_o,
  input logic         ph_m_wr_last_o,
  input logic [16:0]  ph_m_addr_o,
  input logic [127:0] ph_m_wr_data_o,
  input logic         ph_m_part_o,
  input logic         ph_m_ecc_en_o,
  input logic         ph_m_ack_i,
  input logic         ph_m_done_i,
  input logic         ph_m_err_i,
  input logic [2:0]   ph_m_ecc_err_i,
  input logic [127:0] ph_m_rd_data_i,
  input logic         ph_m_init_done_i
);

  default clocking @(posedge clk_i); endclocking
  default disable iff (!rst_ni);

  logic m_req;
  assign m_req = ph_m_rd_req_o | ph_m_wr_req_o;

  // ---- free, constant symbolic values (undriven: Jasper treats them as free inputs) ----
  logic [16:0]  s_addr;
  logic [127:0] s_data;
  logic [2:0]   s_ecc;
  logic         fault_now;  // free each cycle: this read of S is corrupted
  S_AddrStable_M: assume property ($stable(s_addr));
  S_DataStable_M: assume property ($stable(s_data));
  S_EccStable_M:  assume property ($stable(s_ecc));
  S_EccLegal_M:   assume property (s_ecc inside {3'b000, 3'b001, 3'b010, 3'b100});

  // ---- the macro (README "RRAM Controller - Macro Interface") ----
  logic m_out_q;   // a request accepted (ack), done still to come
  logic m_rd_q;    // that request is a read
  logic m_s_q;     // ... of S (data partition)
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      m_out_q <= 1'b0;
      m_rd_q  <= 1'b0;
      m_s_q   <= 1'b0;
    end else if (ph_m_ack_i) begin
      m_out_q <= 1'b1;
      m_rd_q  <= ph_m_rd_req_o;
      m_s_q   <= ph_m_rd_req_o && ph_m_part_o == 1'b0 && ph_m_addr_o == s_addr;
    end else if (ph_m_done_i) begin
      m_out_q <= 1'b0;
    end
  end
  M_AckForReq_M:   assume property (ph_m_ack_i |-> m_req);
  M_OneAtATime_M:  assume property (ph_m_ack_i |-> !m_out_q);
  M_DoneAfterAck_M: assume property (ph_m_done_i |-> m_out_q);
  M_NoErr_M:       assume property (!ph_m_err_i);
  M_EccOneHot0_M:  assume property ($onehot0(ph_m_ecc_err_i));
  M_InitStays_M:   assume property (ph_m_init_done_i |=> ph_m_init_done_i);
  M_AckLive_M:     assume property (m_req && !m_out_q |-> s_eventually ph_m_ack_i);
  M_DoneLive_M:    assume property (m_out_q |-> s_eventually ph_m_done_i);

  // The macro's contents of S: the same data and ECC status for every read until S's page is
  // committed, apart from at most one corrupted read (data differs, no ECC status).
  logic s_committed_q, fault_used_q;
  logic s_page_commit;
  assign s_page_commit = ph_m_wr_req_o && ph_m_wr_last_o && ph_m_ack_i && ph_m_part_o == 1'b0 &&
                         ph_m_addr_o[16:5] == s_addr[16:5];
  logic s_rd_done;
  assign s_rd_done = ph_m_done_i && m_out_q && m_rd_q && m_s_q;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      s_committed_q <= 1'b0;
      fault_used_q  <= 1'b0;
    end else begin
      if (s_page_commit) s_committed_q <= 1'b1;
      if (fault_now) fault_used_q <= 1'b1;
    end
  end
  S_Content_M: assume property (s_rd_done && !s_committed_q && !fault_now |->
                                ph_m_rd_data_i == s_data && ph_m_ecc_err_i == s_ecc);
  S_FaultOnce_M: assume property (fault_now |-> s_rd_done && !s_committed_q && !fault_used_q &&
                                  s_ecc == 3'b000 && ph_m_rd_data_i != s_data &&
                                  ph_m_ecc_err_i == 3'b000);

  // ---- rram_ctrl's side ----
  C_Hold_M: assume property (ph_ctrl_req_i && !(ph_ctrl_rd_done_o || ph_ctrl_wr_done_o) |=>
                             ph_ctrl_req_i && $stable({ph_ctrl_rd_i, ph_ctrl_wr_i, ph_ctrl_part_i,
                                                       ph_ctrl_addr_i, ph_ctrl_wr_data_i,
                                                       ph_ctrl_wr_last_i, ph_ctrl_ecc_en_i,
                                                       ph_ctrl_addr_xor_en_i}));
  C_OneKind_M: assume property (ph_ctrl_req_i |-> ph_ctrl_rd_i ^ ph_ctrl_wr_i);
  H_Hold_M: assume property (ph_host_req_i && !ph_host_gnt_o |=>
                             ph_host_req_i && $stable({ph_host_addr_i, ph_host_ecc_en_i}));
  Abstract_M: assume property (!ph_host_scramble_en_i && !ph_ctrl_scramble_en_i &&
                               ph_host_ecc_en_i && ph_ctrl_ecc_en_i &&
                               ph_rram_disable_i == 4'h9);
  Idle_M: assume property (s_eventually (!ph_host_req_i && !ph_ctrl_req_i));

  // ---- host reads in flight, and what each was granted for ----
  logic [1:0] host_out_q;
  logic [1:0] q_s_q, q_after_q;  // up to 2 entries: read of S, granted after a commit to S's page
  logic       commit_seen_q, read_since_q, s_bad_q;
  wire host_is_s = ph_host_addr_i[18:2] == s_addr;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      host_out_q    <= '0;
      q_s_q         <= '0;
      q_after_q     <= '0;
      commit_seen_q <= 1'b0;
      read_since_q  <= 1'b0;
      s_bad_q       <= 1'b0;
    end else begin
      // the queue (pop at the response, push at the grant; both in one cycle shift and append)
      unique case ({ph_host_gnt_o, ph_host_rd_done_o})
        2'b10: begin
          q_s_q[host_out_q[0]]     <= host_is_s;
          q_after_q[host_out_q[0]] <= commit_seen_q;
          host_out_q               <= host_out_q + 2'd1;
        end
        2'b01: begin
          q_s_q      <= {1'b0, q_s_q[1]};
          q_after_q  <= {1'b0, q_after_q[1]};
          host_out_q <= host_out_q - 2'd1;
        end
        2'b11: begin
          q_s_q     <= host_out_q == 2'd1 ? {1'b0, host_is_s} : {host_is_s, q_s_q[1]};
          q_after_q <= host_out_q == 2'd1 ? {1'b0, commit_seen_q} : {commit_seen_q, q_after_q[1]};
        end
        default: ;
      endcase
      if (s_page_commit) begin
        commit_seen_q <= 1'b1;
        read_since_q  <= 1'b0;
        s_bad_q       <= 1'b0;
      end else if (ph_m_ack_i && ph_m_rd_req_o && ph_m_part_o == 1'b0 && ph_m_addr_o == s_addr) begin
        read_since_q  <= 1'b1;
      end
      if (s_rd_done && !s_committed_q && ph_m_ecc_err_i == 3'b100) s_bad_q <= 1'b1;
    end
  end

  // ---- assertions ----
  HostOutstanding_A:   assert property (host_out_q <= 2'd2);
  HostRspForGrant_A:   assert property (ph_host_rd_done_o |-> host_out_q != 2'd0);
  HostGntForReq_A:     assert property (ph_host_gnt_o |-> ph_host_req_i);
  HostRspLive_A:       assert property (host_out_q != 2'd0 |-> s_eventually ph_host_rd_done_o);
  CtrlRdDoneForReq_A:  assert property (ph_ctrl_rd_done_o |-> ph_ctrl_req_i && ph_ctrl_rd_i);
  CtrlWrDoneForReq_A:  assert property (ph_ctrl_wr_done_o |-> ph_ctrl_req_i && ph_ctrl_wr_i);
  MacroAfterInit_A:    assert property (m_req |-> $past(ph_m_init_done_i));
  MacroReqStable_A:    assert property (m_req && !ph_m_ack_i |=>
                                        m_req && $stable({ph_m_rd_req_o, ph_m_wr_req_o, ph_m_wr_last_o,
                                                          ph_m_addr_o, ph_m_part_o, ph_m_ecc_en_o}));
  MacroPushDataStable_A: assert property (ph_m_wr_req_o && !ph_m_wr_last_o && !ph_m_ack_i |=>
                                          $stable(ph_m_wr_data_o));
  MacroRdXorWr_A:      assert property (!(ph_m_rd_req_o && ph_m_wr_req_o));
  // read-after-write: the response for a read of S granted after a commit to S's page needs a macro
  // read of S after that commit
  ReadAfterWrite_A:    assert property (ph_host_rd_done_o && q_s_q[0] && q_after_q[0] |-> read_since_q);
  // an uncorrectable word is never returned as good data
  EccNotGood_A:        assert property (ph_host_rd_done_o && q_s_q[0] && s_bad_q |-> ph_host_rd_err_o);
  // a corrupted macro read raises rd_intg_err (the shadow read compares)
  // (unless S's page is rewritten first: the entry is invalidated and its shadow read dropped)
  IntgFault_A:         assert property (fault_now |-> s_eventually (ph_rd_intg_err_o || s_committed_q));

  // ---- covers ----
  TwoOutstanding_C:  cover property (host_out_q == 2'd2);
  HostHit_C:         cover property (ph_host_gnt_o && !m_req ##1 ph_host_rd_done_o);
  EccBadRsp_C:       cover property (ph_host_rd_done_o && q_s_q[0] && s_bad_q);
  RawRead_C:         cover property (ph_host_rd_done_o && q_s_q[0] && q_after_q[0]);
  Fault_C:           cover property (fault_now);
  IntgErr_C:         cover property (ph_rd_intg_err_o);
  Commit_C:          cover property (s_page_commit);
  CtrlWrDone_C:      cover property (ph_ctrl_wr_done_o && ph_ctrl_wr_last_i);
  MacroAckWait_C:    cover property (m_req && !ph_m_ack_i ##1 ph_m_ack_i);
endmodule

bind rram_phy_unit rram_phy_fpv u_fpv (.*);
