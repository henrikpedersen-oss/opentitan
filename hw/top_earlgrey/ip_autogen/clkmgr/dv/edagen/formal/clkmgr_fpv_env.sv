// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// clkmgr's FPV environment beyond the generated protocol checks (edagen clkmgr.yaml formal.files).
//
// Scan mode off: with scanmode_i at MuBi4True every clock gate is bypassed for DFT, which is not
// functional behaviour (clkmgr's documents do not describe scan mode). The block tests run with scan
// mode on and off (clkmgr_peri, clkmgr_trans, clkmgr_cmodel_random).
module clkmgr_fpv_env (
  input logic                  clk_i,
  input prim_mubi_pkg::mubi4_t scanmode_i
);
  ScanmodeOff_M: assume property (@(posedge clk_i) scanmode_i == prim_mubi_pkg::MuBi4False);
  // The assumption must leave functional behaviour reachable (not vacuous).
  ScanmodeOffReachable_C: cover property (@(posedge clk_i) scanmode_i == prim_mubi_pkg::MuBi4False);
endmodule

bind clkmgr clkmgr_fpv_env u_fpv_env (.clk_i, .scanmode_i);
