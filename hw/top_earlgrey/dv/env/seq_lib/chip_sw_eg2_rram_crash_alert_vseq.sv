// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_crash_alert (sw/device/tests/eg2_rram/eg2_rram_crash_alert.c; EG2 spec
// REQ_RRAM_23). The bench's half (theory_of_operation.md "RRAM Escalation & Disable"):
//   - `EG2 RRAM crash global`: rram_ctrl's lc_escalate_en input goes On; from then until rram_ctrl's
//     reset, every host_tl response is an error (counted; the core may have stopped, F19);
//   - `EG2 RRAM crash local`: one SCRATCH register read's command integrity corrupted for a cycle at
//     the register block's input (STD_FAULT_STATUS.reg_intg_err, a local escalation); then over SBA:
//     a host read of the RRAM is refused, a controller read returns no data, STD_FAULT_STATUS and
//     FAULT_STATUS still read, reg_intg_err set.
// The alert handler's escalation then resets the chip; the software's second boot checks the clean
// state (it recognises the escalation reset in RESET_INFO: no boot_phase knob).
class chip_sw_eg2_rram_crash_alert_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_crash_alert_vseq)

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  localparam string RramCtrl = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl";
  localparam bit [31:0] CoreBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR;
  localparam bit [31:0] HostBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR;

  rand int unsigned fault;
  rand int unsigned phase0_cycles;

  constraint knobs_c {
    fault dist {0 := 40, 1 := 40, 2 := 20};
    phase0_cycles inside {[10000:40000]};
  }

  virtual function void write_params();
    set_param("fault", fault);
    set_param("phase0_cycles", phase0_cycles);
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  protected task global_check();
    int unsigned ok_rsp, err_rsp;
    int unsigned t;
    for (t = 0; t < 400_000; t++) begin
      if (rd({RramCtrl, ".lc_escalate_en_i"}) == lc_ctrl_pkg::On) break;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    end
    `DV_CHECK_LT_FATAL(t, 400_000, "lc_escalate_en never reached rram_ctrl")
    // Until rram_ctrl's reset: every host response an error.
    while (rd({RramCtrl, ".rst_ni"}) == 1) begin
      if (rd({RramCtrl, ".host_tl_o.d_valid"}) == 1 && rd({RramCtrl, ".host_tl_i.d_ready"}) == 1)
          begin
        if (rd({RramCtrl, ".host_tl_o.d_error"}) == 1) err_rsp++;
        else ok_rsp++;
      end
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    end
    `DV_CHECK_EQ(ok_rsp, 0, "host_tl answered without an error after the global escalation")
    `uvm_info(`gfn, $sformatf("EG2 RRAM crash global: %0d host responses after escalation, all errors",
                              err_rsp), UVM_LOW)
  endtask

  protected task local_check();
    string tl = {RramCtrl, ".u_reg_core.tl_i"};
    int unsigned t;
    bit [31:0] data;
    sba_access_err_e st;
    // The next SCRATCH read: its command integrity flipped for its one cycle.
    for (t = 0; t < 200_000; t++) begin
      if (rd({tl, ".a_valid"}) == 1 &&
          rd({tl, ".a_address"}) == CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_SCRATCH_OFFSET) break;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    end
    `DV_CHECK_LT_FATAL(t, 200_000, "no SCRATCH read reached rram_ctrl")
    begin
      uvm_hdl_data_t intg = rd({tl, ".a_user.cmd_intg"});
      intg[0] = ~intg[0];
      // TODO(inject-registry): announce() reaches the registry only in EG2_PERIPH_SB builds.
      announce("chip_sw_eg2_rram_crash_alert_vseq", "rram_ctrl",
               "core_tl command integrity bit 0 flipped on one SCRATCH read");
      if (!(uvm_hdl_force({tl, ".a_user.cmd_intg"}, intg))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_force({tl, \".a_user.cmd_intg\"}, intg)) %s", tl))
      @(posedge cfg.chip_vif.cpu_clk_rst_if.clk);
      if (!(uvm_hdl_release({tl, ".a_user.cmd_intg"}))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_release({tl, \".a_user.cmd_intg\"})) %s", tl))
    end
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(100);
    sba_rd32(CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_STD_FAULT_STATUS_OFFSET, data, st);
    `DV_CHECK_EQ(st, SbaErrNone, "STD_FAULT_STATUS not readable after the local escalation")
    `DV_CHECK_EQ(data[0], 1'b1, "STD_FAULT_STATUS.reg_intg_err not set")
    sba_rd32(CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET, data, st);
    `DV_CHECK_EQ(st, SbaErrNone, "FAULT_STATUS not readable after the local escalation")
    sba_rd32(HostBase + 32'h1C0000, data, st);
    `DV_CHECK_NE(st, SbaErrNone, "an RRAM host read over SBA succeeded after the local escalation")
    // A controller read of one word: it must deliver no data (rd_fifo stays empty).
    sba_wr32(CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_ADDR_OFFSET, 32'h1C0000, st);
    sba_wr32(CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_CONTROL_OFFSET, 32'h1, st);
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(2000);
    sba_rd32(CoreBase + rram_ctrl_reg_pkg::RRAM_CTRL_STATUS_OFFSET, data, st);
    `DV_CHECK_EQ(st, SbaErrNone, "STATUS not readable after the local escalation")
    `DV_CHECK_EQ(data[1], 1'b1, "the read FIFO received data after the local escalation")
    `uvm_info(`gfn, "EG2 RRAM crash local: blocked, registers readable", UVM_LOW)
  endtask

  virtual task body();
    string line;
    super.body();
    wait_sw_line("EG2 RRAM crash ", line, 200_000_000);
    if (line == "") return;
    if (line == "EG2 RRAM crash global") begin
      global_check();
    end else if (line == "EG2 RRAM crash local") begin
      cfg.debugger.set_dmactive(1);
      local_check();
    end
    // The escalation reset follows; the software's second boot reports the verdict.
  endtask

endclass : chip_sw_eg2_rram_crash_alert_vseq
