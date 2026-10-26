// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The EG2 console's DV channel (sw/device/lib/testing/eg2/eg2_result.c). With kEg2ConsoleDv set,
// the software writes each console character to SW_DV_EG2_CONSOLE_ADDR instead of UART0: bits 7:0
// the character, bits 11:8 the line's verbosity (eg2_verbosity_t: 0 NONE, 1 LOW, 2 MEDIUM,
// 3 HIGH, 4 DEBUG). Bound in tb.sv to the sim SRAM's interface, as sw_logger_if is: the sim SRAM
// takes the DV window's writes, so they never reach the RTL. chip_sw_eg2_base_vseq takes the
// words from `words` and prints each line in run.log.
interface eg2_dv_console_if #(
  parameter int AddrWidth = 32
) (
  input logic                 clk_i,
  input logic                 rst_ni,
  input logic                 wr_valid,  // A qualified full-word write to the sim SRAM.
  input logic [AddrWidth-1:0] addr,
  input logic [31:0]          data
);
  logic [AddrWidth-1:0] console_addr;

  // Every console word in order, until the vseq takes it.
  logic [31:0] words[$];

  always @(posedge clk_i) begin
    if (rst_ni && wr_valid && addr === console_addr) words.push_back(data);
  end
endinterface
