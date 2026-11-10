// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
module tb;
  // dep packages
  import uvm_pkg::*;
  import dv_utils_pkg::*;
  import cheriot_mem_sys_env_pkg::*;
  import cheriot_mem_sys_test_pkg::*;

  // macro includes
  `include "uvm_macros.svh"
  `include "dv_macros.svh"

  wire clk, rst_n;
  wire [NUM_MAX_INTERRUPTS-1:0] interrupts;

  // interfaces
  clk_rst_if clk_rst_if(.clk(clk), .rst_n(rst_n));
  tl_if tl_if(.clk(clk), .rst_n(rst_n));
  pins_if #(NUM_MAX_INTERRUPTS) intr_if(.pins(interrupts));

  // The memory side: hosts on the core data port (with its tag sideband), the core's revocation
  // bitmap port and the bitmap window; devices on the host ports towards the main crossbar (core,
  // revocation engine) and the meta SRAM.
  tl_if cored_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if corerevbm_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if revbm_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if cored_h_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if tbre_h_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if meta_tl_if(.clk(clk), .rst_n(rst_n));
  logic cored_tag_d2h;
  cheriot_mem_sys_side_if side_if(.clk(clk), .rst_n(rst_n), .h2d(cored_tl_if.h2d),
                                  .d2h(cored_tl_if.d2h), .tag_d2h(cored_tag_d2h));

  `DV_ALERT_IF_CONNECT()

  // dut
  cheriot_mem_sys dut (
    .clk_i                (clk      ),
    .rst_ni               (rst_n    ),

    // Bus interface (device) for CSRs
    .regs_tl_d_i          (tl_if.h2d),
    .regs_tl_d_o          (tl_if.d2h),

    // Interrupts
    .intr_tbre_done_o     (interrupts[0]),

    // Alerts
    .alert_rx_i           (alert_rx ),
    .alert_tx_o           (alert_tx ),

    // Execution mode
    .cheriot_ena_i        (side_if.cheriot_ena),

    // Core data port with its capability tag sideband
    .cored_tl_d_i         (cored_tl_if.h2d),
    .cored_tag_h2d_i      (side_if.tag_h2d),
    .cored_tl_d_o         (cored_tl_if.d2h),
    .cored_tag_d2h_o      (cored_tag_d2h),

    // Core revocation bitmap port
    .corerevbm_tl_i       (corerevbm_tl_if.h2d),
    .corerevbm_tl_o       (corerevbm_tl_if.d2h),

    // System revocation bitmap window
    .revbm_tl_d_i         (revbm_tl_if.h2d),
    .revbm_tl_d_o         (revbm_tl_if.d2h),

    // Host port towards the interconnect
    .cored_tl_h_o         (cored_h_tl_if.h2d),
    .cored_tl_h_i         (cored_h_tl_if.d2h),
    .tbre_tl_h_o          (tbre_h_tl_if.h2d),
    .tbre_tl_h_i          (tbre_h_tl_if.d2h),

    // Host port towards the meta SRAM
    .meta_sram_tl_o       (meta_tl_if.h2d),
    .meta_sram_tl_i       (meta_tl_if.d2h)
  );

  // Which cause set TBRE_STATUS.sweep_err (rare; printed for the C-model scoreboard's reports).
  always @(posedge clk) begin
    if (rst_n && dut.tbre_sweep_err) begin
      $display("%0t tb: sweep_err set: mover_read_err %0b mover_err %0b revbm_intg %0b revbm_data_intg %0b revbm_dev %0b rmw_data_intg %0b rmw_rsp_intg %0b rmw_dev %0b (tbre_active %0b)",
               $time, dut.tbre_mover_read_err, dut.tbre_mover_err, dut.tbre_revbm_intg_err,
               dut.tbre_revbm_data_intg_err, dut.tbre_revbm_dev_err, dut.rmw_data_intg_err,
               dut.rmw_rsp_intg_err, dut.rmw_dev_err, dut.tbre_active);
    end
  end

  // The response the RMW filter flags with a data integrity error (diagnostic).
  always @(posedge clk) begin
    if (rst_n && dut.rmw_data_intg_err) begin
      $display("%0t tb: rmw data_intg error on a meta response: d_opcode %0d d_size %0d d_source %0h d_error %0b d_data %08x data_intg %02x (expected %02x); meta d2h valid %0b",
               $time, dut.rmw_tl_d2h.d_opcode, dut.rmw_tl_d2h.d_size, dut.rmw_tl_d2h.d_source,
               dut.rmw_tl_d2h.d_error, dut.rmw_tl_d2h.d_data, dut.rmw_tl_d2h.d_user.data_intg,
               tlul_pkg::get_data_intg(dut.rmw_tl_d2h.d_data), meta_tl_if.d2h.d_valid);
    end
  end

  initial begin
    // drive clk and rst_n from clk_if
    clk_rst_if.set_active();
    uvm_config_db#(virtual clk_rst_if)::set(null, "*.env", "clk_rst_vif", clk_rst_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_tl_agent*", "vif", tl_if);
    uvm_config_db#(intr_vif)::set(null, "*.env", "intr_vif", intr_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_cored_agent*", "vif", cored_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_corerevbm_agent*", "vif", corerevbm_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_revbm_agent*", "vif", revbm_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_cored_h_agent*", "vif", cored_h_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_tbre_h_agent*", "vif", tbre_h_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_meta_agent*", "vif", meta_tl_if);
    uvm_config_db#(virtual cheriot_mem_sys_side_if)::set(null, "*.env", "side_vif", side_if);
    $timeformat(-12, 0, " ps", 12);
    run_test();
  end

endmodule
