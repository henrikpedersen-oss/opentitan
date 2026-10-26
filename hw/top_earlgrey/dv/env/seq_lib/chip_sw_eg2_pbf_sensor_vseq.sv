// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The sensor tests of the EG2 post-boot features (eg2_top_verification spec REQ_PBF_21:
// chip_sw_eg2_pbf_sensor_sb and chip_sw_eg2_pbf_sensor_fault; software
// sw/device/tests/eg2_pbf/eg2_pbf_sensor_{sb,fault}.c). The software raises the AST events
// through sensor_ctrl's ALERT_TRIG, which loops through the AST model's event outputs; the bench
// draws the knobs and confirms from the design that sensor_ctrl's alerts were sent and, where the
// wake-up is configured, its wake-up request rose (REQ_PBF_05). Behind ROM_EXT the signed image
// keeps its defaults, and so does the bench.

// chip_sw_eg2_pbf_sensor_sb: a recoverable or fatal event, the wake-up configured or not.
class chip_sw_eg2_pbf_sensor_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_sensor_sb_vseq)
  `uvm_object_new

  rand bit [3:0] event_idx;
  rand bit       fatal;
  rand bit       wake;
  rand bit [1:0] alert_class;

  constraint knobs_c {
    event_idx inside {[0 : 10]};
    fatal dist {0 :/ 2, 1 :/ 1};
    alert_class inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("event", event_idx);
    set_param("fatal", fatal);
    set_param("wake", wake);
    set_param("alert_class", alert_class);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] f = pinned("fatal", fatal);
    bit [31:0] w = pinned("wake", wake);
    if (cfg.rom_ext_assembled) begin
      f = 0;
      w = 1;
    end
    add_activity("sensor_ctrl alerts sent",
                 "tb.dut.top_earlgrey.earlgrey_pd_aon.u_sensor_ctrl.alert_tx_o");
    if (!f && w) begin
      add_activity("sensor_ctrl wake-up request",
                   "tb.dut.top_earlgrey.earlgrey_pd_aon.u_sensor_ctrl.wkup_req_o");
    end
    if (f) begin
      add_activity("escalation reset request",
                   "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.esc_rst_req_q");
    end
    super.body();
  endtask
endclass

// chip_sw_eg2_pbf_sensor_fault: a second event during an acknowledgement, or a corrupted
// configuration.
class chip_sw_eg2_pbf_sensor_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_sensor_fault_vseq)
  `uvm_object_new

  rand bit       fault;
  rand bit [1:0] fault_point;
  rand bit [3:0] event_idx;
  rand bit [3:0] event2;
  rand bit       same;
  rand bit [1:0] alert_class;

  constraint knobs_c {
    fault_point inside {[0 : 2]};
    event_idx inside {[0 : 10]};
    event2 inside {[0 : 10]};
    same dist {0 :/ 3, 1 :/ 1};
    alert_class inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("fault", fault);
    set_param("fault_point", fault_point);
    set_param("event", event_idx);
    set_param("event2", event2);
    set_param("same", same);
    set_param("alert_class", alert_class);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] f = pinned("fault", fault);
    // Two events (fault 0) send the alert at least twice; the corrupted configuration once.
    add_activity("sensor_ctrl alerts sent",
                 "tb.dut.top_earlgrey.earlgrey_pd_aon.u_sensor_ctrl.alert_tx_o", f ? 1 : 2);
    super.body();
  endtask
endclass
