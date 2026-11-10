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
