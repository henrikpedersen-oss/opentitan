// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_rom_ext_fault (eg2_top_verification spec REQ_PBF_14, REQ_SEC_14; software
// sw/device/tests/eg2_pbf/eg2_pbf_rom_ext_fault.c): a power-on reset while ROM_EXT writes the boot
// data a boot-service request changed. Runs behind secure boot (sw_test_mode_mask_rom, image flag
// rom_ext); without the assembled ROM_EXT image the bench does nothing (the software reports the
// test as not run behind secure boot).
//
// After the software's bench step 1 it resets itself; the bench waits for that reset, then for the
// RRAM controller's next programming (rram_ctrl phy_wr_busy: ROM_EXT writing its boot data), and
// `fault_delay` CPU cycles later applies a power-on reset (the POR_N pin, as
// chip_sw_full_aon_reset_vseq). A request ROM_EXT never writes for fails the run.
class chip_sw_eg2_pbf_rom_ext_fault_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_rom_ext_fault_vseq)
  `uvm_object_new

  rand bit        fault_point;  // 0 boot service, 1 ownership activation (a gap: runs 0)
  rand bit [31:0] fault_delay;  // CPU cycles from the start of the RRAM write to the reset

  constraint knobs_c {
    fault_delay dist {[0 : 50] :/ 40, [51 : 2000] :/ 40, [2001 : 50000] :/ 20};
  }

  // How long the bench waits for ROM_EXT's RRAM write after the reset.
  int unsigned write_timeout_us = 20000;

  virtual function void write_params();
    set_param("fault_point", fault_point);
    set_param("fault_delay", fault_delay);
  endfunction

  virtual task body();
    super.body();
    if (!cfg.rom_ext_assembled) begin
      `uvm_info(`gfn, "EG2_PBF rom_ext_fault: no ROM_EXT in this run, no fault", UVM_LOW)
      return;
    end
    // fault_delay is the bench's alone: drawn here even behind ROM_EXT (the image is signed).
    fork power_loss(pinned("fault_delay", fault_delay)); join_none
  endtask

  protected task power_loss(bit [31:0] delay);
    uvm_hdl_data_t busy;
    bit seen;
    wait_bench_step(1);
    wait (cfg.chip_vif.cpu_rst_n === 1'b0);
    wait (cfg.chip_vif.cpu_rst_n === 1'b1);
    fork
      begin
        do begin
          #100ns;
          void'(uvm_hdl_read({PdMain, ".u_rram_ctrl.phy_wr_busy"}, busy));
        end while (busy == 0);
        seen = 1'b1;
      end
      #(write_timeout_us * 1us);
    join_any
    disable fork;
    if (!seen) begin
      `uvm_error(`gfn, "EG2_PBF rom_ext_fault: no RRAM write by ROM_EXT after the request")
      return;
    end
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(delay);
    void'(announce_fault("chip_sw_eg2_pbf_rom_ext_fault_vseq", "insn",
                         $sformatf("power-on reset %0d cycles into ROM_EXT's RRAM write", delay)));
    apply_reset();
    cfg.sw_test_status_vif.sw_test_status = SwTestStatusUnderReset;
    `uvm_info(`gfn, "EG2_PBF rom_ext_fault: power-on reset applied", UVM_LOW)
  endtask
endclass
