// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_lc_drop (EG2 spec REQ_CDM_05, REQ_SEC_04; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_lc_drop.c), both modes.
//
// Escalation withdraws lc_hw_debug_en during a debug session. The software arms an escalation
// that cannot be stopped (action ARM: rv_core_ibex's fatal_sw_err in class A; phase 0 lasts
// `esc_delay` alert-handler cycles, phase 1 drives lc_ctrl's escalation, phase 3 the escalation
// reset). Between the armed line and the drop the bench holds the drawn session:
//  - halted: the core halted (haltreq);
//  - sba: SBA accesses back to back (writes across the always-on CDC, which take microseconds,
//    and reads of the scratch area), so one is in flight at the drop;
//  - abstract: the core halted and a long program-buffer loop started again and again.
// Expected after the drop (lc_ctrl theory_of_operation "Security Escalation" and its function
// table: ESCALATE behaves as SCRAP, HW_DEBUG_EN and CPU_EN off; pinmux "Non-debug Module Reset":
// the sampled lc_hw_debug_en is invalidated on escalation, and rv_dm's TAP gating always consumes
// the live signal; rv_dm interfaces.md DM_EN.CTRL.LC_GATED: the DMI, SBA, debug request and debug
// ROM are gated):
//  - lc_cpu_en is off as well;
//  - JTAG: sbaddress0 written over the DMI reads back 0, and dmstatus reads 0;
//  - no request on rv_dm's SBA port, no debug_req_i at the core, no instruction retired (the
//    core does not run: CPU_EN), over a 20 us window after a settle time, during which the bench
//    also attempts an SBA write;
//  - the escalation reset follows; boot 2 reports it (rstmgr RESET_INFO bit 6, dif_rstmgr
//    kDifRstmgrResetInfoEscalation, alone) and the debug module works again (the done word over
//    SBA ends the test).
// Late debug enable (DEV with OTP DIS_RV_DM_LATE_DEBUG not MuBi8True): `late_delayed` makes the
// software write LATE_DEBUG_ENABLE only after `EG2 DEBUG late pending`; until then (rv_dm theory
// of operation, "Late Debug Enable Mechanism": reachable via TAP, not fully ungated) the DMI
// answers (sbaddress0 read back), but haltreq raises no debug_req_i and halts nothing, and an SBA
// read makes no request on rv_dm's SBA port. With OTP's MuBi8True the module is enabled from the
// start (late_debug_sw 0, late_delayed 0).
//
// EXPECTED FAILURE in CHERIoT mode for the halted and abstract sessions until ticket F13
// (status/rtl_tickets.md: rv_dm has no CHERIoT input); the sba session does not halt the core.
class chip_eg2_rv_dm_lc_drop_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_lc_drop_vseq)
  `uvm_object_new

  localparam bit [31:0] AonThold = top_earlgrey_pkg::TOP_EARLGREY_AON_TIMER_BASE_ADDR + 32'hc;

  typedef enum int unsigned {SessHalted, SessSba, SessAbstract} session_e;

  rand session_e session;
  rand bit late_delayed;
  rand int unsigned esc_delay;
  rand bit [31:0] probe_vals[3];

  // Overrides the base's: in DEV without OTP's late-debug fuse the module is enabled either at
  // boot (late_debug_sw) or later (late_delayed); with the fuse, or in the other states, by
  // neither.
  constraint late_debug_c {
    solve lc_state before dis_late_debug_otp, late_debug_sw, late_delayed;
    (lc_state != LcStDev) -> (!late_debug_sw && !late_delayed);
    (lc_state == LcStDev && dis_late_debug_otp) -> (!late_debug_sw && !late_delayed);
    (lc_state == LcStDev && !dis_late_debug_otp) -> (late_debug_sw != late_delayed);
  }
  constraint esc_c {
    esc_delay inside {[1200:4800]};
  }

  protected bit dropped;
  protected realtime t_drop;

  virtual function void write_params();
    super.write_params();
    set_param("late_delayed", late_delayed);
    set_param("esc_delay", esc_delay);
  endfunction

  protected function bit hw_debug_en_on();
    return cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalHwDebugEn) == lc_ctrl_pkg::On;
  endfunction

  // DEV before LATE_DEBUG_ENABLE: the DMI answers, the debug request and SBA are gated.
  protected task gated_checks();
    bit [31:0] a;
    uvm_reg_data_t v;
    bit req_seen, sba_seen;
    sbaddr_wr(probe_vals[0] & ~32'h3);
    sbaddr_rd(a);
    chk_eq("late debug pending: DMI reachable (sbaddress0 read back)", a, probe_vals[0] & ~32'h3);
    cfg.debugger.set_haltreq(1);
    for (int i = 0; i < 2000; i++) begin
      #(10ns);
      req_seen |= probe("debug_req_i");
    end
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    cfg.debugger.set_haltreq(0);
    chk_true("late debug pending: haltreq raises no debug_req_i", !req_seen);
    chk_eq("late debug pending: dmstatus.anyhalted", get_field_val(
           cfg.jtag_dmi_ral.dmstatus.anyhalted, v), 0);
    sbcs_wr(3'd2, 1'b0, 1'b1, 1'b0);
    sbaddr_wr(top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR);
    for (int i = 0; i < 2000; i++) begin
      #(10ns);
      sba_seen |= sba_a_valid();
    end
    chk_true("late debug pending: an SBA read makes no bus request", !sba_seen);
    sbcs_rd(v);
    `uvm_info(`gfn, $sformatf("EG2 DM late debug pending: sbcs 0x%08x after the gated read", v),
              UVM_LOW)
  endtask

  protected task wait_drop(realtime timeout);
    realtime t_end = $realtime + timeout;
    while (hw_debug_en_on() && $realtime < t_end) #(100ns);
    dropped = 1'b1;
    t_drop = $realtime;
    chk_true("the escalation withdrew lc_hw_debug_en", !hw_debug_en_on());
  endtask

  protected task run_session();
    bit [2:0] serr, err;
    bit [31:0] rdata;
    uvm_reg_data_t v;
    case (session)
      SessHalted: begin
        cfg.debugger.set_haltreq(1);
        cfg.debugger.wait_cpu_halted();
        cfg.debugger.set_haltreq(0);
      end
      SessSba: begin
        for (int unsigned i = 0; !dropped; i++) begin
          sba_raw(1'b1, AonThold, 3'd2, i, serr, rdata);
          if (!dropped) sba_raw(1'b0, scratch_addr, 3'd2, 0, serr, rdata);
        end
      end
      default: begin
        cfg.debugger.set_haltreq(1);
        cfg.debugger.wait_cpu_halted();
        cfg.debugger.set_haltreq(0);
        progbuf_wr({InsnLuiT1, InsnAddiT1M1, InsnBnezT1, InsnEbreak});
        while (!dropped) begin
          csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1)),
                 .blocking(1), .predict(1));
          do begin
            csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(v), .blocking(1));
          end while (!dropped && get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, v));
        end
      end
    endcase
  endtask

  protected task after_drop_checks();
    uvm_reg_data_t v;
    int unsigned r0;
    bit sba_seen, req_seen;
    #(5us);
    chk_eq("lc_cpu_en after the escalation (ESCALATE disables CPU_EN)",
           cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalCpuEn) == lc_ctrl_pkg::On, 0);
    r0 = retired;
    fork
      begin
        for (int i = 0; i < 2000; i++) begin
          #(10ns);
          sba_seen |= sba_a_valid();
          req_seen |= probe("debug_req_i");
        end
      end
      begin
        csr_wr(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(probe_vals[1]), .blocking(1),
               .predict(0));
        csr_rd(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(v), .blocking(1),
               .check(UVM_NO_CHECK));
        chk_eq("after the drop: sbaddress0 over JTAG reads 0", v, 0, 1'b1);
        csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1), .check(UVM_NO_CHECK));
        chk_eq("after the drop: dmstatus over JTAG reads 0", v, 0);
        // An SBA write attempt: sbcs, sbaddress0, sbdata0.
        csr_wr(.ptr(cfg.jtag_dmi_ral.sbcs), .value(32'h0004_0000), .blocking(1), .predict(0));
        csr_wr(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(scratch_addr), .blocking(1),
               .predict(0));
        csr_wr(.ptr(cfg.jtag_dmi_ral.sbdata0), .value(probe_vals[2]), .blocking(1), .predict(0));
      end
    join
    chk_true("after the drop: no request on rv_dm's SBA port", !sba_seen);
    chk_true("after the drop: no debug_req_i at the core", !req_seen);
    chk_eq("after the drop: instructions retired (CPU_EN off)", retired - r0, 0);
  endtask

  virtual task body();
    bit ok;
    int unsigned c0;
    super.body();
    `uvm_info(`gfn, $sformatf("EG2 DM lc drop: %s, session %s, late delayed %0d, esc_delay %0d",
                              lc_state.name(), session.name(), late_delayed, esc_delay), UVM_LOW)
    if (late_delayed) begin
      wait_lines("EG2 DEBUG late pending", LineLatePending, 0, ok, 200_000);
      if (!ok) return;
      cfg.debugger.set_dmactive(1);
      gated_checks();
      wait_lines("EG2 DEBUG late enabled", LineLateEnabled, 0, ok);
      if (!ok) return;
      sbcs_wr(3'd2);
    end
    corners_ready(ok);
    if (!ok) return;
    send_cmd(8'h10, 0, ok);  // ARM
    if (!ok) return;
    wait_lines("EG2 DEBUG armed", LineArmed, 0, ok);
    if (!ok) return;
    c0 = core_resets;
    fork
      run_session();
      wait_drop(5ms);
    join
    `uvm_info(`gfn, $sformatf("EG2 DM lc drop: lc_hw_debug_en off at %0t", t_drop), UVM_LOW)
    after_drop_checks();
    // The escalation reset, boot 2.
    wait (core_resets > c0);
    fork
      cfg.m_jtag_riscv_agent_cfg.m_jtag_agent_cfg.vif.do_trst_n(2);
      wait (cfg.chip_vif.pinmux_lc_hw_debug_en);
    join
    cfg.debugger.set_dmactive(1);
    wait_boot_ready(1, 1, ok);
    if (!ok) return;
    chk_eq("boot 2: reset reason (escalation only)", boot_info, 32'h40);
    finish(32'h13);
  endtask

endclass : chip_eg2_rv_dm_lc_drop_vseq
