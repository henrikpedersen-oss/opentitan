// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The alert and escalation tests of the EG2 post-boot features (eg2_top_verification spec
// REQ_PBF_17: chip_sw_eg2_pbf_alert_sb and chip_sw_eg2_pbf_alert_fault; software
// sw/device/tests/eg2_pbf/eg2_pbf_alert_{sb,fault}.c). The software raises the alert and drives
// the escalation; the bench draws the knobs and confirms the activity from the design: the
// escalating class's interrupt, and pwrmgr's escalation reset request where the escalation runs
// to the reset (REQ_PBF_05). Behind ROM_EXT the signed image keeps its defaults, and so does the
// bench.

// chip_sw_eg2_pbf_alert_sb: a class's escalation, stopped by software at a phase or run to the
// escalation reset with the crash dump.
class chip_sw_eg2_pbf_alert_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_alert_sb_vseq)
  `uvm_object_new

  rand bit [1:0] alert_class;
  rand bit [2:0] stop_phase;
  rand bit [1:0] dump_phase;

  constraint knobs_c {
    alert_class inside {[0 : 2]};
    // Half the runs go to the escalation reset (4), the others stop in one of the four phases.
    stop_phase dist {4 :/ 4, [0 : 3] :/ 4};
    dump_phase inside {[0 : 3]};
  }

  virtual function void write_params();
    set_param("alert_class", alert_class);
    set_param("stop_phase", stop_phase);
    set_param("dump_phase", dump_phase);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] cls = pinned("alert_class", alert_class);
    bit [31:0] stop = pinned("stop_phase", stop_phase);
    string intr[3] = '{"intr_classa_o", "intr_classb_o", "intr_classc_o"};
    if (cfg.rom_ext_assembled) begin
      cls = 0;
      stop = 4;
    end
    add_activity("escalating class interrupt",
                 {"tb.dut.top_earlgrey.earlgrey_pd_main.u_alert_handler.", intr[cls]});
    if (stop == 4) begin
      add_activity("escalation reset request",
                   "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.esc_rst_req_q");
    end
    super.body();
  endtask
endclass

// chip_sw_eg2_pbf_alert_fault: software tries to stop a locked escalation; it goes on to the
// reset.
class chip_sw_eg2_pbf_alert_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_alert_fault_vseq)
  `uvm_object_new

  rand bit [1:0] alert_class;
  rand bit [1:0] fault_point;
  rand bit [1:0] fault;

  constraint knobs_c {
    alert_class inside {[0 : 2]};
    fault_point inside {[0 : 2]};
    fault inside {[0 : 3]};
  }

  virtual function void write_params();
    set_param("alert_class", alert_class);
    set_param("fault_point", fault_point);
    set_param("fault", fault);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] cls = pinned("alert_class", alert_class);
    string intr[3] = '{"intr_classa_o", "intr_classb_o", "intr_classc_o"};
    add_activity("escalating class interrupt",
                 {"tb.dut.top_earlgrey.earlgrey_pd_main.u_alert_handler.", intr[cls]});
    add_activity("escalation reset request",
                 "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.esc_rst_req_q");
    super.body();
  endtask
endclass
