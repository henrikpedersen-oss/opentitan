// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_lockstep (EG2 spec REQ_CDM_08, REQ_CIB_03; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_lockstep.c), both modes.
//
// The documents: Ibex security.rst "Dual core lockstep" (the shadow core runs on delayed inputs,
// "All outputs of the shadow core are compared against a delayed version of the outputs of the
// main core. Any mismatch between the two sets of outputs will trigger an internal major alert");
// rv_core_ibex registers.md ERR_STATUS.FATAL_CORE_ERR ("alert_major_internal_o from Ibex seen")
// and theory_of_operation.md "Local Escalation Path" (on fatal_hw_err fetch is disabled until
// rv_core_ibex is reset); rstmgr "System Reset Tree" (an ndmreset in TEST, DEV and RMA resets all
// but the debug module).
//
// 1. Fault-free debug, n_rounds rounds while the core runs PARK: a halt (chip_eg2_rv_dm_corners
//    base: dcsr, dpc, every GPR through abstract commands), abstract writes and reads of a0 and t1
//    (PARK leaves them free), a program buffer `addi a0, a0, 1` (postexec) whose result is read
//    back, n_steps single steps (dcsr.step: one instruction each, dcsr.cause 4), a0 and t1
//    restored, resume (the first instruction at dpc, the other registers unchanged). After each
//    round ERR_STATUS over SBA has FATAL_CORE_ERR 0, and during the whole phase the alert agent
//    of rv_core_ibex_fatal_hw_err sees no alert. PARK's end line carries the software's checks;
//    then ALERT_CHECK: the software finds no fatal_hw_err cause and FATAL_CORE_ERR 0.
// 2. The glitch: PARK again, a halt, then one output of the shadow core (drawn among core_busy_o,
//    instr_req_o, instr_addr_o, data_req_o, data_we_o, data_addr_o; one bit) inverted for
//    glitch_cycles cycles while the core sits in debug mode. Expected: fatal_hw_err at the alert
//    agent within AlertWaitClks core cycles; ERR_STATUS.FATAL_CORE_ERR 1 over SBA; the alert
//    handler's cause for it set; with resumereq the core retires no instruction outside debug mode
//    (fetch disabled).
// 3. ndmreset: the software boots again (reset reason NDM), ERR_STATUS 0 over SBA, no
//    fatal_hw_err for AlertQuietClks cycles, ALERT_CHECK passes; the done word ends the run.
// The glitch is announced to the injection registry ("insn": the model cannot follow it).
// Whether the comparator is on is not read: the glitch's alert in step 2 shows it was, in the
// same debug session as step 1.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md).
class chip_eg2_rv_dm_lockstep_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_lockstep_vseq)
  `uvm_object_new

  localparam bit [7:0] KindPark = 8'd9;
  localparam bit [7:0] ActionAlertCheck = 8'h10;
  localparam int unsigned TestId = 32'h41;
  localparam string AlertName = "rv_core_ibex_fatal_hw_err";
  localparam string ShadowCore =
      "tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_core.gen_lockstep.u_ibex_lockstep.u_shadow_core";
  // No document gives the alert's latency; a bound well above the lockstep delay and the alert
  // handshake.
  localparam int unsigned AlertWaitClks = 200;
  localparam int unsigned AlertQuietClks = 2000;
  localparam bit [31:0] ErrStatusAddr =
      top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR + 32'h54;
  localparam int unsigned FatalCoreErrBit = 9;
  localparam int unsigned NumPorts = 6;

  rand int unsigned n_rounds;
  rand int unsigned n_steps;
  rand bit [31:0] a0_val, t1_val;
  rand int unsigned port_sel;
  rand int unsigned addr_bit;
  rand int unsigned glitch_cycles;

  constraint knobs_c {
    n_rounds inside {[1:3]};
    n_steps inside {[1:6]};
    port_sel < NumPorts;
    addr_bit < 32;
    glitch_cycles inside {[1:3]};
  }

  protected int unsigned alert_cycles;
  protected bit alert_monitor_on;

  protected function string port_name(int unsigned sel);
    case (sel)
      0: return "core_busy_o";
      1: return "instr_req_o";
      2: return "instr_addr_o";
      3: return "data_req_o";
      4: return "data_we_o";
      default: return "data_addr_o";
    endcase
  endfunction

  protected task alert_monitor();
    forever begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      if (alert_monitor_on && cfg.m_alert_agent_cfgs[AlertName].vif.get_alert()) alert_cycles++;
    end
  endtask

  protected task err_status_chk(string what, bit exp, bit primary = 1'b0);
    bit [31:0] v;
    sba_access_err_e st;
    sba_rd32(ErrStatusAddr, v, st);
    chk_eq({what, ": SBA read of ERR_STATUS"}, st, SbaErrNone);
    chk_eq({what, ": ERR_STATUS.FATAL_CORE_ERR"}, v[FatalCoreErrBit], exp, primary);
  endtask

  protected task abs_rw(bit [15:0] regno, bit [31:0] v, string what);
    bit [2:0] err;
    bit [31:0] rb;
    gpr_wr(regno, v, err);
    chk_eq({what, ": abstract write cmderr"}, err, 0);
    gpr_rd(regno, rb, err);
    chk_eq({what, ": abstract read cmderr"}, err, 0);
    chk_eq({what, ": read back"}, rb, v);
  endtask

  // One step: one instruction retired outside debug mode, then debug mode with cause 4.
  protected task step_checked(string what);
    int unsigned n0 = debug_entries, nd0 = nd_retired;
    cfg.debugger.set_resumereq(1);
    wait_debug_entry(n0, what);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_resumereq(0);
    chk_eq({what, ": instructions retired by one step"}, nd_retired - nd0, 1);
    gpr_sample = 6;
    check_entry(what, RvDebugCauseStep, 1'b1);
  endtask

  // Resume: the first instruction at dpc, in the same privilege, every register but a0 and t1
  // (which the debugger wrote and restored as integers) as at the halt.
  protected task resume_light(string what);
    int unsigned r0 = resumes;
    cfg.debugger.set_resumereq(1);
    cfg.debugger.wait_cpu_resumed();
    cfg.debugger.set_resumereq(0);
    fork : isolation_fork
      begin
        fork
          wait (resumes > r0);
          #(1ms);
        join_any
        disable fork;
      end
    join
    chk_true({what, ": an instruction retired after resume"}, resumes > r0);
    if (resumes <= r0) return;
    chk_eq({what, ": the first instruction after resume is at dpc"}, post_pc, last_dpc);
    chk_eq({what, ": privilege after resume"}, post_mode, entry_valid ? entry_mode : 2'b11);
    foreach (snap[r]) begin
      if (r == 0 || r == 6 || r == 10 || !snap[r].valid) continue;
      chk_eq($sformatf("%s: x%0d after the debug session", what, r), post_snap[r].data,
             snap[r].data);
      chk_eq($sformatf("%s: x%0d capability metadata (tag) after the debug session", what, r),
             post_snap[r].cap, snap[r].cap);
    end
  endtask

  protected task fault_free_round(int unsigned r);
    string what = $sformatf("lockstep round %0d", r);
    bit [31:0] a0_orig, t1_orig, v;
    bit [2:0] err;
    rv_core_csr_dcsr_t dcsr;
    halt_checked({what, ": halt"});
    gpr_rd(RegA0, a0_orig, err);
    gpr_rd(RegT1, t1_orig, err);
    abs_rw(RegA0, a0_val + r, {what, ": a0"});
    abs_rw(RegT1, t1_val ^ r, {what, ": t1"});
    // Program buffer: addi a0, a0, 1 ; ebreak, run by a postexec command without a transfer.
    progbuf_wr({InsnAddiA0, InsnEbreak});
    abs_cmd(reg_cmd(RegA0, 1'b0, 1'b0, 1'b1), err);
    chk_eq({what, ": program buffer cmderr"}, err, 0);
    gpr_rd(RegA0, v, err);
    chk_eq({what, ": a0 after the program buffer's addi"}, v, a0_val + r + 1);
    gpr_wr(RegA0, a0_orig, err);
    gpr_wr(RegT1, t1_orig, err);
    // Single steps.
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b1;
    cfg.debugger.write_dcsr(dcsr);
    for (int unsigned i = 0; i < n_steps; i++) step_checked($sformatf("%s step %0d", what, i));
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b0;
    cfg.debugger.write_dcsr(dcsr);
    resume_light({what, ": resume"});
    err_status_chk(what, 1'b0);
  endtask

  protected task glitch(output bit fired);
    string port = port_name(port_sel);
    string path = {ShadowCore, ".", port};
    uvm_hdl_data_t v;
    uvm_reg_data_t cause;
    int unsigned bit_idx = (port_sel inside {2, 5}) ? addr_bit : 0;
    int unsigned nd0, a0;
    halt_checked("lockstep glitch: halt");
    if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
    v[bit_idx] = ~v[bit_idx];
    void'(eg2_inject_registry::announce({"bench fault at ", path}, "insn",
                                        $sformatf("shadow core output bit %0d inverted for %0d cycles",
                                                  bit_idx, glitch_cycles),
                                        $realtime, eg2_inject_registry::UntilConsumed, 1'b0, 1,
                                        1'b1));
    `uvm_info(`gfn, $sformatf("EG2 DM lockstep: %s bit %0d inverted for %0d cycles", path,
                              bit_idx, glitch_cycles), UVM_LOW)
    a0 = alert_cycles;
    if (!uvm_hdl_force(path, v)) `uvm_fatal(`gfn, {"cannot force ", path})
    cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(glitch_cycles);
    if (!uvm_hdl_release(path)) `uvm_fatal(`gfn, {"cannot release ", path})
    for (int unsigned i = 0; i < AlertWaitClks && alert_cycles == a0; i++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
    end
    fired = alert_cycles > a0;
    chk_true($sformatf("lockstep glitch (%s): %s within %0d cycles", port, AlertName,
                       AlertWaitClks), fired);
    err_status_chk("lockstep glitch", 1'b1, 1'b1);
    csr_rd(.ptr(ral.alert_handler.alert_cause[top_earlgrey_pkg::TopEarlgreyAlertIdRvCoreIbexFatalHwErr]),
           .value(cause), .backdoor(1));
    chk_eq("lockstep glitch: the alert handler's cause for fatal_hw_err", cause[0], 1);
    // Fetch disabled: nothing retires outside debug mode after a resume request.
    nd0 = nd_retired;
    cfg.debugger.set_resumereq(1);
    #(20us);
    cfg.debugger.set_resumereq(0);
    chk_eq("lockstep glitch: instructions retired after resumereq (fetch disabled)",
           nd_retired - nd0, 0);
  endtask

  virtual task body();
    bit ok, fired;
    int unsigned b0, r0, c0, a0;
    super.body();
    if (!cfg.m_alert_agent_cfgs.exists(AlertName)) `uvm_fatal(`gfn, {"no alert agent ", AlertName})
    fork
      alert_monitor();
    join_none
    corners_ready(ok);
    if (!ok) return;
    alert_monitor_on = 1'b1;
    `uvm_info(`gfn, $sformatf("EG2 DM lockstep: %s mode, %0d rounds, %0d steps, glitch %s",
                              rvfi.cheriot_ena === 1'b1 ? "CHERIoT" : "RISC-V", n_rounds, n_steps,
                              port_name(port_sel)), UVM_LOW)
    // 1. Fault-free debug.
    send_cmd(KindPark, 0, ok);
    if (!ok) return;
    for (int unsigned r = 0; r < n_rounds; r++) begin
      fault_free_round(r);
      if (sw_ended()) return;
    end
    end_cmd(1'b1, ok);
    if (!ok) return;
    send_cmd(ActionAlertCheck, 0, ok);
    if (!ok) return;
    end_cmd(1'b0, ok);
    if (!ok) return;
    chk_eq("fault-free debug: fatal_hw_err alert cycles", alert_cycles, 0);
    // 2. The glitch while halted.
    send_cmd(KindPark, 0, ok);
    if (!ok) return;
    glitch(fired);
    // 3. ndmreset, the second boot.
    b0 = boots;
    r0 = readies;
    c0 = core_resets;
    cfg.debugger.set_ndmreset(1);
    cfg.clk_rst_vif.wait_clks(20);
    cfg.debugger.set_ndmreset(0);
    wait (core_resets > c0);
    wait_boot_ready(b0, r0, ok);
    if (!ok) return;
    chk_eq("after ndmreset: reset reason NDM (rstmgr RESET_INFO bit 7)", boot_info, 32'h80);
    err_status_chk("after ndmreset", 1'b0);
    a0 = alert_cycles;
    cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(AlertQuietClks);
    chk_eq("after ndmreset: fatal_hw_err alert cycles", alert_cycles - a0, 0);
    send_cmd(ActionAlertCheck, 0, ok);
    if (!ok) return;
    end_cmd(1'b0, ok);
    if (!ok) return;
    finish(TestId);
  endtask

endclass : chip_eg2_rv_dm_lockstep_vseq
