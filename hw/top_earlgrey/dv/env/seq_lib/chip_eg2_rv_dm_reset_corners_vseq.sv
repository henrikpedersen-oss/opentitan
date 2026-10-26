// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_reset_corners (EG2 spec REQ_CDM_04, REQ_PBF_20; "Debug Controller" / "Corner
// cases"; sw/device/tests/eg2_debug/eg2_rv_dm_reset_corners.c), both modes. The core runs the
// PARK kernel between the phases; every ndmreset reboots it.
//
// Phases, in a drawn order:
//  - ndm_halted: ndmreset while the core is halted.
//  - ndm_abstract: ndmreset while a long program buffer command runs.
//  - ndm_sba: ndmreset while an SBA write across the always-on CDC (aon_timer WKUP_THOLD_LO) is in
//    flight.
//  - dmactive: dmactive pulsed to 0 while the core is halted with module state set.
//  - reset_halt: setresethaltreq, then ndmreset without haltreq; then haltreq held across ndmreset.
// Expected (RISC-V Debug Specification 0.13.2; rv_dm's documentation; rstmgr's and pinmux's):
//  - ndmreset resets everything but the debug module and the TAP (spec 3.2, rstmgr "NDM RESET",
//    pinmux "Non-debug Module Reset"): dmactive, data1, progbuf1, sbaddress0 and sbcs's fields
//    keep their values; dmstatus.anyhavereset and allhavereset are set (spec 3.12.2: set when the
//    hart was reset, until ackhavereset); the software's next boot reports rstmgr's NDM reset
//    (RESET_INFO bit 7, dif_rstmgr kDifRstmgrResetInfoNdm) and not POR. The SBA write in flight
//    completes or errors and the register then holds its old value, the new one or its reset
//    value: never a mix. What becomes of an abstract command whose hart is reset the specification
//    does not state: busy and cmderr are logged, and the module is recovered with dmactive if
//    busy stays set (reported as a gap, not checked).
//  - dmactive = 0 (spec 3.12.2: "The module's state ... takes its reset values"): data0, progbuf0,
//    abstractauto, command's effect, sbaddress0, sbcs (sbaccess 2, the others 0), cmderr and
//    dmcontrol's haltreq and ndmreset read their reset values after it; the halted core is then
//    resumed and checked.
//  - Halt on reset: dmstatus.hasresethaltreq reads 0 and setresethaltreq is a no-op (rv_dm's
//    debug-system.md), so the core does not halt after an ndmreset with only setresethaltreq;
//    with haltreq held across ndmreset the core halts at its first instruction: dpc = the reset
//    vector {boot_addr[31:8], 0x80} (Ibex exception_interrupts.rst), dcsr.cause = 3, no
//    instruction retired before it, and base mode (REQ_CDM_04: ndmreset returns the core to base
//    mode; the CHERIoT run switches again when the software boots).
// The checked halts and resumes are those of chip_eg2_rv_dm_corners_base_vseq.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (rv_dm has no CHERIoT input): the halts of
// the running CHERIoT-mode software. The halt at the first instruction after reset is in base mode.
class chip_eg2_rv_dm_reset_corners_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_reset_corners_vseq)
  `uvm_object_new

  localparam bit [31:0] AonThold = top_earlgrey_pkg::TOP_EARLGREY_AON_TIMER_BASE_ADDR + 32'hc;

  typedef enum int unsigned {
    PhNdmHalted, PhNdmAbstract, PhNdmSba, PhDmactive, PhResetHalt, PhNum
  } phase_e;

  rand phase_e phases[PhNum];
  rand bit [31:0] vals[8];
  constraint phases_c {
    unique {phases};
  }

  // ndmreset pulsed; waits for the reboot's lines and checks the reset reason and the module
  // state kept across it (`d1`, `p1`, `sa`: data1, progbuf1, sbaddress0 written before).
  protected task ndm_pulse(string what, bit check_state, bit [31:0] d1, bit [31:0] p1,
                           bit [31:0] sa, output bit ok);
    int unsigned b0 = boots, r0 = readies;
    uvm_reg_data_t v;
    cfg.debugger.set_ndmreset(1);
    cfg.clk_rst_vif.wait_clks(20);
    cfg.debugger.set_ndmreset(0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmcontrol), .value(v), .blocking(1));
    chk_eq({what, ": dmcontrol.dmactive kept"}, get_field_val(cfg.jtag_dmi_ral.dmcontrol.dmactive,
                                                               v), 1);
    if (check_state) begin
      csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(v), .blocking(1));
      chk_eq({what, ": data1 kept"}, v, d1);
      csr_rd(.ptr(cfg.jtag_dmi_ral.progbuf[1]), .value(v), .blocking(1));
      chk_eq({what, ": progbuf1 kept"}, v, p1);
      csr_rd(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(v), .blocking(1));
      chk_eq({what, ": sbaddress0 kept"}, v, sa);
    end
    wait_boot_ready(b0, r0, ok);
    if (!ok) return;
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    chk_eq({what, ": dmstatus.anyhavereset"}, get_field_val(
           cfg.jtag_dmi_ral.dmstatus.anyhavereset, v), 1);
    chk_eq({what, ": dmstatus.allhavereset"}, get_field_val(
           cfg.jtag_dmi_ral.dmstatus.allhavereset, v), 1);
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
    chk_eq({what, ": reset reason after ndmreset (NDM only)"}, boot_info, 32'h80);
  endtask

  protected task set_state(bit [31:0] d1, bit [31:0] p1, bit [31:0] sa);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(d1), .blocking(1), .predict(1));
    csr_wr(.ptr(cfg.jtag_dmi_ral.progbuf[1]), .value(p1), .blocking(1), .predict(1));
    sbaddr_wr(sa);
  endtask

  protected task ph_ndm_halted(output bit ok);
    send_cmd(9, 0, ok);
    if (!ok) return;
    halt_checked("ndmreset while halted: halt");
    set_state(vals[0], vals[1], vals[2] & ~32'h3);
    ndm_pulse("ndmreset while halted", 1'b1, vals[0], vals[1], vals[2] & ~32'h3, ok);
  endtask

  protected task ph_ndm_abstract(output bit ok);
    uvm_reg_data_t st;
    bit [2:0] err;
    send_cmd(9, 0, ok);
    if (!ok) return;
    halt_checked("ndmreset during an abstract command: halt");
    progbuf_wr({InsnLuiT1, InsnAddiT1M1, InsnBnezT1, InsnEbreak});
    csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1)),
           .blocking(1), .predict(1));
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(st), .blocking(1));
    chk_true("the program buffer loop runs at the ndmreset (stimulus)",
             get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, st));
    // progbuf1 is the loop's own instruction here.
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(vals[3]), .blocking(1), .predict(1));
    sbaddr_wr(vals[4] & ~32'h3);
    ndm_pulse("ndmreset during an abstract command", 1'b1, vals[3], InsnAddiT1M1,
              vals[4] & ~32'h3, ok);
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(st), .blocking(1));
    `uvm_info(`gfn, $sformatf({"EG2 DM ndmreset during an abstract command: afterwards busy %0d ",
                               "cmderr %0d (not defined by the debug specification; unchecked)"},
                              get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, st),
                              get_field_val(cfg.jtag_dmi_ral.abstractcs.cmderr, st)), UVM_LOW)
    if (get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, st)) begin
      `uvm_info(`gfn, "EG2 DM: busy after ndmreset, the module recovered with dmactive", UVM_LOW)
      cfg.debugger.set_dmactive(0);
      cfg.debugger.set_dmactive(1);
    end else if (get_field_val(cfg.jtag_dmi_ral.abstractcs.cmderr, st) != 0) begin
      cmderr_clear();
    end
    abs_wait(err);
  endtask

  protected task ph_ndm_sba(output bit ok);
    bit [2:0] serr;
    bit [31:0] rdata, sa;
    uvm_reg_data_t v;
    int unsigned b0 = boots, r0 = readies;
    sba_raw(1'b1, AonThold, 3'd2, vals[5], serr, rdata);
    chk_eq("SBA write of the old value", serr, 0);
    sbcs_wr(3'd2);
    sbaddr_wr(AonThold);
    sbdata_wr(vals[6]);
    sbcs_rd(v);
    chk_true("the SBA write across the CDC is in flight at the ndmreset (stimulus)",
             get_field_val(cfg.jtag_dmi_ral.sbcs.sbbusy, v));
    cfg.debugger.set_ndmreset(1);
    cfg.clk_rst_vif.wait_clks(20);
    cfg.debugger.set_ndmreset(0);
    sba_wait(v);
    `uvm_info(`gfn, $sformatf("EG2 DM ndmreset during an SBA write: sberror %0d",
                              get_field_val(cfg.jtag_dmi_ral.sbcs.sberror, v)), UVM_LOW)
    sbaddr_rd(sa);
    chk_eq("sbaddress0 kept across ndmreset (no auto-increment)", sa, AonThold);
    wait_boot_ready(b0, r0, ok);
    if (!ok) return;
    chk_eq("reset reason after ndmreset (NDM only)", boot_info, 32'h80);
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
    sba_raw(1'b0, AonThold, 3'd2, 0, serr, rdata);
    chk_eq("SBA read back of the register", serr, 0);
    chk_true($sformatf({"the register written during ndmreset holds the old value 0x%08x, the ",
                        "new one 0x%08x or its reset value 0: 0x%08x"}, vals[5], vals[6], rdata),
             rdata inside {vals[5], vals[6], 32'h0});
  endtask

  protected task ph_dmactive(output bit ok);
    uvm_reg_data_t v;
    bit [2:0] err;
    send_cmd(9, 0, ok);
    if (!ok) return;
    halt_checked("dmactive during a session: halt");
    data0_wr(vals[7]);
    csr_wr(.ptr(cfg.jtag_dmi_ral.progbuf[0]), .value(InsnAddiA0), .blocking(1), .predict(1));
    sbcs_wr(3'd0, 1'b1, 1'b0, 1'b1);
    sbaddr_wr(scratch_addr);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractauto), .value(32'h1), .blocking(1), .predict(1));
    abs_cmd(reg_cmd(16'h1020, 1'b0), err);  // leaves cmderr 3
    cfg.debugger.set_dmactive(0);
    cfg.debugger.set_dmactive(1);
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[0]), .value(v), .blocking(1));
    chk_eq("data0 after dmactive 0", v, 0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.progbuf[0]), .value(v), .blocking(1));
    chk_eq("progbuf0 after dmactive 0", v, 0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractauto), .value(v), .blocking(1));
    chk_eq("abstractauto after dmactive 0", v, 0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(v), .blocking(1));
    chk_eq("abstractcs.cmderr after dmactive 0", get_field_val(
           cfg.jtag_dmi_ral.abstractcs.cmderr, v), 0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(v), .blocking(1));
    chk_eq("sbaddress0 after dmactive 0", v, 0);
    sbcs_rd(v);
    chk_eq("sbcs.sbaccess after dmactive 0", get_field_val(cfg.jtag_dmi_ral.sbcs.sbaccess, v), 2);
    chk_eq("sbcs.sbautoincrement after dmactive 0", get_field_val(
           cfg.jtag_dmi_ral.sbcs.sbautoincrement, v), 0);
    chk_eq("sbcs.sbreadondata after dmactive 0", get_field_val(
           cfg.jtag_dmi_ral.sbcs.sbreadondata, v), 0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmcontrol), .value(v), .blocking(1));
    chk_eq("dmcontrol.ndmreset after dmactive 0", get_field_val(
           cfg.jtag_dmi_ral.dmcontrol.ndmreset, v), 0);
    // The core: still in debug mode (RVFI) or released by the module's reset; halted again if
    // released, then resumed and checked.
    `uvm_info(`gfn, $sformatf("EG2 DM dmactive pulse: the core is %s",
                              in_debug ? "still in debug mode" : "running"), UVM_LOW)
    if (in_debug) begin
      cfg.debugger.wait_cpu_halted();
    end else begin
      halt_checked("dmactive during a session: halt again");
    end
    resume_checked("dmactive during a session: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task ph_reset_halt(output bit ok);
    uvm_reg_data_t v;
    int unsigned b0, r0, c0, n0;
    bit [31:0] exp_dpc;
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    chk_eq("dmstatus.hasresethaltreq (halt-on-reset not implemented)", get_field_val(
           cfg.jtag_dmi_ral.dmstatus.hasresethaltreq, v), 0);
    // setresethaltreq alone: a no-op, the core runs after the ndmreset.
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.setresethaltreq), .value(1), .blocking(1));
    ndm_pulse("ndmreset after setresethaltreq", 1'b0, 0, 0, 0, ok);
    if (!ok) return;
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    chk_eq("dmstatus.allhalted after ndmreset with setresethaltreq only", get_field_val(
           cfg.jtag_dmi_ral.dmstatus.allhalted, v), 0);
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.clrresethaltreq), .value(1), .blocking(1));
    // haltreq held across ndmreset: the core halts at its first instruction, in base mode.
    b0 = boots;
    r0 = readies;
    c0 = core_resets;
    n0 = debug_entries;
    cfg.debugger.set_ndmreset(1);
    cfg.debugger.set_haltreq(1);
    cfg.clk_rst_vif.wait_clks(20);
    cfg.debugger.set_ndmreset(0);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_haltreq(0);
    wait_debug_entry(n0, "halt at the first instruction after ndmreset");
    chk_true("a core reset seen before the halt", core_resets > c0);
    exp_dpc = {rvfi.boot_addr[31:8], 8'h80};
    chk_eq("the core's mode at the halt after reset (base)", entry_cheriot, 0);
    check_entry("halt at the first instruction after ndmreset", RvDebugCauseHaltReq, 1'b0,
                exp_dpc);
    resume_checked("halt at the first instruction after ndmreset: resume");
    wait_boot_ready(b0, r0, ok);
    if (!ok) return;
    chk_eq("reset reason after ndmreset (NDM only)", boot_info, 32'h80);
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
    foreach (phases[i]) begin
      `uvm_info(`gfn, $sformatf("EG2 DM reset corners: %s", phases[i].name()), UVM_LOW)
      case (phases[i])
        PhNdmHalted: ph_ndm_halted(ok);
        PhNdmAbstract: ph_ndm_abstract(ok);
        PhNdmSba: ph_ndm_sba(ok);
        PhDmactive: ph_dmactive(ok);
        default: ph_reset_halt(ok);
      endcase
      if (!ok || sw_ended()) return;
    end
    finish(32'h12);
  endtask

endclass : chip_eg2_rv_dm_reset_corners_vseq
