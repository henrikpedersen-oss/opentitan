// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Miter of the C-to-RTL equivalence of rram_ctrl's scrambling mask (equiv.ys): Bambu's core
// rram_gf_mask_hls (from rram_gf_mask_hls.c) against rram_scramble itself. The key reaches
// rram_scramble's key register through keys_disable (MuBi4True loads rand_addr_key every cycle);
// requester 0 asks for the mask of addr and holds the request until acknowledged. bad: the HLS word
// differs from the RTL mask's word; done_seen: both answered (the second proof, so that a core that
// never finishes cannot pass the first).
module rram_gf_mask_equiv (
  input          clock,
  input  [127:0] key_i,
  input  [16:0]  addr_i,
  input  [1:0]   word_i,
  output reg     bad,
  output reg     done_seen
);
  reg [8:0]   phase;
  reg [127:0] key_hold;
  reg [16:0]  addr_hold;
  reg [1:0]   word_hold;
  reg         hls_done, rtl_done;
  reg [31:0]  hls_word;
  reg [127:0] rtl_mask;

  wire reset = (phase == 0);
  wire start_port = (phase == 2);
  wire done_port;
  wire [31:0] hls_data_o;
  wire [1:0] calc_ack;
  wire [127:0] mask0;

  rram_gf_mask_hls u_hls (
    .clock      (clock),
    .reset      (!reset),  // Bambu resets on low
    .start_port (start_port),
    .done_port  (done_port),
    .return_port(hls_data_o),
    .k0         (key_hold[31:0]),
    .k1         (key_hold[63:32]),
    .k2         (key_hold[95:64]),
    .k3         (key_hold[127:96]),
    .addr       ({15'b0, addr_hold}),
    .word       ({6'b0, word_hold})
  );

  rram_scramble_unit u_rtl (
    .clk_i             (clock),
    .rst_ni            (!reset),
    .sc_keys_disable_i (4'h6),
    .sc_keys_valid_i   (1'b0),
    .sc_addr_key_i     (128'h0),
    .sc_data_key_i     (128'h0),
    .sc_rand_addr_key_i(key_hold),
    .sc_rand_data_key_i(128'h0),
    .sc_calc_req_i     ({1'b0, phase >= 2 && !rtl_done}),
    .sc_op_req_i       (2'b00),
    .sc_op_type_i      (2'b00),
    .sc_cipher_switch_i(2'b00),
    .sc_addr0_i        (addr_hold),
    .sc_addr1_i        (17'h0),
    .sc_data0_i        (128'h0),
    .sc_data1_i        (128'h0),
    .sc_calc_ack_o     (calc_ack),
    .sc_op_ack_o       (),
    .sc_mask0_o        (mask0),
    .sc_mask1_o        (),
    .sc_dout0_o        (),
    .sc_dout1_o        (),
    .sc_arb_err_o      ()
  );

  always @(posedge clock) begin
    if (phase != 9'h1ff) phase <= phase + 1'b1;
    if (reset) begin
      key_hold <= key_i;
      addr_hold <= addr_i;
      word_hold <= word_i;
      bad <= 1'b0;
      done_seen <= 1'b0;
      hls_done <= 1'b0;
      rtl_done <= 1'b0;
    end else begin
      if (done_port && !hls_done) begin
        hls_done <= 1'b1;
        hls_word <= hls_data_o;
      end
      if (calc_ack[0] && phase >= 2 && !rtl_done) begin
        rtl_done <= 1'b1;
        rtl_mask <= mask0;
      end
      if (hls_done && rtl_done) begin
        bad <= bad | (hls_word != rtl_mask[32 * word_hold +: 32]);
        done_seen <= 1'b1;
      end
    end
  end
endmodule
