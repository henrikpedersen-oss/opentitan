// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The power tests of the EG2 post-boot features (eg2_top_verification spec REQ_PBF_18:
// chip_sw_eg2_pbf_power_sb and chip_sw_eg2_pbf_power_fault; software
// sw/device/tests/eg2_pbf/eg2_pbf_power_{sb,fault}.c). The bench draws the knobs, drives the
// wake-up pin and the main power glitch, and confirms from the design that pwrmgr entered low
// power (REQ_PBF_05). Behind ROM_EXT the signed image keeps its defaults, and so does the bench.

// chip_sw_eg2_pbf_power_sb: a wake-up source, normal or deep sleep, a time asleep.
class chip_sw_eg2_pbf_power_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_power_sb_vseq)
  `uvm_object_new

  // 0 aon_timer, 1 sensor_ctrl (software trigger), 2 pinmux wake-up detector on IOB7 (bench).
  rand bit [1:0] wake_src;
  rand bit       deep;
  rand bit [1:0] sleep_class;

  constraint knobs_c {
    wake_src inside {[0 : 2]};
    sleep_class inside {[0 : 2]};
  }

  // The AON cycles of each sleep class (eg2_pbf_power_sb.c kSleepCycles), 5 us each.
  localparam int unsigned SleepCycles[3] = '{20, 100, 400};

  virtual function void write_params();
    set_param("wake_src", wake_src);
    set_param("deep", deep);
    set_param("sleep_class", sleep_class);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] src = pinned("wake_src", wake_src);
    bit [31:0] cls = pinned("sleep_class", sleep_class);
    if (cfg.rom_ext_assembled) begin
      src = 0;
      cls = 1;
    end
    add_activity("pwrmgr low power", "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.low_power_o");
    // The pin keeps a defined value until it wakes the chip.
    cfg.chip_vif.pinmux_wkup_if.set_pulldown_en(1'b1);
    super.body();
    if (src == 2) begin
      fork
        begin
          `DV_WAIT(cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest)
          wait (cfg.chip_vif.pwrmgr_low_power === 1'b1);
          #(SleepCycles[cls] * 5us);
          `uvm_info(`gfn, "EG2_PBF power: the wake-up pin (IOB7) rises", UVM_LOW)
          cfg.chip_vif.pinmux_wkup_if.drive(1'b1);
          wait (cfg.chip_vif.pwrmgr_low_power === 1'b0);
          cfg.chip_vif.pinmux_wkup_if.drive(1'b0);
        end
      join_none
    end
  endtask
endclass

// chip_sw_eg2_pbf_power_fault: a main power glitch at the sleep's entry, during it, or at its exit.
class chip_sw_eg2_pbf_power_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_power_fault_vseq)
  `uvm_object_new

  rand bit       deep;
  rand bit [1:0] fault_point;
  // Cycles after core_sleeping rises before the entry glitch (chip_sw_main_power_glitch_vseq).
  rand int unsigned cycles_after_trigger;

  constraint knobs_c {
    fault_point inside {[0 : 2]};
    cycles_after_trigger inside {[5 : 13]};
  }

  virtual function void write_params();
    set_param("deep", deep);
    set_param("fault_point", fault_point);
    set_param("model_gaps", 0);
  endfunction

  virtual task pre_start();
    super.pre_start();
    // The glitch trips these pwrmgr assertions by design (chip_sw_main_power_glitch_vseq).
    $assertoff(1, "tb.dut.top_earlgrey.earlgrey_pd_aon.pwrmgr_rstreqs_sva_if.MainPwrRstOff_A");
    $assertoff(1, "tb.dut.top_earlgrey.earlgrey_pd_aon.pwrmgr_rstreqs_sva_if.MainPwrRstOn_A");
  endtask

  protected task glitch();
    int id = announce_fault("eg2_pbf power_fault vcmain_pok glitch", "rvfi,insn",
                            $sformatf("main power glitch at fault point %0d", fault_point));
    cfg.ast_supply_vif.force_vcmain_pok(1'b0);
    #1ps;
    cfg.ast_supply_vif.release_vcmain_pok();
  endtask

  virtual task body();
    bit [31:0] point = pinned("fault_point", fault_point);
    add_activity("pwrmgr low power", "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.low_power_o");
    super.body();
    fork
      begin
        `DV_WAIT(cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest)
        case (point)
          0: begin
            void'(announce_fault("eg2_pbf power_fault vcmain_pok glitch", "rvfi,insn",
                                 "main power glitch at the sleep entry"));
            cfg.ast_supply_vif.glitch_vcmain_pok_on_next_core_sleeping_trigger(
                cycles_after_trigger);
          end
          1: begin
            wait (cfg.chip_vif.pwrmgr_low_power === 1'b1);
            // The middle of the 500 us sleep.
            #250us;
            glitch();
          end
          default: begin
            wait (cfg.chip_vif.pwrmgr_low_power === 1'b1);
            wait (cfg.chip_vif.pwrmgr_low_power === 1'b0);
            glitch();
          end
        endcase
        `DV_WAIT(cfg.sw_test_status_vif.sw_test_status == SwTestStatusInBootRom)
        cfg.ast_supply_vif.reenable_vcmain_assertion();
      end
    join_none
  endtask
endclass
