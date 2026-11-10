// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rram_scramble with the parameters rram_phy gives it (the defaults: the package's reset keys,
// SecScrambleEn, Interleave). Bench wrapper only (edagen rram_scramble.yaml): it flattens the two
// requesters' request and response structs into vectors, every port prefixed sc_ for edagen's
// signal-level agent (requester 0 at index 0 / the ...0 ports, requester 1 at index 1). No logic of
// its own.
module rram_scramble_unit
  import rram_ctrl_pkg::*;
(
  input  logic         clk_i,
  input  logic         rst_ni,
  input  logic [3:0]   sc_keys_disable_i,
  input  logic         sc_keys_valid_i,
  input  logic [127:0] sc_addr_key_i,
  input  logic [127:0] sc_data_key_i,
  input  logic [127:0] sc_rand_addr_key_i,
  input  logic [127:0] sc_rand_data_key_i,
  input  logic [1:0]   sc_calc_req_i,
  input  logic [1:0]   sc_op_req_i,
  input  logic [1:0]   sc_op_type_i,       // 1: descramble (DeScrambleOp)
  input  logic [1:0]   sc_cipher_switch_i,
  input  logic [16:0]  sc_addr0_i,
  input  logic [16:0]  sc_addr1_i,
  input  logic [127:0] sc_data0_i,
  input  logic [127:0] sc_data1_i,
  output logic [1:0]   sc_calc_ack_o,
  output logic [1:0]   sc_op_ack_o,
  output logic [127:0] sc_mask0_o,
  output logic [127:0] sc_mask1_o,
  output logic [127:0] sc_dout0_o,
  output logic [127:0] sc_dout1_o,
  output logic         sc_arb_err_o
);

  scramble_req_t [1:0] req;
  scramble_rsp_t [1:0] rsp;

  assign req[0] = '{calc_req:      sc_calc_req_i[0],
                    op_req:        sc_op_req_i[0],
                    op_type:       cipher_ops_e'(sc_op_type_i[0]),
                    cipher_switch: sc_cipher_switch_i[0],
                    addr:          sc_addr0_i,
                    data_in:       sc_data0_i};
  assign req[1] = '{calc_req:      sc_calc_req_i[1],
                    op_req:        sc_op_req_i[1],
                    op_type:       cipher_ops_e'(sc_op_type_i[1]),
                    cipher_switch: sc_cipher_switch_i[1],
                    addr:          sc_addr1_i,
                    data_in:       sc_data1_i};

  rram_scramble u_scramble (
    .clk_i,
    .rst_ni,
    .keys_disable_i (prim_mubi_pkg::mubi4_t'(sc_keys_disable_i)),
    .keys_valid_i   (sc_keys_valid_i),
    .addr_key_i     (sc_addr_key_i),
    .data_key_i     (sc_data_key_i),
    .rand_addr_key_i(sc_rand_addr_key_i),
    .rand_data_key_i(sc_rand_data_key_i),
    .scramble_req_i (req),
    .scramble_rsp_o (rsp),
    .arb_err_o      (sc_arb_err_o)
  );

  assign sc_calc_ack_o = {rsp[1].calc_ack, rsp[0].calc_ack};
  assign sc_op_ack_o   = {rsp[1].op_ack, rsp[0].op_ack};
  assign sc_mask0_o    = rsp[0].mask;
  assign sc_mask1_o    = rsp[1].mask;
  assign sc_dout0_o    = rsp[0].data_out;
  assign sc_dout1_o    = rsp[1].data_out;

endmodule
