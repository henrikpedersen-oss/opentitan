// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rram_jtag_lc_gate (EG2 verification spec REQ_RRAM_20, REQ_SEC_04): the rram_macro JTAG port
// and the analog pad work only with lc_nvm_debug_en (RFC RRAM §3.1 p7, §8 p21; lc_ctrl
// theory_of_operation.md "NVM_DEBUG_EN": enabled in TEST_UNLOCKED0-6, disabled in TEST_UNLOCKED7
// and every later state). No software: the stub CPU programs pinmux.
//
// One life-cycle state per run, drawn uniformly from the states whose OTP image boots (RAW and
// SCRAP excluded: no CPU, no TAP strap sampling). Checks:
//   1. lc_nvm_debug_en at rram_macro's input is On exactly in TEST_UNLOCKED0-6;
//   2. the macro's JTAG pins are routed by pinmux (MIO_PERIPH_INSEL rram_macro_tck/tms/tdi, its
//      tdo on an MIO output) and reach the macro: each toggled pad is seen at its cio input;
//   3. gated (lc_nvm_debug_en Off): while TCK, TMS and TDI toggle through a TAP reset and an IDCODE
//      scan, the macro never enables TDO and the analog pad stays undriven (high impedance);
//   4. enabled: the IDCODE scan returns an IDCODE (bit 0 set, not all ones) on TDO.
// Expected failure on this RTL: step 4 in TEST_UNLOCKED0-6. The open-source emulation macro has no
// TAP (rram_macro.sv: tck, tms, tdi unused, tdo tied off), so the enabled half of the requirement
// cannot pass until the vendor macro (or a TAP model) is in the chip; the gated half and the gate
// signal itself are checked on every run. Proposed ticket: rram_macro emulation has no JTAG TAP.
class chip_eg2_rram_jtag_lc_gate_vseq extends chip_stub_cpu_base_vseq;
  `uvm_object_utils(chip_eg2_rram_jtag_lc_gate_vseq)

  `uvm_object_new

  localparam string Macro = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_macro";
  // MIO pads for TCK, TMS, TDI and TDO: IOB6-IOB9, no strap among them.
  localparam int PadTck = 15, PadTms = 16, PadTdi = 17, PadTdo = 18;
  localparam bit [31:0] Pinmux = top_earlgrey_pkg::TOP_EARLGREY_PINMUX_BASE_ADDR;
  localparam bit [31:0] InselOff = 32'hf8, OutselOff = 32'h2a8;
  localparam int InTck = 51, InTms = 52, InTdi = 53;  // kTopEarlgreyPinmuxPeripheralInRramMacro*
  localparam int OutTdo = 57;                        // kTopEarlgreyPinmuxOutselRramMacroTdo
  localparam int InselMioBase = 2;                   // kTopEarlgreyPinmuxInselIoa0

  rand lc_state_e lc_state;
  // When set, the enabled states' IDCODE check runs (the requirement; fails on the emulation
  // macro). +eg2_rram_jtag_expect_tap=0 turns it off to run the rest alone.
  bit expect_tap = 1'b1;

  constraint lc_state_c {
    lc_state inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2, LcStTestUnlocked3,
                     LcStTestUnlocked4, LcStTestUnlocked5, LcStTestUnlocked6, LcStTestUnlocked7,
                     LcStDev, LcStProd, LcStProdEnd, LcStRma};
  }

  virtual function void initialize_otp_lc_state();
    otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], lc_state);
  endfunction

  protected function logic sample(string sig);
    uvm_hdl_data_t v;
    if (!(uvm_hdl_read({Macro, ".", sig}, v))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({Macro, \".\", sig}, v)) %s", sig))
    return v[0];
  endfunction

  protected task tck_cycle(bit tms, bit tdi, output logic tdo, output logic tdo_en);
    cfg.chip_vif.mios_if.drive_pin(PadTms, tms);
    cfg.chip_vif.mios_if.drive_pin(PadTdi, tdi);
    cfg.chip_vif.mios_if.drive_pin(PadTck, 1'b0);
    #50ns;
    cfg.chip_vif.mios_if.drive_pin(PadTck, 1'b1);
    #25ns;
    tdo = sample("cio_tdo_o");
    tdo_en = sample("cio_tdo_en_o");
    #25ns;
  endtask

  virtual task body();
    bit enabled;
    lc_ctrl_pkg::lc_tx_t nvm_dbg;
    bit [31:0] idcode;
    int unsigned tdo_en_cycles;
    void'($value$plusargs("eg2_rram_jtag_expect_tap=%d", expect_tap));
    // No super.body(): chip_stub_cpu_base_vseq has none, so it would reach cip_base_vseq::body,
    // a fatal "Need to override this" (like chip_rv_dm_lc_disabled_vseq, which does not call it).
    `DV_WAIT(cfg.chip_vif.lc_ready == 1'b1)
    enabled = lc_state inside {LcStTestUnlocked0, LcStTestUnlocked1, LcStTestUnlocked2,
                               LcStTestUnlocked3, LcStTestUnlocked4, LcStTestUnlocked5,
                               LcStTestUnlocked6};
    `uvm_info(`gfn, $sformatf("EG2 RRAM jtag gate: %s, NVM debug expected %s", lc_state.name(),
                              enabled ? "enabled" : "disabled"), UVM_LOW)
    // 1. The gate signal.
    begin
      uvm_hdl_data_t v;
      if (!(uvm_hdl_read({Macro, ".lc_nvm_debug_en_i"}, v))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({Macro, \".lc_nvm_debug_en_i\"}, v)) %s", "lc_nvm_debug_en_i"))
      nvm_dbg = lc_ctrl_pkg::lc_tx_t'(v[$bits(lc_ctrl_pkg::lc_tx_t)-1:0]);
    end
    `DV_CHECK_EQ(nvm_dbg == lc_ctrl_pkg::On, enabled,
                 $sformatf("lc_nvm_debug_en at rram_macro in %s", lc_state.name()))
    // 2. The pins through pinmux.
    // tl_access's data is inout: it needs variables.
    begin
      bit [31:0] sel;
      sel = InselMioBase + PadTck;
      tl_access(.addr(Pinmux + InselOff + 4 * InTck), .write(1), .data(sel));
      sel = InselMioBase + PadTms;
      tl_access(.addr(Pinmux + InselOff + 4 * InTms), .write(1), .data(sel));
      sel = InselMioBase + PadTdi;
      tl_access(.addr(Pinmux + InselOff + 4 * InTdi), .write(1), .data(sel));
      sel = OutTdo;
      tl_access(.addr(Pinmux + OutselOff + 4 * PadTdo), .write(1), .data(sel));
    end
    cfg.chip_vif.mios_if.drive_en_pin(PadTck, 1'b1);
    cfg.chip_vif.mios_if.drive_en_pin(PadTms, 1'b1);
    cfg.chip_vif.mios_if.drive_en_pin(PadTdi, 1'b1);
    for (int v = 0; v < 2; v++) begin
      cfg.chip_vif.mios_if.drive_pin(PadTck, v);
      cfg.chip_vif.mios_if.drive_pin(PadTms, !v);
      cfg.chip_vif.mios_if.drive_pin(PadTdi, v);
      #100ns;
      if (!((sample("cio_tck_i")) == (v))) `uvm_error(`gfn, $sformatf("Check failed sample(\"cio_tck_i\") == v (0x%0h vs 0x%0h) %s", sample("cio_tck_i"), v, "TCK pad not at rram_macro"))
      if (!((sample("cio_tms_i")) == (!v))) `uvm_error(`gfn, $sformatf("Check failed sample(\"cio_tms_i\") == !v (0x%0h vs 0x%0h) %s", sample("cio_tms_i"), !v, "TMS pad not at rram_macro"))
      if (!((sample("cio_tdi_i")) == (v))) `uvm_error(`gfn, $sformatf("Check failed sample(\"cio_tdi_i\") == v (0x%0h vs 0x%0h) %s", sample("cio_tdi_i"), v, "TDI pad not at rram_macro"))
    end
    // 3./4. A TAP reset, then Shift-DR from Run-Test/Idle (IDCODE is the reset instruction).
    for (int i = 0; i < 6; i++) begin
      logic tdo, tdo_en;
      tck_cycle(1, 0, tdo, tdo_en);
      if (tdo_en) tdo_en_cycles++;
    end
    begin
      // Run-Test/Idle, Select-DR, Capture-DR, Shift-DR.
      bit tms_seq[4] = '{0, 1, 0, 0};
      logic tdo, tdo_en;
      foreach (tms_seq[i]) begin
        tck_cycle(tms_seq[i], 0, tdo, tdo_en);
        if (tdo_en) tdo_en_cycles++;
      end
      for (int i = 0; i < 32; i++) begin
        tck_cycle(i == 31, 0, tdo, tdo_en);
        idcode[i] = tdo;
        if (tdo_en) tdo_en_cycles++;
      end
    end
    if (!enabled) begin
      uvm_hdl_data_t a;
      `DV_CHECK_EQ(tdo_en_cycles, 0, $sformatf("rram_macro drove TDO in %s", lc_state.name()))
      if (uvm_hdl_check_path("tb.dut.RRAM_ANALOG") && uvm_hdl_read("tb.dut.RRAM_ANALOG", a)) begin
        `DV_CHECK_CASE_EQ(a[0], 1'bz, $sformatf("the RRAM analog pad is driven in %s",
                                                lc_state.name()))
      end else begin
        `uvm_info(`gfn, "no tb.dut.RRAM_ANALOG net to sample: analog pad not checked", UVM_LOW)
      end
    end else if (expect_tap) begin
      `DV_CHECK(idcode[0] == 1'b1 && idcode != '1,
                $sformatf("no IDCODE from the rram_macro TAP in %s (0x%08h, TDO enabled %0d cycles)",
                          lc_state.name(), idcode, tdo_en_cycles))
    end
  endtask

endclass : chip_eg2_rram_jtag_lc_gate_vseq
