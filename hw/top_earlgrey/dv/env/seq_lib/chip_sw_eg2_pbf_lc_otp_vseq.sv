// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life-cycle and OTP tests of the EG2 post-boot features (eg2_top_verification spec
// REQ_PBF_16: chip_sw_eg2_pbf_lc_otp_sb and chip_sw_eg2_pbf_lc_otp_fault; software
// sw/device/tests/eg2_pbf/eg2_pbf_lc_otp_{sb,fault}.c). The bench draws the knobs, tells the
// software which life-cycle state's OTP image it loaded (lc_expect, from the base vseq's draw),
// and confirms from the design that OTP's direct access interface ran and lc_ctrl programmed the
// transition counter (REQ_PBF_05). The watchdog, armed by the software, resets the chip after a
// request (the core stops in POST_TRANSITION). Behind ROM_EXT the signed image keeps its
// defaults and the boot code may lock DAI and the transition interface, so the bench requires no
// activity there.

// chip_sw_eg2_pbf_lc_otp_sb: OTP reads and a refused life-cycle transition request.
class chip_sw_eg2_pbf_lc_otp_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_lc_otp_sb_vseq)
  `uvm_object_new

  rand bit [1:0] partition;
  rand bit [1:0] transition;

  constraint knobs_c {
    partition inside {[0 : 2]};
    transition dist {0 :/ 1, 1 :/ 2, 2 :/ 2};
  }

  // The state of the OTP image the bench loads (pre_start): 1 DEV, 2 PROD, 3 RMA, 0 another.
  protected function bit [31:0] lc_expect();
    case (cfg.use_otp_image)
      OtpTypeLcStDev: return 1;
      OtpTypeLcStProd: return 2;
      OtpTypeLcStRma: return 3;
      default: return 0;
    endcase
  endfunction

  virtual function void write_params();
    set_param("partition", partition);
    set_param("transition", transition);
    set_param("lc_expect", lc_expect());
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] t = pinned("transition", transition);
    if (!cfg.rom_ext_assembled) begin
      add_activity("OTP direct access (the DAI state machine)",
                   "tb.dut.top_earlgrey.earlgrey_pd_main.u_otp_ctrl.u_otp_ctrl_dai.state_q", 2);
      if (t != 0) begin
        add_activity("lc_ctrl transition counter programming",
                     "tb.dut.top_earlgrey.earlgrey_pd_main.u_lc_ctrl.lc_otp_program_o");
      end
    end
    super.body();
  endtask
endclass

// chip_sw_eg2_pbf_lc_otp_fault: a reset in the middle of an OTP write or a transition request.
class chip_sw_eg2_pbf_lc_otp_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_lc_otp_fault_vseq)
  `uvm_object_new

  rand bit       fault;
  rand bit [1:0] fault_point;

  constraint knobs_c {
    fault_point inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("fault", fault);
    set_param("fault_point", fault_point);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] f = pinned("fault", fault);
    bit [31:0] p = pinned("fault_point", fault_point);
    if (f == 0) begin
      add_activity("OTP direct access (the DAI state machine)",
                   "tb.dut.top_earlgrey.earlgrey_pd_main.u_otp_ctrl.u_otp_ctrl_dai.state_q", 2);
    end else if (p == 2) begin
      // The earlier fault points may bite before the counter is programmed.
      add_activity("lc_ctrl transition counter programming",
                   "tb.dut.top_earlgrey.earlgrey_pd_main.u_lc_ctrl.lc_otp_program_o");
    end
    super.body();
  endtask
endclass
