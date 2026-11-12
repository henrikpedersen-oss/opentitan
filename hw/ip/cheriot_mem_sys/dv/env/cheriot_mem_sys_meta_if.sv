// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Debugging taps only: +cms_meta_trace prints what the DUT routed with uvm_info.
// No expected value, requester ownership or check may depend on this interface.
interface cheriot_mem_sys_meta_if(input logic clk, input logic rst_n);
  tlul_pkg::tl_h2d_t tag_h2d[2], rmw_h2d, branches_h2d[4], meta_h2d;
  tlul_pkg::tl_d2h_t tag_d2h[2], rmw_d2h, branches_d2h[4], meta_d2h;
  clocking mon_cb @(posedge clk);
    input rst_n;
    input tag_h2d, tag_d2h, rmw_h2d, rmw_d2h;
    input branches_h2d, branches_d2h, meta_h2d, meta_d2h;
  endclocking
endinterface
