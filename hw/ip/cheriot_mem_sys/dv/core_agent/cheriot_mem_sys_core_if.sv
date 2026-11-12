// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The core's side of the memory subsystem: the core data port (TL-UL), the capability tag that
// travels with each request and each response, and the execution mode.
interface cheriot_mem_sys_core_if (input logic clk, input logic rst_n);

  tlul_pkg::tl_h2d_t     h2d;          // cored_tl_d_i
  tlul_pkg::tl_d2h_t     d2h;          // cored_tl_d_o
  logic                  tag_h2d;      // cored_tag_h2d_i: store tag, or capability-load hint
  logic                  tag_d2h;      // cored_tag_d2h_o: tag of a load's response
  prim_mubi_pkg::mubi4_t cheriot_ena;  // cheriot_ena_i

  // The WTRC's assertions describe what the core does on this port. A test that drives something
  // the core cannot produce turns them off around it (tb.sv).
  bit wtrc_assert_en = 1'b1;

  // Count only handshakes at this external port. These checks never read DUT state.
  int unsigned outstanding;
  wire a_fire = h2d.a_valid && d2h.a_ready;
  wire d_fire = d2h.d_valid && h2d.d_ready;
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) outstanding <= 0;
    else case ({a_fire, d_fire})
      2'b10: outstanding <= outstanding + 1;
      2'b01: outstanding <= outstanding - 1;
      default: ;
    endcase
  end

  // Tag Filter/interface contract: the sideband belongs to the stalled request.
  CoreTagStable_A: assert property (@(posedge clk) disable iff (!rst_n)
      h2d.a_valid && !d2h.a_ready |=> $stable(tag_h2d))
    else $error("CoreTagStable_A: request tag changed under backpressure");
  CoreResponseTagKnown_A: assert property (@(posedge clk) disable iff (!rst_n)
      d2h.d_valid |-> !$isunknown(tag_d2h))
    else $error("CoreResponseTagKnown_A: response tag is unknown");
  CoreModeKnown_A: assert property (@(posedge clk) !$isunknown(cheriot_ena))
    else $error("CoreModeKnown_A: execution mode is unknown");

  // BENCH ASSUMPTION A6 (spec Q8/Q48 remain open): stimulus keeps the mode fixed
  // from presentation through the last response. Assert the assumption in simulation;
  // this is a testbench obligation, not a claim that the RTL handles mode switches.
  CoreModeStable_BenchAssumption_A: assert property (@(posedge clk) disable iff (!rst_n)
      h2d.a_valid || outstanding != 0 |=> $stable(cheriot_ena))
    else $error("CoreModeStable_BenchAssumption_A: A6 violated by stimulus");

  // Non-vacuity witnesses accompany assertion coverage in the coverage database.
  CoreTagStall_C: cover property (@(posedge clk) disable iff (!rst_n)
      h2d.a_valid && !d2h.a_ready ##1 h2d.a_valid && d2h.a_ready);
  CoreResponse_C: cover property (@(posedge clk) disable iff (!rst_n) d2h.d_valid);
  CoreOutstanding_C: cover property (@(posedge clk) disable iff (!rst_n) outstanding != 0);

  clocking host_cb @(posedge clk);
    output h2d;
    output tag_h2d;
    input  d2h;
    input  tag_d2h;
  endclocking

  clocking mon_cb @(posedge clk);
    input h2d;
    input d2h;
    input tag_h2d;
    input tag_d2h;
    input cheriot_ena;
  endclocking

  initial begin
    h2d         = '0;
    tag_h2d     = 1'b0;
    cheriot_ena = prim_mubi_pkg::MuBi4False;
  end

endinterface
