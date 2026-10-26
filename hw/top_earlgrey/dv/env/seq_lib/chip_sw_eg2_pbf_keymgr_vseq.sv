// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_keymgr_sb and _fault (eg2_top_verification spec REQ_PBF_15; software
// sw/device/tests/eg2_pbf/eg2_pbf_keymgr_{sb,fault}.c). Behind ROM_EXT the signed image keeps its
// defaults (chip_sw_eg2_base_vseq) and the bench follows them.

// Sealing and versioned keys from keymgr_dpe.
class chip_sw_eg2_pbf_keymgr_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_keymgr_sb_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit       key_type;       // 0 sealing, 1 versioned
  rand bit [1:0] version_class;  // 0 below, 1 at, 2 above the maximum version, 3 drawn
  rand bit [1:0] salt_class;     // 0 zero, 1 random, 2 all ones

  constraint knobs_c {
    version_class inside {[0 : 3]};
    // Around the maximum: at and above it as often as below.
    version_class dist {0 :/ 30, 1 :/ 30, 2 :/ 30, 3 :/ 10};
    salt_class inside {[0 : 2]};
    salt_class dist {0 :/ 20, 1 :/ 60, 2 :/ 20};
  }

  virtual function void write_params();
    set_param("key_type", key_type);
    set_param("version_class", version_class);
    set_param("salt_class", salt_class);
  endfunction

  virtual task body();
    // REQ_PBF_05: keymgr_dpe completed operations (OP_STATUS leaves IDLE for DONE_SUCCESS or
    // DONE_ERROR): the bring-up advances and the test's generates.
    add_activity("keymgr_dpe operations", {PD, ".u_keymgr_dpe.u_reg.op_status_qs"}, 3);
    super.body();
  endtask
endclass

// An escalation during an advance or a generate.
class chip_sw_eg2_pbf_keymgr_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_keymgr_fault_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [1:0] op;           // 0 advance, 1 generate
  rand bit [1:0] fault_point;  // before the start, in flight, after completion

  constraint knobs_c {
    op inside {[0 : 1]};
    fault_point inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("op", op);
    set_param("fault_point", fault_point);
  endfunction

  // On the life-cycle escalation the key manager is deactivated: WORKING_STATE Invalid and the
  // software key wiped (theory of operation "Invalid"), checked before the escalation reset. The
  // software output is sampled until the escalation; a non-zero key that is still there 2 us
  // later was not wiped.
  protected task check_escalation();
    uvm_hdl_data_t v, key;
    forever begin
      void'(uvm_hdl_read({PD, ".u_keymgr_dpe.u_reg.sw_share0_output_0_qs"}, key));
      void'(uvm_hdl_read({PD, ".lc_ctrl_lc_escalate_en"}, v));
      if (v[3:0] == 4'b0101) break;
      #100ns;
    end
    void'(announce_fault("eg2_pbf_keymgr_fault escalation", "keymgr_dpe",
                         "life-cycle escalation"));
    #2us;
    void'(uvm_hdl_read({PD, ".u_keymgr_dpe.u_reg.working_state_qs"}, v));
    if (v[1:0] != 2'd3) begin
      `uvm_error(`gfn, $sformatf("EG2_PBF keymgr_dpe: WORKING_STATE %0d under escalation, not Invalid",
                                 v[1:0]))
    end
    void'(uvm_hdl_read({PD, ".u_keymgr_dpe.u_reg.sw_share0_output_0_qs"}, v));
    if (key[31:0] != 0 && v[31:0] == key[31:0]) begin
      `uvm_error(`gfn, "EG2_PBF keymgr_dpe: software key not wiped under escalation")
    end
    `uvm_info(`gfn, "EG2_PBF escalation: keymgr_dpe Invalid", UVM_LOW)
  endtask

  virtual task body();
    add_activity("keymgr_dpe operations", {PD, ".u_keymgr_dpe.u_reg.op_status_qs"}, 2);
    super.body();
    fork
      check_escalation();
    join_none
  endtask
endclass
