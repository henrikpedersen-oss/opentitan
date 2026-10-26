// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_lc_disabled_cheriot (sw/device/tests/eg2_debug/eg2_rv_dm_lc_disabled.c; EG2 spec
// REQ_CDM_05, REQ_PBF_20, REQ_SEC_04), CHERIoT mode: chip_rv_dm_lc_disabled_vseq's checks with the
// core running CHERIoT-mode software instead of the stub CPU.
//
// The life-cycle state is drawn uniformly among the states in which the core runs (TEST_UNLOCKED
// 0-7, DEV, PROD, PROD_END, RMA) and written into OTP by backdoor before reset with
// DIS_RV_DM_LATE_DEBUG; the software writes LATE_DEBUG_ENABLE (late_debug_sw). The expectations
// follow rv_dm.hjson (DM_EN.CTRL.LC_GATED, LATE_DEBUG_ENABLE) as chip_rv_dm_lc_disabled_vseq states
// them: the DMI is enabled in TEST_UNLOCKED0-7, DEV and RMA; the debug memory in TEST_UNLOCKED0-7
// and RMA, and in DEV with DIS_RV_DM_LATE_DEBUG = MuBi8True or LATE_DEBUG_ENABLE = MuBi32True.
// The software checks the core's debug-memory loads; after its ready line the bench activates the
// debug module over JTAG and writes and reads back sbaddress0 n_jtag times: all-zero when the DMI
// is gated, the written value otherwise.
class chip_eg2_rv_dm_lc_disabled_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_lc_disabled_vseq)

  rand lc_ctrl_state_pkg::lc_state_e lc_state;
  rand bit dis_late_debug_otp;
  rand bit late_debug_sw;
  rand int unsigned n_loads;
  rand int unsigned n_jtag;
  rand bit [31:0] jtag_wdata[];

  constraint lc_state_c {
    lc_state inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2, LcStTestUnlocked3,
                     LcStTestUnlocked4, LcStTestUnlocked5, LcStTestUnlocked6, LcStTestUnlocked7,
                     LcStDev, LcStProd, LcStProdEnd, LcStRma};
  }

  constraint knobs_c {
    n_loads inside {[1:16]};
    n_jtag inside {[2:8]};
    jtag_wdata.size() == n_jtag;
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
    `uvm_info(`gfn, $sformatf("EG2 debug lc: %s, OTP late debug disabled %0d, sw late %0d",
                              lc_state.name(), dis_late_debug_otp, late_debug_sw), UVM_LOW)
  endfunction

  // lc_ctrl's decoded state (LC_STATE's 5-bit code; lc_ctrl_state_pkg dec_lc_state_e).
  protected function int unsigned dec_state();
    case (lc_state)
      LcStTestUnlocked0: return 1;
      LcStTestUnlocked1: return 3;
      LcStTestUnlocked2: return 5;
      LcStTestUnlocked3: return 7;
      LcStTestUnlocked4: return 9;
      LcStTestUnlocked5: return 11;
      LcStTestUnlocked6: return 13;
      LcStTestUnlocked7: return 15;
      LcStDev:           return 16;
      LcStProd:          return 17;
      LcStProdEnd:       return 18;
      default:           return 19;  // LcStRma
    endcase
  endfunction

  protected function bit dmi_gated();
    return !(lc_state inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2,
                              LcStTestUnlocked3, LcStTestUnlocked4, LcStTestUnlocked5,
                              LcStTestUnlocked6, LcStTestUnlocked7, LcStRma, LcStDev});
  endfunction

  protected function bit mem_gated();
    if (lc_state inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2,
                         LcStTestUnlocked3, LcStTestUnlocked4, LcStTestUnlocked5,
                         LcStTestUnlocked6, LcStTestUnlocked7, LcStRma}) begin
      return 0;
    end
    if (lc_state == LcStDev && (dis_late_debug_otp || late_debug_sw)) return 0;
    return 1;
  endfunction

  virtual function void write_params();
    set_param("lc_state_dec", dec_state());
    set_param("late_debug_sw", late_debug_sw);
    set_param("exp_mem_gated", mem_gated());
    set_param("n_loads", n_loads);
  endfunction

  virtual task body();
    string line;
    uvm_reg_data_t rdata, exp;
    super.body();
    wait_sw_line("EG2 DEBUG lc ready", line);
    if (line == "") return;  // the software ended first (an _inject_fail build)
    `uvm_info(`gfn, $sformatf("EG2 debug lc: DMI gated %0d, debug memory gated %0d", dmi_gated(),
                              mem_gated()), UVM_LOW)
    // Activate the debug module even where it is gated, so a wrong gate is not hidden by an
    // inactive module (as chip_rv_dm_lc_disabled_vseq does).
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.dmactive), .value(1), .blocking(1), .predict(1));
    cfg.clk_rst_vif.wait_clks(5);
    foreach (jtag_wdata[i]) begin
      csr_wr(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(jtag_wdata[i]), .blocking(1),
             .predict(!dmi_gated()));
      cfg.clk_rst_vif.wait_clks(5);
      exp = dmi_gated() ? '0 : jtag_wdata[i];
      csr_rd(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(rdata), .blocking(1), .check(UVM_NO_CHECK));
      `DV_CHECK_EQ(rdata, exp, $sformatf("RV_DM sbaddress0 read back in %s (DMI gated %0d)",
                                         lc_state.name(), dmi_gated()))
    end
  endtask

endclass : chip_eg2_rv_dm_lc_disabled_vseq
