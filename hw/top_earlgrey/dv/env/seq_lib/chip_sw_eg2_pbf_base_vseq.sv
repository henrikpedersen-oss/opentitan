// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Base of the EG2 post-boot feature tests (eg2_top_verification spec REQ_PBF, "Post-boot features
// in CHERIoT mode" and "Post-boot faults"; software in sw/device/tests/eg2_pbf). What it adds to
// chip_sw_eg2_base_vseq:
//
//   - The knobs every `_sb` test shares (spec table "Post-boot features in CHERIoT mode"): the
//     life-cycle state, i.e. the OTP image the bench loads (DEV, PROD, RMA, uniform; pinned with
//     +eg2_pbf_lc=dev|prod|rma or with dvsim's +use_otp_image), the ROM_EXT slot, and the
//     instruction cache (drawn by chip_sw_eg2_base_vseq). Only slot A is assembled
//     (hw/top_earlgrey/BUILD sim_dv_rom_ext: "Slot A only"), so `slot` B is reported as not run.
//   - The activity check (REQ_PBF_05): a test names the design signals that show its feature
//     working (add_activity); from the moment the software enters its test code
//     (SwTestStatusInTest) to its end each is sampled every `activity_period_ns`, and each change
//     to a non-zero value counts. When the software is done, every signal must have reached its
//     minimum count, or the run fails with `EG2_PBF activity <name>: <n> < <min>`: a feature that
//     did nothing never passes on the software's word alone. Sampled through uvm_hdl_read, so a
//     path that does not exist fails at once.
//   - Deliberate faults of the bench (a power glitch, a JTAG reset) are announced to the injection
//     registry when it is built in (EG2_PERIPH_SB), with announce_fault().
//
// Behind ROM_EXT the owner image is signed, so the bench writes none of its knobs into it
// (chip_sw_eg2_base_vseq): the test runs with its image's defaults and the bench uses the same.
class chip_sw_eg2_pbf_base_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_base_vseq)
  `uvm_object_new

  // The life cycle state of the run: the OTP image loaded at dut_init.
  typedef enum int {PbfLcDev = 0, PbfLcProd = 1, PbfLcRma = 2} pbf_lc_e;
  rand pbf_lc_e   lc_state;
  rand bit        slot_b;
  int unsigned    activity_period_ns = 100;

  constraint lc_state_c {
    lc_state dist {PbfLcDev :/ 1, PbfLcProd :/ 1, PbfLcRma :/ 1};
  }
  constraint slot_c {
    slot_b dist {0 :/ 1, 1 :/ 1};
  }

  typedef struct {
    string       name;
    string       path;
    int unsigned min_count;
    int unsigned count;
  } activity_t;
  protected activity_t activities[$];
  protected bit        activity_on;

  // A design signal whose changes to a non-zero value show the feature at work.
  protected function void add_activity(string name, string path, int unsigned min_count = 1);
    activity_t a;
    a.name = name;
    a.path = path;
    a.min_count = min_count;
    if (!uvm_hdl_check_path(path)) begin
      `uvm_error(`gfn, $sformatf("EG2_PBF activity %s: no such signal %s", name, path))
    end
    activities.push_back(a);
  endfunction

  // A knob pinned on the command line (+eg2_param_<name>), as chip_sw_eg2_base_vseq writes it.
  protected function bit [31:0] pinned(string name, bit [31:0] value);
    bit [31:0] p;
    if ($value$plusargs({"eg2_param_", name, "=%d"}, p)) return p;
    return value;
  endfunction

  // A deliberate fault of the bench, announced to the checks it may trip. A feature fault is
  // stimulus, not a corruption of a check: it may cause a mismatch on its targets and need not
  // (must_detect 0), so a target that sees nothing is not a MISSED injection. The `_fault` tests
  // run in build mode en_eg2_periph, where the registry is built in (chip_eg2_pbf_tests.hjson).
  protected function int announce_fault(string source, string targets, string kind);
`ifdef EG2_PERIPH_SB
    if (eg2_inject_registry::active()) begin
      return eg2_inject_registry::announce(source, targets, kind, .must_detect(1'b0));
    end
`endif
    `uvm_info(`gfn, $sformatf("EG2_PBF fault %s (%s) on %s", source, kind, targets), UVM_LOW)
    return -1;
  endfunction

  virtual task pre_start();
    string lc;
    if ($value$plusargs("eg2_pbf_lc=%s", lc)) begin
      lc_state = lc == "dev" ? PbfLcDev : lc == "prod" ? PbfLcProd : PbfLcRma;
    end
    // dvsim's +use_otp_image wins over the draw.
    if (!$test$plusargs("use_otp_image")) begin
      cfg.use_otp_image = lc_state == PbfLcDev  ? OtpTypeLcStDev :
                          lc_state == PbfLcProd ? OtpTypeLcStProd : OtpTypeLcStRma;
    end
    `uvm_info(`gfn, $sformatf("EG2_PBF life cycle: OTP image %s", cfg.use_otp_image.name()),
              UVM_LOW)
    if (slot_b) begin
      `uvm_info(`gfn, {"EG2_PBF slot B drawn: not run, only slot A is assembled ",
                       "(sim_dv_rom_ext); the run uses slot A"}, UVM_LOW)
    end
    super.pre_start();
  endtask

  virtual task body();
    super.body();
    fork
      sample_activity();
    join_none
  endtask

  protected task sample_activity();
    uvm_hdl_data_t v[$];
    if (activities.size() == 0) return;
    wait (cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest);
    activity_on = 1'b1;
    foreach (activities[i]) v.push_back('0);
    while (activity_on) begin
      foreach (activities[i]) begin
        uvm_hdl_data_t x;
        void'(uvm_hdl_read(activities[i].path, x));
        if (x != v[i] && x != '0) activities[i].count++;
        v[i] = x;
      end
      #(activity_period_ns * 1ns);
    end
  endtask

  virtual task post_start();
    super.post_start();
    activity_on = 1'b0;
    foreach (activities[i]) begin
      if (activities[i].count < activities[i].min_count) begin
        `uvm_error(`gfn, $sformatf("EG2_PBF activity %s: %0d < %0d (%s)", activities[i].name,
                                   activities[i].count, activities[i].min_count,
                                   activities[i].path))
      end else begin
        `uvm_info(`gfn, $sformatf("EG2_PBF activity %s: %0d (at least %0d)", activities[i].name,
                                  activities[i].count, activities[i].min_count), UVM_LOW)
      end
    end
  endtask
endclass
