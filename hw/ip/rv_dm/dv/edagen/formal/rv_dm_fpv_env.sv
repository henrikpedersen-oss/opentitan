// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rv_dm's FPV environment beyond the generated protocol checks (edagen rv_dm.yaml formal.files).
//
// Scan mode off. With scanmode_i at MuBi4True rv_dm's resets come from scan_rst_ni (DFT), which the
// formal reset sequence does not drive, so state such as PULP dm_csrs' response FIFO starts out of
// reset and its assertions fail in the first cycle (depthShallNotExceedParamDepth,
// OnlyRvalidWhenNotUnderRst_A, 2026-11-07). Scan mode is a test mode: its behaviour is checked by the
// block test rv_dm_scanmode (REQ_DCT_27), not by the functional proofs.
module rv_dm_fpv_env (
  input logic                  clk_i,
  input prim_mubi_pkg::mubi4_t scanmode_i
);
  ScanmodeOff_M: assume property (@(posedge clk_i) scanmode_i == prim_mubi_pkg::MuBi4False);
  // The assumption must leave functional behaviour reachable (not vacuous).
  ScanmodeOffReachable_C: cover property (@(posedge clk_i) scanmode_i == prim_mubi_pkg::MuBi4False);
endmodule

bind rv_dm rv_dm_fpv_env u_fpv_env (.clk_i, .scanmode_i);
