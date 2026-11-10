// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's OTP hardware plug (edagen rram_ctrl_otp.yaml formal.files), bound into
// rram_ctrl_otp_unit. Two asynchronous clocks: the otp_ctrl side on clk_otp_i, the controller side
// on clk_i. Written from the documentation:
//   - the plug reaches the OTP pages only (theory_of_operation.md "Memory Protection for OTP
//     Hardware Plug": "accesses only the OTP region, the top pages of the data partition"; the
//     address map in README.md: pages 4090-4095 of the data partition);
//   - its controller operations are well formed: a write covers whole RRAM words (registers.md
//     CONTROL.NUM, otherwise op_err), every write word carries valid TL-UL data integrity;
//   - otp_ctrl's command protocol (otp_ctrl theory_of_operation.md "Generalized Open-source
//     Interface"): one response per accepted command, in order, never one without a command; the
//     error code is one of the Macro* codes, never MacroEccCorrError (theory_of_operation.md
//     "Integrity Scheme": correctable errors "never surface as an error");
//   - with RRAM access disabled every command but Init is answered with an error, and after an
//     undefined command (FAULT_STATUS.otp_op_err) the plug answers no command with NoError;
//   - intg_err only after a read word with broken bus integrity, fatal_err only after an
//     undefined command (no other fault is reachable without fault injection).
//
// Assumptions, as otp_ctrl and the controller path drive the plug: valid is held with its command
// until ready (ready / valid handshake), at most two commands outstanding ("the open source OTP
// controller allows up to two outstanding OTP commands"), done answers a request. Every assumption
// has a cover showing the checked cases stay reachable.
module rram_ctrl_otp_fpv
  import otp_ctrl_macro_pkg::*;
  import rram_ctrl_pkg::*;
