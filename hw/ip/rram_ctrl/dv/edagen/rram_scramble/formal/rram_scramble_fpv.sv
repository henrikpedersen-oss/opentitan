// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// FPV of rram_ctrl's scrambling unit (edagen rram_scramble.yaml formal.files), bound into
// rram_scramble_unit. From theory_of_operation.md "RRAM Scrambling" and prim_prince.md:
//   - descramble(scramble(x)) == x: requester 0's next cipher request after a scramble is made the
//     descramble of its result (an assumption on the free inputs), which must give x back;
//   - the result does not depend on cipher_switch ("identical, interchangeable engines"): a second
//     rram_scramble with every cipher_switch inverted runs in lockstep and must agree;
//   - the mask is GF_MULT(word_addr, addr_key): mask(0) = 0, mask(1) = addr_key, mask(2) =
//     addr_key * x, and the whole product against a reference multiplier (field and bit order as
//     the reference model takes them, eg2_rram_scramble_core.h);
//   - the key registers: reset values, keys_valid, keys_disable (any value but MuBi4False) wins;
//   - the arbiters: an acknowledgement only for a request, never two requesters at once, every
//     request acknowledged (liveness), arb_err never.
// Assumptions: the requesters hold a request and its data until acknowledged (rram_phy's read and
// write paths); the key registers do not change while a request is open (rram_ctrl latches the
// keys before operations start; an operation across a key change is not documented). Each has a
// cover.
module rram_scramble_fpv
  import rram_ctrl_pkg::*;
