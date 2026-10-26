// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of the EG2 debug-module tests' sequences (chip_rv_dm_jtag, chip_rv_dm_dtm,
// chip_rv_dm_control_status, chip_rv_dm_sba_tl_errors; software in sw/device/tests/eg2_debug,
// contract in eg2_debug.h). Runs with +use_jtag_dmi=1 (the JTAG DTM and DMI register models).
//
//  - The life-cycle state is drawn among the states that enable the debug module (rv_dm.hjson,
//    DM_EN.CTRL.LC_GATED): TEST_UNLOCKED0-7, RMA, and DEV with late debug either enabled by OTP
//    (DIS_RV_DM_LATE_DEBUG = MuBi8True) or by the software's write of LATE_DEBUG_ENABLE
//    (late_debug_sw, written into the image). It is written into OTP by backdoor before reset, as
//    chip_rv_dm_lc_disabled_vseq does.
//  - The software prints `EG2 DEBUG ready 0x<done> 0x<scratch>` (wait_ready); the JTAG accesses
//    start at a drawn moment after it (w_when_<t>: at once, after a short or after a long delay,
//    so the core is at different points of its loop).
//  - finish() ends the software's loop: the test's magic word written to `done` over SBA.
//
// CHERIoT mode: rv_dm has no CHERIoT input (status/rtl_tickets.md F13). JTAG, DTM and SBA do not
// depend on the core's mode; a halt does (chip_eg2_rv_dm_control_status_vseq).
class chip_eg2_rv_dm_base_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_base_vseq)

  rand lc_ctrl_state_pkg::lc_state_e lc_state;
  rand bit dis_late_debug_otp;
  rand bit late_debug_sw;
  rand int unsigned w_when_at_ready, w_when_short, w_when_long;
  rand int unsigned delay_short_ns, delay_long_ns;
  // A uniform draw over the total weight: the class it falls in is drawn by the weights.
  rand int unsigned when_pick;

  // From the software's ready line.
  bit [31:0] done_addr, scratch_addr;

  constraint lc_state_c {
    lc_state dist {
      LcStTestUnlocked0 := 4, LcStTestUnlocked1 := 4, LcStTestUnlocked2 := 4,
      LcStTestUnlocked3 := 4, LcStTestUnlocked4 := 4, LcStTestUnlocked5 := 4,
      LcStTestUnlocked6 := 3, LcStTestUnlocked7 := 3, LcStDev := 35, LcStRma := 35
    };
  }

  // In DEV the debug module is enabled by OTP or by LATE_DEBUG_ENABLE; elsewhere the write is
  // not needed and is not made.
  constraint late_debug_c {
    solve lc_state before dis_late_debug_otp, late_debug_sw;
    (lc_state != LcStDev) -> !late_debug_sw;
    (lc_state == LcStDev) -> (dis_late_debug_otp || late_debug_sw);
  }

  constraint w_when_c {
    w_when_at_ready inside {[1:100]};
    w_when_short inside {[1:100]};
    w_when_long inside {[1:100]};
    delay_short_ns inside {[1_000:50_000]};
    delay_long_ns inside {[100_000:400_000]};
    solve w_when_at_ready, w_when_short, w_when_long before when_pick;
    when_pick < w_when_at_ready + w_when_short + w_when_long;
  }

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  virtual function void initialize_otp_lc_state();
    otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], lc_state);
    otp_write_hw_cfg1_partition(
        .mem_bkdr_util_h(cfg.mem_bkdr_util_h[Otp]),
        .en_sram_ifetch(prim_mubi_pkg::MuBi8False),
        .dis_rv_dm_late_debug(dis_late_debug_otp ? prim_mubi_pkg::MuBi8True
                                                 : prim_mubi_pkg::MuBi8False));
    `uvm_info(`gfn, $sformatf("EG2 debug: life cycle %s, OTP late debug disabled %0d, sw late %0d",
                              lc_state.name(), dis_late_debug_otp, late_debug_sw), UVM_LOW)
  endfunction

  virtual function void write_params();
    set_param("late_debug_sw", late_debug_sw);
  endfunction

  // Waits for the software's ready line, then the drawn delay; activates the debug module.
  protected task wait_ready();
    string line;
    int unsigned weights[3] = '{w_when_at_ready, w_when_short, w_when_long};
    int unsigned r = when_pick;
    wait_sw_line("EG2 DEBUG ready", line);
    if (!($sscanf(line, "EG2 DEBUG ready 0x%h 0x%h", done_addr, scratch_addr) == 2)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 DEBUG ready 0x%%h 0x%%h\", done_addr, scratch_addr) == 2) %s", $sformatf("cannot read the addresses in '%s'", line)))
    if (r < weights[0]) begin
      `uvm_info(`gfn, "EG2 debug: JTAG accesses at the ready line", UVM_LOW)
    end else if (r < weights[0] + weights[1]) begin
      `uvm_info(`gfn, $sformatf("EG2 debug: JTAG accesses after %0d ns", delay_short_ns), UVM_LOW)
      #(delay_short_ns * 1ns);
    end else begin
      `uvm_info(`gfn, $sformatf("EG2 debug: JTAG accesses after %0d ns", delay_long_ns), UVM_LOW)
      #(delay_long_ns * 1ns);
    end
    cfg.debugger.set_dmactive(1);
  endtask

  // Ends the software's loop: the magic word of test `id` (eg2_debug.h) to `done` over SBA.
  protected task finish(int unsigned id);
    sba_access_err_e st;
    sba_wr32(done_addr, 32'h600d_0000 + id, st);
    `DV_CHECK_EQ(st, SbaErrNone, "the SBA write of the done word failed")
  endtask

endclass : chip_eg2_rv_dm_base_vseq
