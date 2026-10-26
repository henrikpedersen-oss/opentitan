// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 noise source (eg2_top_verification spec "Random Number Generation Test Suite"):
// +eg2_rng=host|model|pattern:<name>|file:<path> (dv/eg2_rng/eg2_rng_source.h). Included in tb.sv
// under EG2_RNG only (build mode en_eg2_rng); with +eg2_rng absent or `model` it forces nothing
// and AST's behavioural model drives entropy_src as before.
//
// The symbols replace what entropy_src takes from its noise input, inside the consumer (the
// mutation library's rule: never force a port, the simulator merges it with the driver's net):
// sfifo_esrng_wdata, the data entropy_src_core pushes into its esrng FIFO whenever AST's
// rng_valid pulses and the block is enabled. Each push takes the current symbol, and the next one
// is drawn on that edge, so every sample is a new symbol. AST's valid timing is unchanged.
// A test's vseq may select another source at run time (eg2_rng_select): a health-test pattern
// that starts when the vseq says so.

import "DPI-C" function int eg2_rng_select(input string spec);
import "DPI-C" function int eg2_rng_active();
import "DPI-C" function int eg2_rng_symbol();
import "DPI-C" function longint eg2_rng_count();
import "DPI-C" function void eg2_rng_finish();

`define EG2_RNG_CORE `PD_MAIN_HIER.u_entropy_src.u_entropy_src_core

logic [3:0] eg2_rng_sym;
bit         eg2_rng_forced;

initial begin
  string src;
  if ($value$plusargs("eg2_rng=%s", src) && src != "model") begin
    if (eg2_rng_select(src) != 0) $fatal(1, "EG2_RNG: bad +eg2_rng=%s", src);
  end
end

// Force while a source is selected; release when a vseq selects `model` again.
always @(posedge `EG2_RNG_CORE.clk_i) begin
  if (eg2_rng_active() != 0) begin
    if (!eg2_rng_forced) begin
      int s;
      s = eg2_rng_symbol();
      eg2_rng_sym = s < 0 ? 4'h0 : s[3:0];
      force `EG2_RNG_CORE.sfifo_esrng_wdata = eg2_rng_sym;
      eg2_rng_forced = 1'b1;
    end else if (`EG2_RNG_CORE.sfifo_esrng_push === 1'b1) begin
      int s;
      s = eg2_rng_symbol();
      if (s < 0) $fatal(1, "EG2_RNG: the recorded stream ended");
      eg2_rng_sym <= s[3:0];
    end
  end else if (eg2_rng_forced) begin
    release `EG2_RNG_CORE.sfifo_esrng_wdata;
    eg2_rng_forced = 1'b0;
  end
end

final begin
  eg2_rng_finish();
end

`undef EG2_RNG_CORE
