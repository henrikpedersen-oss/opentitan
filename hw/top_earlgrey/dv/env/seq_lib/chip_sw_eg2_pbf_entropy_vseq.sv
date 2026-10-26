// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_entropy_sb and _fault (eg2_top_verification spec REQ_PBF_12; software
// sw/device/tests/eg2_pbf/eg2_pbf_entropy_{sb,fault}.c). Behind ROM_EXT the signed image keeps its
// defaults (chip_sw_eg2_base_vseq) and the bench follows them.

// csrng's software instance: a random command sequence checked against a CTR_DRBG reference.
class chip_sw_eg2_pbf_entropy_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_entropy_sb_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [4:0] num_cmds;
  rand bit [4:0] max_blocks;   // generate 1 to max_blocks blocks
  rand bit [1:0] adata_class;  // 0 none, 1 1-11 words, 2 12 words, 3 drawn per command

  constraint knobs_c {
    num_cmds inside {[1 : 16]};
    max_blocks inside {[1 : 16]};
    max_blocks dist {1 :/ 10, [2 : 15] :/ 60, 16 :/ 30};
    adata_class inside {[0 : 3]};
  }

  virtual function void write_params();
    set_param("num_cmds", num_cmds);
    set_param("max_blocks", max_blocks);
    set_param("adata_class", adata_class);
  endfunction

  virtual task body();
    // REQ_PBF_05: csrng delivered genbits to the software port (one per generate at least).
    add_activity("csrng genbits valid", {PD, ".u_csrng.u_reg.genbits_vld_genbits_vld_qs"}, 2);
    super.body();
  endtask
endclass

// A forced health-test failure of entropy_src while the consumers request entropy.
class chip_sw_eg2_pbf_entropy_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_entropy_fault_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [4:0]  blocks;
  rand bit [6:0]  threshold;
  rand bit [31:0] stall_polls;

  constraint knobs_c {
    blocks inside {[2 : 16]};
    threshold inside {[1 : 64]};
    stall_polls inside {[5000 : 50000]};
  }

  virtual function void write_params();
    set_param("blocks", blocks);
    set_param("threshold", threshold);
    set_param("stall_polls", stall_polls);
  endfunction

  // After entropy_src's main state machine alert: count the genbits blocks csrng still hands to
  // each EDN (packer pushes). EDN serves its consumers from its DRBG instance until a reseed, so
  // the count is reported, not judged; the stall itself is the software's check (a reseed that
  // needs entropy never completes).
  protected task count_after_failure();
    uvm_hdl_data_t v, p0, p1;
    int unsigned n0, n1;
    forever begin
      void'(uvm_hdl_read({PD, ".u_entropy_src.u_reg.recov_alert_sts_es_main_sm_alert_qs"}, v));
      if (v[0]) break;
      #100ns;
    end
    void'(announce_fault("eg2_pbf_entropy_fault health test", "entropy_src,edn0,edn1",
                         "forced health-test failure"));
    `uvm_info(`gfn, "EG2_PBF entropy_src: main state machine alert", UVM_LOW)
    repeat (2000) begin
      void'(uvm_hdl_read({PD, ".u_edn0.u_edn_core.packer_cs_push"}, v));
      if (v[0] && !p0[0]) n0++;
      p0 = v;
      void'(uvm_hdl_read({PD, ".u_edn1.u_edn_core.packer_cs_push"}, v));
      if (v[0] && !p1[0]) n1++;
      p1 = v;
      #50ns;
    end
    `uvm_info(`gfn, $sformatf("EG2_PBF genbits blocks to edn0 %0d, edn1 %0d in the 100 us after the alert",
                              n0, n1), UVM_LOW)
  endtask

  virtual task body();
    // REQ_PBF_05: the health test failed and raised its interrupt.
    add_activity("es_health_test_failed",
                 {PD, ".u_entropy_src.u_reg.intr_state_es_health_test_failed_qs"}, 1);
    add_activity("es_main_sm_alert",
                 {PD, ".u_entropy_src.u_reg.recov_alert_sts_es_main_sm_alert_qs"}, 1);
    super.body();
    fork
      count_after_failure();
    join_none
  endtask
endclass
