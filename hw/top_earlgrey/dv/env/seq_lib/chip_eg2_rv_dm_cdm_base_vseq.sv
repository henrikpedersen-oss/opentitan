// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of chip_sw_rv_dm_cheriot_mode_switch, chip_eg2_rv_dm_reg_tags and
// chip_rv_dm_cheriot_progbuf_cap (EG2 spec REQ_CDM_04, REQ_CDM_08, REQ_CDM_09; "Debug Controller").
// Software in sw/device/tests/eg2_debug (the stage protocol of eg2_dm_cap.h). What it adds to
// chip_eg2_rv_dm_cap_base_vseq:
//  - aarsize 3 abstract commands (rv_dm theory_of_operation.md "CHERIoT Mode": a capability moved
//    without its tag, address in data0, metadata in data1; GPR regno 0x1000-0x101f, SCR regno
//    0x0-0x1f; in base mode aarsize 3 is cmderr 2, REQ_CDM_03);
//  - the mode check at a halt: the core's mode (RVFI cheriot_ena) and the debug module's
//    (rv_dm's cheriot_ena_i port, MuBi4True = CHERIoT) both equal the mode the documents give
//    for the phase (the bench's form of the spec's Eg2ModeConsistent_A, which does not exist yet);
//  - one single step (dcsr.step, RISC-V Debug Specification 0.13.2 4.8.1);
//  - the ndmreset helpers (rstmgr: the reset reason after an NDM reset is 0x80; the debug module
//    stays active).
class chip_eg2_rv_dm_cdm_base_vseq extends chip_eg2_rv_dm_cap_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_cdm_base_vseq)
  `uvm_object_new

  localparam bit [2:0] CmdErrNotSupported = 3'd2;
  localparam bit [4:0] XA3 = 5'd13, XA5 = 5'd15, XS0 = 5'd8, XS1 = 5'd9, XSp = 5'd2;
  localparam int unsigned ParkSpan = 12;  // eg2_dmx_park's wait loop: three instructions
  localparam string RvDmHier = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_dm";

  // ---------------------------------------------------------------- encodings

  // B-type: bne rs1, rs2, imm (imm a multiple of 2, -4096..4094).
  protected function bit [31:0] enc_bne(bit [4:0] rs1, bit [4:0] rs2, bit [12:0] imm);
    return {imm[12], imm[10:5], rs2, rs1, 3'b001, imm[4:1], imm[11], 7'h63};
  endfunction

  // ---------------------------------------------------------------- data1 and aarsize 3

  protected task data1_wr(bit [31:0] v);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(v), .blocking(1), .predict(1));
  endtask

  protected task data1_rd(output bit [31:0] v);
    uvm_reg_data_t d;
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(d), .blocking(1));
    v = d[31:0];
  endtask

  // aarsize 3 read: cmderr, and the address (data0) and metadata (data1) when it is 0.
  protected task cap_rd(bit [15:0] regno, output bit [31:0] addr, output bit [31:0] meta,
                        output bit [2:0] cmderr);
    abs_cmd(reg_cmd(regno, 1'b0, 1'b1, 1'b0, 3'd3), cmderr);
    addr = '0;
    meta = '0;
    if (cmderr == CmdErrNone) begin
      data0_rd(addr);
      data1_rd(meta);
    end
  endtask

  // aarsize 3 write of address `addr` and metadata `meta`.
  protected task cap_wr(bit [15:0] regno, bit [31:0] addr, bit [31:0] meta,
                        output bit [2:0] cmderr);
    data0_wr(addr);
    data1_wr(meta);
    abs_cmd(reg_cmd(regno, 1'b1, 1'b1, 1'b0, 3'd3), cmderr);
  endtask

  // A program buffer run whose cmderr is returned, not checked, and cleared if set.
  protected task try_progbuf(string what, bit [31:0] words[$], output bit [2:0] cmderr);
    run_progbuf(what, words, cmderr);
    if (cmderr != 0) cmderr_clear();
  endtask

  // ---------------------------------------------------------------- mode

  // rv_dm's cheriot_ena_i port: 1 for MuBi4True (the only value that selects CHERIoT mode).
  protected function bit dm_port_cheriot();
    uvm_hdl_data_t v;
    string path = {RvDmHier, ".cheriot_ena_i"};
    if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
    return v[3:0] == 4'(prim_mubi_pkg::MuBi4True);
  endfunction

  // At a halt: the core's mode at debug entry and the debug module's both `exp_cheriot`.
  protected function void check_mode(string what, bit exp_cheriot);
    chk_eq({what, ": the core's mode at the halt (1 CHERIoT)"}, entry_cheriot, exp_cheriot);
    chk_eq({what, ": the debug module's mode (cheriot_ena_i MuBi4True)"}, dm_port_cheriot(),
           exp_cheriot);
    chk_eq({what, ": the debug module's mode = the core's"}, dm_port_cheriot(),
           rvfi.cheriot_ena === 1'b1);
  endfunction

  // ---------------------------------------------------------------- step

  // One step with dcsr.step set, from a halt; the new halt checked (cause step). dcsr.step is
  // cleared again.
  protected task step_once(string what);
    rv_core_csr_dcsr_t dcsr;
    int unsigned n0 = debug_entries, nd0 = nd_retired;
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b1;
    cfg.debugger.write_dcsr(dcsr);
    cfg.debugger.set_resumereq(1);
    wait_debug_entry(n0, what);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_resumereq(0);
    chk_eq({what, ": instructions retired by one step"}, nd_retired - nd0, 1);
    gpr_sample = 4;
    check_entry(what, RvDebugCauseStep, 1'b1);
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b0;
    cfg.debugger.write_dcsr(dcsr);
  endtask

  // ---------------------------------------------------------------- park

  // Starts phase `arg` of the software and halts the core in its wait loop.
  protected task park_and_halt(string what, bit [7:0] arg, output bit ok);
    phase_begin(arg, ok);
    if (!ok) return;
    wait_parked(what, ok);
    if (!ok) return;
    gpr_sample = 0;
    halt_parked({what, ": halt"}, ParkSpan);
  endtask

  // Resumes and ends the phase (stop word, end line with verdict 1).
  protected task resume_and_end(string what, output bit ok);
    resume_plain({what, ": resume"});
    end_cmd(1'b1, ok);
  endtask

  // ---------------------------------------------------------------- ndmreset

  // ndmreset pulsed (with haltreq held across it if `halt`); waits for the reboot's lines unless
  // `halt` (the caller resumes first).
  protected task ndm_reset(string what, bit halt, output bit ok);
    uvm_reg_data_t v;
    int unsigned c0 = core_resets, n0 = debug_entries;
    ok = 1'b1;
    cfg.debugger.set_ndmreset(1);
    if (halt) set_haltreq(1);
    cfg.clk_rst_vif.wait_clks(20);
    cfg.debugger.set_ndmreset(0);
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmcontrol), .value(v), .blocking(1));
    chk_eq({what, ": dmcontrol.dmactive kept across ndmreset"},
           get_field_val(cfg.jtag_dmi_ral.dmcontrol.dmactive, v), 1);
    if (halt) begin
      cfg.debugger.wait_cpu_halted();
      set_haltreq(0);
      wait_debug_entry(n0, what);
      chk_true({what, ": a core reset seen before the halt"}, core_resets > c0);
    end
  endtask

  // After an ndmreset: the boot and ready lines, the reset reason NDM, havereset acknowledged.
  protected task ndm_reboot(string what, int unsigned b0, int unsigned r0, output bit ok);
    wait_boot_ready(b0, r0, ok);
    if (!ok) return;
    chk_eq({what, ": reset reason after ndmreset (NDM only, rstmgr RESET_INFO)"}, boot_info,
           32'h80);
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
  endtask

endclass : chip_eg2_rv_dm_cdm_base_vseq
