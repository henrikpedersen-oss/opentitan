// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Miter: the Bambu synthesis of dm_ar_semantic() (the documented effect) against what dm_mem's
// abstract-command program does (dm_ar_check). Cycle 0 resets both and latches a free command;
// dm_mem's words are read in cycles 1-11, Bambu runs from cycle 1, and cycle End compares.

module dm_ar_equiv (
  input             clock,
  input      [15:0] regno_i,
  input      [2:0]  aarsize_i,
  input             write_i,
  input             postinc_i,
  input             postexec_i,
  input             cheriot_i,
  input             restrict_i,
  output reg        bad,
  output reg        done_seen
);
  localparam [6:0] End = 7'd60;

  reg [6:0]  phase;
  reg [15:0] regno_q;
  reg [2:0]  aarsize_q;
  reg        write_q, postinc_q, postexec_q, cheriot_q, restrict_q;
  reg [31:0] exp_q;

  wire        reset = (phase == 7'd0);
  wire        start_port = (phase == 7'd1);
  wire        done_port;
  wire [31:0] hls_result;
  wire        words_valid, excluded, ok;

  dm_ar_semantic_hls u_hls (
    .clock      (clock),
    .reset      (!reset),  // Bambu resets on low
    .start_port (start_port),
    .done_port  (done_port),
    .return_port(hls_result),
    .regno      ({16'b0, regno_q}),
    .aarsize    ({29'b0, aarsize_q}),
    .write      ({31'b0, write_q}),
    .postinc    ({31'b0, postinc_q}),
    .postexec   ({31'b0, postexec_q}),
    .cheriot    ({31'b0, cheriot_q})
  );

  dm_ar_check u_rtl (
    .clk_i        (clock),
    .rst_ni       (!reset),
    .regno_i      (regno_q),
    .aarsize_i    (aarsize_q),
    .write_i      (write_q),
    .postinc_i    (postinc_q),
    .postexec_i   (postexec_q),
    .cheriot_i    (cheriot_q),
    .restrict_i   (restrict_q),
    .exp_i        (exp_q),
    .words_valid_o(words_valid),
    .excluded_o   (excluded),
    .ok_o         (ok)
  );

  always @(posedge clock) begin
    if (phase != End) phase <= phase + 7'd1;
    if (reset) begin
      regno_q    <= regno_i;
      aarsize_q  <= aarsize_i;
      write_q    <= write_i;
      postinc_q  <= postinc_i;
      postexec_q <= postexec_i;
      cheriot_q  <= cheriot_i;
      restrict_q <= restrict_i;
      exp_q      <= 32'b0;
      bad        <= 1'b0;
      done_seen  <= 1'b0;
    end else begin
      if (done_port && !done_seen) begin
        exp_q     <= hls_result;
        done_seen <= 1'b1;
      end
      if (phase == End - 7'd1) begin
        bad <= done_seen && words_valid && !excluded && !ok;
      end
    end
  end
endmodule
