// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The CHERIoT services after boot (eg2_top_verification spec REQ_PBF_22: chip_sw_eg2_pbf_cheriot_sb
// and chip_sw_eg2_pbf_cheriot_fault; software sw/device/tests/eg2_pbf/eg2_pbf_cheriot_*.c,
// CHERIoT builds only).

// chip_sw_eg2_pbf_cheriot_sb: allocation, free with revocation, reuse.
class chip_sw_eg2_pbf_cheriot_sb_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_cheriot_sb_vseq)
  `uvm_object_new

  rand bit [1:0]  size_class;
  rand bit [31:0] num_allocs;
  rand bit [1:0]  free_order;
  rand bit        sweep_when;  // 0 before the reuse, 1 during it

  constraint knobs_c {
    num_allocs inside {[2 : 16]};
    free_order inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("size_class", size_class);
    set_param("num_allocs", num_allocs);
    set_param("free_order", free_order);
    set_param("sweep_when", sweep_when);
  endfunction

  virtual task body();
    add_activity("revocation engine sweeping", {PdMain, ".u_cheriot_mem_sys.tbre_active"});
    super.body();
  endtask
endclass

// chip_sw_eg2_pbf_cheriot_fault: an escalation or a watchdog bite during a sweep or a compartment
// call. The bench's own check: the chip is back in RISC-V mode when the core leaves the fault's
// reset (rv_core_ibex theory of operation, "Execution Mode Switch": the write-once switch selects
// CHERIoT mode only once written, so a reset returns the chip to ePMP mode): cheriot_ena is not
// MuBi4True at that moment.
class chip_sw_eg2_pbf_cheriot_fault_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_cheriot_fault_vseq)
  `uvm_object_new

  rand bit        fault_point;     // 0 during a sweep, 1 during a compartment call
  rand bit        fault;           // 0 escalation, 1 watchdog bite
  rand bit [31:0] sweep_progress;  // 0-100 %

  constraint knobs_c {
    fault dist {0 :/ 70, 1 :/ 30};
    sweep_progress inside {[0 : 100]};
  }

  virtual function void write_params();
    set_param("fault_point", fault_point);
    set_param("fault", fault);
    set_param("sweep_progress", sweep_progress);
  endfunction

  virtual task body();
    if (pinned("fault_point", fault_point) == 0) begin
      add_activity("revocation engine sweeping", {PdMain, ".u_cheriot_mem_sys.tbre_active"});
    end
    super.body();
    fork mode_after_reset(); join_none
  endtask

  protected task mode_after_reset();
    uvm_hdl_data_t ena;
    wait (cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest);
    wait (cfg.chip_vif.cpu_rst_n === 1'b0);
    wait (cfg.chip_vif.cpu_rst_n === 1'b1);
    void'(uvm_hdl_read({PdMain, ".rv_core_ibex_cheriot_ena"}, ena));
    if (ena[3:0] == prim_mubi_pkg::MuBi4True) begin
      `uvm_error(`gfn, "EG2_PBF cheriot_fault: CHERIoT mode still on when the core left the reset")
    end else begin
      `uvm_info(`gfn, $sformatf("EG2_PBF cheriot_fault: cheriot_ena 0x%0h after the reset",
                                ena[3:0]), UVM_LOW)
    end
  endtask
endclass

// chip_sw_eg2_pbf_compartment_<area> (REQ_PBF_22): an area's device through a compartment's grant
// and one access beyond it. The hjson entry pins the area (+eg2_param_area); grant and outside are
// drawn here with the spec's distributions.
class chip_sw_eg2_pbf_compartment_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_compartment_vseq)
  `uvm_object_new

  rand int unsigned area;
  rand int unsigned grant;    // 0 exact, 1 narrower, 2 none
  rand int unsigned outside;  // 0 past the window, 1 another device, 2 without the permission

  constraint knobs_c {
    area inside {[0 : 11]};
    grant dist {0 :/ 60, 1 :/ 20, 2 :/ 20};
    outside inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("area", area);
    set_param("grant", grant);
    set_param("outside", outside);
    set_param("model_gaps", 0);
  endfunction
endclass
