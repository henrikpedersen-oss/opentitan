// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_counters (EG2 spec REQ_CDM_08; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_counters.c), both modes. The core runs the WFI kernel
// (rv_timer wakes it every `wfi_ticks` ticks) while the bench halts it n_halts times for
// `hold_us` each.
//
// Expected (Ibex cs_registers.rst dcsr: stopcount and stoptime are not implemented, "Other bit
// fields read as zero"; RISC-V Debug Specification 0.13.2 4.8.1: stopcount 0 "increment counters
// as usual", stoptime 0 "increment timers as usual"):
//  - dcsr.stopcount and dcsr.stoptime written 1 read 0;
//  - mcycle (read through abstract commands) advances across the hold by at least 90% of the
//    hold's core cycles; minstret does not go back;
//  - rv_timer's counter (TIMER_V_LOWER0, read over SBA) advances across the hold;
//  - no timer interrupt lost: the compare the kernel armed passes during the hold, so the core
//    leaves wfi at once after the resume (the wfi instruction's next one retires within
//    2 * wfi_ticks us), and the software's kernel check passes at the end.
// The halts and resumes are checked as in chip_eg2_rv_dm_corners_base_vseq.
//
// The spec row asks for stopcount and stoptime set and clear: Ibex implements neither, so only
// the clear behaviour is reachable (status/dv_todo.md).
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md).
class chip_eg2_rv_dm_counters_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_counters_vseq)
  `uvm_object_new

  localparam bit [15:0] RegMcycle = 16'h0b00;
  localparam bit [15:0] RegMinstret = 16'h0b02;
  localparam bit [31:0] TimerV = top_earlgrey_pkg::TOP_EARLGREY_RV_TIMER_BASE_ADDR + 32'h110;

  rand int unsigned n_halts;
  rand int unsigned hold_us[4];
  rand int unsigned wfi_ticks;

  constraint knobs_c {
    n_halts inside {[2:4]};
    wfi_ticks inside {[5:15]};
    foreach (hold_us[i]) hold_us[i] inside {[40:120]};
  }

  virtual function void write_params();
    super.write_params();
    set_param("wfi_ticks", wfi_ticks);
  endfunction

  protected task csr_rd_abs(bit [15:0] regno, string what, output bit [31:0] v);
    bit [2:0] err;
    gpr_rd(regno, v, err);
    chk_eq({what, " read: cmderr"}, err, 0);
  endtask

  virtual task body();
    bit ok;
    rv_core_csr_dcsr_t dcsr;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    send_cmd(5, 0, ok);  // WFI
    if (!ok) return;
    watch_times.delete();
    watch_pc = start_target + 8;  // the instruction after wfi retired: the core woke
    for (int unsigned h = 0; h < n_halts; h++) begin
      bit [31:0] cyc0, cyc1, ins0, ins1, tv0, tv1;
      bit [2:0] serr;
      int unsigned w0;
      realtime t0, t1, t_res;
      string what = $sformatf("counters halt %0d", h);
      gpr_sample = 4;
      halt_checked(what);
      cfg.debugger.read_dcsr(dcsr);
      dcsr.stopcount = 1'b1;
      dcsr.stoptime = 1'b1;
      cfg.debugger.write_dcsr(dcsr);
      cfg.debugger.read_dcsr(dcsr);
      chk_eq({what, ": dcsr.stopcount written 1 (not implemented: reads 0)"}, dcsr.stopcount, 0);
      chk_eq({what, ": dcsr.stoptime written 1 (not implemented: reads 0)"}, dcsr.stoptime, 0);
      csr_rd_abs(RegMcycle, "mcycle", cyc0);
      t0 = $realtime;
      csr_rd_abs(RegMinstret, "minstret", ins0);
      sba_raw(1'b0, TimerV, 3'd2, 0, serr, tv0);
      chk_eq({what, ": rv_timer read over SBA"}, serr, 0);
      #(hold_us[h] * 1us);
      csr_rd_abs(RegMcycle, "mcycle", cyc1);
      t1 = $realtime;
      csr_rd_abs(RegMinstret, "minstret", ins1);
      sba_raw(1'b0, TimerV, 3'd2, 0, serr, tv1);
      chk_eq({what, ": rv_timer read over SBA"}, serr, 0);
      chk_true($sformatf("%s: mcycle advanced by %0d over %0t (stopcount 0)", what, cyc1 - cyc0,
                         t1 - t0), (cyc1 - cyc0) >= int'(0.9 * ((t1 - t0) / cpu_period)));
      chk_true($sformatf("%s: minstret did not go back (%0d to %0d)", what, ins0, ins1),
               ins1 - ins0 < 32'h8000_0000);
      chk_true($sformatf("%s: rv_timer advanced across the hold (%0d to %0d, stoptime 0)", what,
                         tv0, tv1), tv1 != tv0 && tv1 - tv0 < 32'h8000_0000);
      w0 = watch_times.size();
      resume_checked(what);
      t_res = $realtime;
      while (watch_times.size() == w0 && $realtime < t_res + 2 * wfi_ticks * 1us) #(100ns);
      chk_true({what, ": the core left wfi after the resume (the timer interrupt that came ",
                "during the hold was kept)"}, watch_times.size() > w0);
    end
    watch_pc = 0;
    end_cmd(1'b1, ok);
    finish(32'h17);
  endtask

endclass : chip_eg2_rv_dm_counters_vseq