(
  input logic         clk_i,
  input logic         rst_ni,
  input logic [3:0]   sc_keys_disable_i,
  input logic         sc_keys_valid_i,
  input logic [127:0] sc_addr_key_i,
  input logic [127:0] sc_data_key_i,
  input logic [127:0] sc_rand_addr_key_i,
  input logic [127:0] sc_rand_data_key_i,
  input logic [1:0]   sc_calc_req_i,
  input logic [1:0]   sc_op_req_i,
  input logic [1:0]   sc_op_type_i,
  input logic [1:0]   sc_cipher_switch_i,
  input logic [16:0]  sc_addr0_i,
  input logic [16:0]  sc_addr1_i,
  input logic [127:0] sc_data0_i,
  input logic [127:0] sc_data1_i,
  input logic [1:0]   sc_calc_ack_o,
  input logic [1:0]   sc_op_ack_o,
  input logic [127:0] sc_mask0_o,
  input logic [127:0] sc_mask1_o,
  input logic [127:0] sc_dout0_o,
  input logic [127:0] sc_dout1_o,
  input logic         sc_arb_err_o
);
  localparam logic [3:0] F = 4'h9;

  function automatic logic [127:0] xtime(logic [127:0] a);
    return {a[126:0], 1'b0} ^ (a[127] ? 128'h87 : 128'h0);
  endfunction
  function automatic logic [127:0] gf_mul(logic [127:0] a, logic [16:0] b);
    logic [127:0] r = '0;
    for (int i = 0; i < 17; i++) begin
      if (b[i]) r ^= a;
      a = xtime(a);
    end
    return r;
  endfunction

  logic [127:0] akey, dkey;
  logic rt_armed;  // the round trip below: a scramble of requester 0 waits for its descramble
  assign akey = rram_scramble_unit.u_scramble.addr_key_q;
  assign dkey = rram_scramble_unit.u_scramble.data_key_q;

  default clocking @(posedge clk_i); endclocking
  default disable iff (!rst_ni);

  // ---- Assumptions ----
  for (genvar k = 0; k < 2; k++) begin : gen_req
    logic [16:0]  a;
    logic [127:0] d;
    assign a = k ? sc_addr1_i : sc_addr0_i;
    assign d = k ? sc_data1_i : sc_data0_i;
    CalcHold_M: assume property (sc_calc_req_i[k] && !sc_calc_ack_o[k] |=> sc_calc_req_i[k] && $stable(a));
    OpHold_M: assume property (sc_op_req_i[k] && !sc_op_ack_o[k] |=>
                               sc_op_req_i[k] && $stable(d) && $stable(sc_op_type_i[k]) &&
                               $stable(sc_cipher_switch_i[k]));
    // ---- Arbiters ----
    CalcAckNeedsReq_A: assert property (sc_calc_ack_o[k] |-> sc_calc_req_i[k]);
    OpAckNeedsReq_A: assert property (sc_op_ack_o[k] |-> sc_op_req_i[k]);
    CalcServed_A: assert property (sc_calc_req_i[k] |-> s_eventually sc_calc_ack_o[k]);
    OpServed_A: assert property (sc_op_req_i[k] |-> s_eventually sc_op_ack_o[k]);
    // ---- The mask ----
    MaskZero_A: assert property (sc_calc_ack_o[k] && a == '0 |-> (k ? sc_mask1_o : sc_mask0_o) == '0);
    MaskOne_A: assert property (sc_calc_ack_o[k] && a == 17'd1 |-> (k ? sc_mask1_o : sc_mask0_o) == akey);
    MaskTwo_A: assert property (sc_calc_ack_o[k] && a == 17'd2 |-> (k ? sc_mask1_o : sc_mask0_o) == xtime(akey));
    MaskProduct_A: assert property (sc_calc_ack_o[k] |-> (k ? sc_mask1_o : sc_mask0_o) == gf_mul(akey, a));
    MaskReachable_C: cover property (sc_calc_ack_o[k] && a != '0);
  end
  // The key registers do not change while a request is open, nor between a scramble and the
  // descramble of its result (the round trip below). Stated on the registers, not on the inputs:
  // a request in the first cycle after reset with keys_disable already not MuBi4False loads the
  // random keys under a running mask (counterexample to MaskOne_A, MaskTwo_A, MaskProduct_A on the
  // input-only form, 2026-11-07; the mask then uses the key of its first cycle, which the
  // documentation does not decide). With other keys the round trip does not return x
  // (counterexample to RoundTrip_A).
  KeysStableWhileBusy_M: assume property (sc_calc_req_i != '0 || sc_op_req_i != '0 || rt_armed |=>
                                          $stable(akey) && $stable(dkey));
  BothCalcReachable_C: cover property (sc_calc_req_i == 2'b11);
  BothOpReachable_C: cover property (sc_op_req_i == 2'b11 ##[1:$] sc_op_ack_o[1]);
  KeyLoadReachable_C: cover property (sc_keys_valid_i ##[1:$] sc_op_ack_o[0]);

  OneCalcAtATime_A: assert property (sc_calc_ack_o != 2'b11);
  OneOpAtATime_A: assert property (sc_op_ack_o != 2'b11);
  NoArbErr_A: assert property (!sc_arb_err_o);

  // ---- Key registers ----
  // The first cycle after reset: the registers still hold their reset values.
  logic after_first;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) after_first <= 1'b0;
    else after_first <= 1'b1;
  end
  KeyReset_A: assert property (!after_first |-> akey == RndCnstAddrKeyDefault && dkey == RndCnstDataKeyDefault);
  KeyDisable_A: assert property (sc_keys_disable_i != F |=> akey == $past(sc_rand_addr_key_i) &&
                                                             dkey == $past(sc_rand_data_key_i));
  KeyLoad_A: assert property (sc_keys_disable_i == F && sc_keys_valid_i |=> akey == $past(sc_addr_key_i) &&
                                                                            dkey == $past(sc_data_key_i));
  KeyHold_A: assert property (sc_keys_disable_i == F && !sc_keys_valid_i |=> $stable(akey) && $stable(dkey));

  // ---- descramble(scramble(x)) == x, requester 0 ----
  logic [127:0] rt_x, rt_y;
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rt_armed <= 1'b0;
      rt_x <= '0;
      rt_y <= '0;
    end else if (sc_op_ack_o[0]) begin
      rt_armed <= !rt_armed && sc_op_type_i[0] == ScrambleOp;
      rt_x <= sc_data0_i;
      rt_y <= sc_dout0_o;
    end
  end
  RoundTripStimulus_M: assume property (rt_armed && sc_op_req_i[0] |->
                                        sc_op_type_i[0] == DeScrambleOp && sc_data0_i == rt_y);
  RoundTrip_A: assert property (rt_armed && sc_op_ack_o[0] |-> sc_dout0_o == rt_x);
  RoundTripReachable_C: cover property (rt_armed && sc_op_ack_o[0]);

  // ---- cipher_switch does not change the result ----
  scramble_req_t [1:0] req_sw;
  scramble_rsp_t [1:0] rsp_sw;
  assign req_sw[0] = '{calc_req: sc_calc_req_i[0], op_req: sc_op_req_i[0],
                       op_type: cipher_ops_e'(sc_op_type_i[0]), cipher_switch: !sc_cipher_switch_i[0],
                       addr: sc_addr0_i, data_in: sc_data0_i};
  assign req_sw[1] = '{calc_req: sc_calc_req_i[1], op_req: sc_op_req_i[1],
                       op_type: cipher_ops_e'(sc_op_type_i[1]), cipher_switch: !sc_cipher_switch_i[1],
                       addr: sc_addr1_i, data_in: sc_data1_i};
  rram_scramble u_switched (
    .clk_i,
    .rst_ni,
    .keys_disable_i (prim_mubi_pkg::mubi4_t'(sc_keys_disable_i)),
    .keys_valid_i   (sc_keys_valid_i),
    .addr_key_i     (sc_addr_key_i),
    .data_key_i     (sc_data_key_i),
    .rand_addr_key_i(sc_rand_addr_key_i),
    .rand_data_key_i(sc_rand_data_key_i),
    .scramble_req_i (req_sw),
    .scramble_rsp_o (rsp_sw),
    .arb_err_o      ()
  );
  SwitchSameAck_A: assert property (sc_op_ack_o == {rsp_sw[1].op_ack, rsp_sw[0].op_ack});
  SwitchSameResult0_A: assert property (sc_op_ack_o[0] |-> sc_dout0_o == rsp_sw[0].data_out);
  SwitchSameResult1_A: assert property (sc_op_ack_o[1] |-> sc_dout1_o == rsp_sw[1].data_out);
endmodule

bind rram_scramble_unit rram_scramble_fpv u_fpv (.*);
