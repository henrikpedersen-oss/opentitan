// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_keymgr_dpe_sideload_kmac_error (sw/device/tests/eg2_mem/
// eg2_keymgr_dpe_sideload_kmac_error.c; EG2 spec REQ_PBF_15). After `EG2 MEM kmac glitch armed`
// the sequence waits for KMAC's application interface FSM (kmac_app, u_app_intf) to leave its
// idle state for the key manager's generation, lets it run when_cycles cycles, then forces the sparse
// FSM's flop output to 0 (not one of its encodings, which are sparse and non-zero:
// PRIM_FLOP_SPARSE_FSM) for two cycles: KMAC's FSM error. After `EG2 MEM kmac error checked` it
// sets boot_phase = 1 and resets the chip, as the fault status stays until a reset.
class chip_sw_eg2_keymgr_dpe_sideload_kmac_error_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_keymgr_dpe_sideload_kmac_error_vseq)

  `uvm_object_new

  localparam string KmacApp = "tb.dut.top_earlgrey.earlgrey_pd_main.u_kmac.u_app_intf";
  localparam string AppSt = {KmacApp, ".st"};
  localparam string AppFlop = {KmacApp, ".u_state_regs.state_raw"};

  // When the glitch comes after the FSM leaves idle: at once, early, late in the digest.
  rand int unsigned when_cycles;
  constraint when_c {
    when_cycles dist {0 := 20, [1:20] :/ 40, [21:200] :/ 40};
  }

  virtual function void write_params();
    set_param("boot_phase", 0);
  endfunction

  virtual task body();
    `DV_CHECK_FATAL(uvm_hdl_check_path(AppSt), {"no KMAC app FSM at ", AppSt})
    `DV_CHECK_FATAL(uvm_hdl_check_path(AppFlop), {"no KMAC app FSM flop at ", AppFlop})
    super.body();
    fork
      glitch();
    join_none
  endtask

  protected task glitch();
    string line;
    uvm_hdl_data_t idle, st;
    wait_sw_line("EG2 MEM kmac glitch armed", line, 32'd4_000_000_000);
    `DV_CHECK_FATAL(uvm_hdl_read(AppSt, idle))
    do begin
      cfg.clk_rst_vif.wait_clks(1);
      `DV_CHECK_FATAL(uvm_hdl_read(AppSt, st))
    end while (st == idle);
    cfg.clk_rst_vif.wait_clks(when_cycles);
    announce("chip_sw_eg2_keymgr_dpe_sideload_kmac_error_vseq", "kmac,keymgr_dpe",
             $sformatf("KMAC app FSM forced to 0 %0d cycles after it left idle", when_cycles));
    `DV_CHECK_FATAL(uvm_hdl_force(AppFlop, 0))
    cfg.clk_rst_vif.wait_clks(2);
    `DV_CHECK_FATAL(uvm_hdl_release(AppFlop))
    @(cfg.sw_logger_vif.printed_log_event);
    wait_sw_line("EG2 MEM kmac error checked", line, 32'd4_000_000_000);
    set_param("boot_phase", 1);
    `uvm_info(`gfn, "EG2 KMAC error checked; power-on reset into the second boot", UVM_LOW)
    apply_reset();
  endtask

endclass : chip_sw_eg2_keymgr_dpe_sideload_kmac_error_vseq