(
  input logic         clk_i,
  input logic         rst_ni,
  input logic         clk_otp_i,
  input logic         rst_otp_ni,
  input logic         ot_valid_i,
  input logic [6:0]   ot_cmd_i,
  input logic [1:0]   ot_size_i,
  input logic [10:0]  ot_addr_i,
  input logic [63:0]  ot_wdata_i,
  input logic         ot_ready_o,
  input logic         ot_rvalid_o,
  input logic [63:0]  ot_rdata_o,
  input logic [2:0]   ot_err_o,
  input logic         ot_fatal_lc_fsm_err_o,
  input logic         ot_fatal_alert_o,
  input logic         ot_recov_alert_o,
  input logic         ct_start_o,
  input logic [1:0]   ct_op_o,
  input logic         ct_part_o,
  input logic [9:0]   ct_num_o,
  input logic         ct_req_o,
  input logic [20:0]  ct_addr_o,
  input logic         ct_done_i,
  input logic [3:0]   ct_err_i,
  input logic         ct_rready_o,
  input logic         ct_rvalid_i,
  input logic [38:0]  ct_rdata_i,
  input logic         ct_wvalid_o,
  input logic         ct_wready_i,
  input logic [38:0]  ct_wdata_o,
  input logic         ct_fatal_err_o,
  input logic         ct_intg_err_o,
  input logic         ct_init_done_i,
  input logic         ct_wr_busy_i,
  input logic [3:0]   ct_rram_dis_i
);
  localparam logic [3:0] MuBi4True = 4'h6;
  localparam int unsigned FirstOtpPage = 4090, LastOtpPage = 4095;

  function automatic logic is_cmd(logic [6:0] c);
    return c inside {Read, Write, ReadRaw, WriteRaw, Init, Zeroize};
  endfunction

  // ---- otp_ctrl side: the accepted commands, in order (at most two) ----
  logic accept;
  assign accept = ot_valid_i && ot_ready_o;
  logic [1:0] n_out;
  logic [6:0] q_cmd[2];
  logic       q_poison[2];  // accepted after an undefined command (or is one)
  logic       bad_cmd_seen;
  always_ff @(posedge clk_otp_i or negedge rst_otp_ni) begin
    if (!rst_otp_ni) begin
      n_out <= '0;
      q_cmd <= '{default: '0};
      q_poison <= '{default: 1'b0};
      bad_cmd_seen <= 1'b0;
    end else begin
      logic pop, push, poison;
      pop = ot_rvalid_o && n_out != 0;
      push = accept;
      poison = bad_cmd_seen || !is_cmd(ot_cmd_i);
      if (pop) begin
        q_cmd[0] <= q_cmd[1];
        q_poison[0] <= q_poison[1];
      end
      if (push) begin
        // The new command goes behind the ones still outstanding after this cycle's pop.
        if ((n_out == 0) || (n_out == 1 && pop)) begin
          q_cmd[0] <= ot_cmd_i;
          q_poison[0] <= poison;
        end else begin
          q_cmd[1] <= ot_cmd_i;
          q_poison[1] <= poison;
        end
        if (!is_cmd(ot_cmd_i)) bad_cmd_seen <= 1'b1;
      end
      n_out <= n_out + 2'(push) - 2'(pop);
    end
  end

  // ---- controller side ----
  logic dis_always;  // RRAM access disabled in every cycle since reset
  logic bad_rd_seen;  // a read word with broken bus integrity has been delivered
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      dis_always <= 1'b1;
      bad_rd_seen <= 1'b0;
    end else begin
      if (ct_rram_dis_i != MuBi4True) dis_always <= 1'b0;
      if (ct_rvalid_i && ct_rdata_i != prim_secded_pkg::prim_secded_inv_39_32_enc(ct_rdata_i[31:0]))
        bad_rd_seen <= 1'b1;
    end
  end
  logic dis_now;
  assign dis_now = dis_always && ct_rram_dis_i == MuBi4True;

  // ---- assumptions ----
  // ready / valid: a command is held, unchanged, until it is accepted.
  M_valid_held: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_valid_i && !ot_ready_o |=> ot_valid_i && $stable({ot_cmd_i, ot_size_i, ot_addr_i, ot_wdata_i}));
  // At most two commands outstanding.
  M_two_out: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) n_out == 2 |-> !ot_valid_i);
  // done answers a request.
  M_done_req: assume property (@(posedge clk_i) disable iff (!rst_ni) ct_done_i |-> ct_req_o);
  // otp_ctrl sends Init first and only then (otp_ctrl theory_of_operation.md "Direct Access
  // Interface Control": "Upon reset release, the DAI controller first sends an initialization
  // command"); a command before Init is dropped without a response and a second Init is a fatal
  // error (status/rtl_tickets.md, the OTP plug's undocumented corner cases).
  logic init_seen;
  always_ff @(posedge clk_otp_i or negedge rst_otp_ni)
    if (!rst_otp_ni) init_seen <= 1'b0;
    else if (accept && ot_cmd_i == Init) init_seen <= 1'b1;
  M_init_first: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_valid_i && !init_seen |-> ot_cmd_i == Init);
  M_init_once: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_valid_i && init_seen |-> ot_cmd_i != Init);
  // 16-, 32- or 64-bit words ("a single OTP word, 16, 32, or 64 bits wide"), aligned to their size
  // as otp_ctrl aligns its accesses, inside the OTP data pages (otp_ctrl's depth, 1032 native words,
  // is inside them; an address past them wraps, see the ticket).
  M_size: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) ot_valid_i |-> ot_size_i != 2'd2);
  M_align: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_valid_i |-> (ot_size_i == 2'd1 -> ot_addr_i[0] == 1'b0) && (ot_size_i == 2'd3 -> ot_addr_i[1:0] == 2'b00));
  M_in_pages: assume property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) ot_valid_i |-> ot_addr_i < 11'd1280);
  // RRAM access stays disabled until reset once it is ("the only way to restore normal operation is
  // a full system reset", theory_of_operation.md "RRAM Escalation & Disable").
  M_dis_sticky: assume property (@(posedge clk_i) disable iff (!rst_ni)
      ct_rram_dis_i != 4'h9 |=> $stable(ct_rram_dis_i));

  // ---- the OTP pages only, in the data partition ----
  A_otp_pages_only: assert property (@(posedge clk_i) disable iff (!rst_ni)
      ct_req_o && ct_start_o |-> ct_addr_o[20:9] >= 12'(FirstOtpPage) && ct_addr_o[20:9] <= 12'(LastOtpPage));
  A_data_partition: assert property (@(posedge clk_i) disable iff (!rst_ni)
      ct_req_o && ct_start_o |-> ct_part_o == RramPartData);
  A_op_defined: assert property (@(posedge clk_i) disable iff (!rst_ni)
      ct_req_o && ct_start_o |-> ct_op_o inside {RramOpRead, RramOpWrite, RramOpRewrite});
  A_write_whole_words: assert property (@(posedge clk_i) disable iff (!rst_ni)
      ct_req_o && ct_start_o && ct_op_o == RramOpWrite |-> (32'(ct_num_o) + 1) % 4 == 0);
  A_wdata_intg: assert property (@(posedge clk_i) disable iff (!rst_ni)
      ct_wvalid_o |-> ct_wdata_o == prim_secded_pkg::prim_secded_inv_39_32_enc(ct_wdata_o[31:0]));

  // ---- otp_ctrl's command protocol ----
  A_no_rsp_without_cmd: assert property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o |-> n_out != 0);
  A_err_code: assert property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o |-> ot_err_o inside {NoError, MacroError, MacroEccUncorrError, MacroWriteBlankError});
  A_no_rsp_before_accept_same_cycle: assert property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      n_out == 0 && accept |-> !ot_rvalid_o);

  // ---- disabled, undefined command ----
  A_disabled_err: assert property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && n_out != 0 && dis_now && q_cmd[0] != Init |-> ot_err_o != NoError);
  A_after_bad_cmd_err: assert property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && n_out != 0 && q_poison[0] |-> ot_err_o != NoError);

  // ---- fatal outputs ----
  A_intg_err_cause: assert property (@(posedge clk_i) disable iff (!rst_ni) ct_intg_err_o |-> bad_rd_seen);
  A_fatal_err_cause: assert property (@(posedge clk_i) disable iff (!rst_ni) ct_fatal_err_o |-> bad_cmd_seen);

  // ---- covers ----
  C_accept: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) accept && is_cmd(ot_cmd_i));
  C_last_word: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) accept && ot_addr_i == 11'd1279);
  C_dis_later: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      (ot_rvalid_o && ot_err_o == NoError) ##[1:$] (ot_rvalid_o && ct_rram_dis_i == MuBi4True));
  C_two_out: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni) n_out == 2);
  for (genvar c = 0; c < 6; c++) begin : g_cmd_rsp
    localparam cmd_e Cmd = c == 0 ? Read : c == 1 ? Write : c == 2 ? ReadRaw : c == 3 ? WriteRaw :
                           c == 4 ? Init : Zeroize;
    C_rsp_ok: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
        ot_rvalid_o && n_out != 0 && q_cmd[0] == Cmd && ot_err_o == NoError);
    C_rsp_err: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
        ot_rvalid_o && n_out != 0 && q_cmd[0] == Cmd && ot_err_o != NoError);
  end
  C_blank: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && ot_err_o == MacroWriteBlankError);
  C_ecc: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && ot_err_o == MacroEccUncorrError);
  C_disabled_rsp: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && n_out != 0 && dis_now && q_cmd[0] != Init);
  C_bad_cmd_rsp: cover property (@(posedge clk_otp_i) disable iff (!rst_otp_ni)
      ot_rvalid_o && n_out != 0 && q_poison[0]);
  C_write_op: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_req_o && ct_start_o && ct_op_o == RramOpWrite);
  C_rewrite_op: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_req_o && ct_start_o && ct_op_o == RramOpRewrite);
  C_intg_page: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_req_o && ct_start_o && ct_addr_o[20:9] == 12'(FirstOtpPage));
  C_last_page: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_req_o && ct_start_o && ct_addr_o[20:9] == 12'(LastOtpPage));
  C_intg_err: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_intg_err_o);
  C_fatal_err: cover property (@(posedge clk_i) disable iff (!rst_ni) ct_fatal_err_o);
endmodule

bind rram_ctrl_otp_unit rram_ctrl_otp_fpv u_fpv (.*);
