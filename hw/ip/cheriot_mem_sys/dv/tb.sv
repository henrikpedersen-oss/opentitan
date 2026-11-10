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

  `DV_ALERT_IF_CONNECT()

  // The core on the core data port, with its tag sideband and the mode; TL-UL hosts on the core's
  // revocation bitmap port and the bitmap window; TL-UL devices on the host ports towards the main
  // crossbar (core, revocation engine) and the meta SRAM.
  cheriot_mem_sys_core_if core_if(.clk(clk), .rst_n(rst_n));
  tl_if corerevbm_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if revbm_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if cored_h_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if tbre_h_tl_if(.clk(clk), .rst_n(rst_n));
  tl_if meta_tl_if(.clk(clk), .rst_n(rst_n));

  // dut
  // The feature tests use a small address map (16 KiB SRAM, 8 KiB NVM; build mode cms_map), as
  // they fill and sweep whole regions; Earl Grey 2's otherwise.
`ifdef CHERIOT_MEM_SYS_CMS_MAP
  cheriot_mem_sys #(
    .MainSramBaseAddr(MainSramBase),
    .MainSramTopAddr (MainSramTop),
    .NvmBaseAddr     (NvmBase),
    .NvmTopAddr      (NvmTop),
    .MetaSramBaseAddr(MetaBase),
    .MemSizeRevbm    (RevbmBytes)
  ) dut (
`else
  cheriot_mem_sys dut (
`endif
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
    .cheriot_ena_i        (core_if.cheriot_ena),

    // Core data port with its capability tag sideband
    .cored_tl_d_i         (core_if.h2d),
    .cored_tag_h2d_i      (core_if.tag_h2d),
    .cored_tl_d_o         (core_if.d2h),
    .cored_tag_d2h_o      (core_if.tag_d2h),

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

  // The WTRC's assertions describe what the core does; a test that drives something the core
  // cannot produce turns them off around it (cheriot_mem_sys_base_vseq::wtrc_assertions).
  always @(core_if.wtrc_assert_en) begin
    if (core_if.wtrc_assert_en) $asserton(0, dut.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc);
    else                        $assertoff(0, dut.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc);
  end

  initial begin
    // drive clk and rst_n from clk_if
    clk_rst_if.set_active();
    uvm_config_db#(virtual clk_rst_if)::set(null, "*.env", "clk_rst_vif", clk_rst_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_tl_agent*", "vif", tl_if);
    uvm_config_db#(intr_vif)::set(null, "*.env", "intr_vif", intr_if);
    uvm_config_db#(virtual cheriot_mem_sys_core_if)::set(null, "*.env.m_core_agent*", "vif",
                                                         core_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_corerevbm_agent*", "vif", corerevbm_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_revbm_agent*", "vif", revbm_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_cored_h_agent*", "vif", cored_h_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_tbre_h_agent*", "vif", tbre_h_tl_if);
    uvm_config_db#(virtual tl_if)::set(null, "*.env.m_meta_agent*", "vif", meta_tl_if);
    $timeformat(-12, 0, " ps", 12);
    run_test();
  end

endmodule
