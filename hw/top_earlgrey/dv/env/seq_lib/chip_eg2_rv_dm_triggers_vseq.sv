// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_triggers (EG2 spec REQ_CDM_08; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_triggers.c), both modes.
//
// The spec row's checks, from debug mode at the first halt: tdata1 = 0x28001048 and tdata2 = 0
// after reset for each of the 4 triggers; the number of triggers read back from tselect, and
// tselect written one beyond the last selecting the last (Ibex cs_registers.rst tselect: "Writing a
// value larger than or equal to the number of supported triggers will write the highest valid
// index"); each trigger keeping its own tdata2. Then each trigger set to the timed instruction of a
// drawn kernel of eg2_dm_corners.h (code in RRAM). Expected (Ibex cs_registers.rst tdata1: "Ibex
// only implements one type of trigger, instruction address match"; type 2, dmode 1, hit "0 = Hit
// indication not supported", timing 0 "Break before the instruction", action 1 "Enter debug mode
// on match", load and store 0 "not supported"):
//  - tdata1 written with execute, load and store set reads back type 2, dmode 1, action 1,
//    execute 1, load 0, store 0, hit 0;
//  - after resume the core enters debug mode with dcsr.cause = 2 (trigger) and dpc = the trigger's
//    address, the instruction there not retired (RVFI); tdata1.hit still reads 0;
//  - the halt and resume checks of chip_eg2_rv_dm_corners_base_vseq.
// Not done yet: triggers on code in ROM and SRAM (status/dv_todo.md). tdata1's reset value is
// the table's fixed fields; the document's "Reset Value: 0x2800_1000" line leaves out m and u.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md).
class chip_eg2_rv_dm_triggers_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_triggers_vseq)
  `uvm_object_new

  localparam bit [15:0] RegTselect = 16'h07a0;
  localparam bit [15:0] RegTdata1 = 16'h07a1;
  localparam bit [15:0] RegTdata2 = 16'h07a2;
  // type 2, dmode, action 1, m, u, execute, store, load.
  localparam bit [31:0] Tdata1Exec = 32'h2800_1000 | 32'h40 | 32'h8 | 32'h4 | 32'h2 | 32'h1;

  // The spec row: the core's 4 triggers (rv_core_ibex DbgHwBreakNum); tdata1's reset value is the
  // fixed fields of cs_registers.rst's table (type 2, dmode, action 1, m, u), execute 0.
  localparam int unsigned NumTriggers = 4;
  localparam bit [31:0] Tdata1Reset = 32'h2800_1048;

  rand bit [7:0] kinds[8];
  rand bit [31:0] tdata2_vals[8];
  constraint kinds_c {
    foreach (kinds[i]) kinds[i] inside {8'd1, 8'd2, 8'd3, 8'd4, 8'd6, 8'd9};
    foreach (tdata2_vals[i]) tdata2_vals[i][1:0] == 0;
  }

  protected task csr_wr_abs(bit [15:0] regno, bit [31:0] v, string what);
    bit [2:0] err;
    gpr_wr(regno, v, err);
    chk_eq({what, " write: cmderr"}, err, 0);
  endtask

  protected task csr_rd_abs(bit [15:0] regno, string what, output bit [31:0] v);
    bit [2:0] err;
    gpr_rd(regno, v, err);
    chk_eq({what, " read: cmderr"}, err, 0);
  endtask

  // The number of triggers: tselect values that read back.
  protected task count_triggers(output int unsigned n);
    bit [31:0] v;
    n = 0;
    for (int unsigned i = 0; i < 8; i++) begin
      csr_wr_abs(RegTselect, i, "tselect");
      csr_rd_abs(RegTselect, "tselect", v);
      if (v != i) break;
      n++;
    end
  endtask

  virtual task body();
    bit ok;
    int unsigned n_trig;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    send_cmd(9, 0, ok);  // PARK while the triggers are counted
    if (!ok) return;
    halt_checked("triggers: halt to count them");
    // tdata1 and tdata2 as reset, before any write.
    for (int unsigned i = 0; i < NumTriggers; i++) begin
      bit [31:0] v;
      csr_wr_abs(RegTselect, i, "tselect");
      csr_rd_abs(RegTdata1, "tdata1", v);
      chk_eq($sformatf("trigger %0d: tdata1 after reset", i), v, Tdata1Reset);
      csr_rd_abs(RegTdata2, "tdata2", v);
      chk_eq($sformatf("trigger %0d: tdata2 after reset", i), v, 0);
    end
    count_triggers(n_trig);
    `uvm_info(`gfn, $sformatf("EG2 DM triggers: %0d", n_trig), UVM_LOW)
    chk_eq("number of triggers (DbgHwBreakNum)", n_trig, NumTriggers);
    begin
      bit [31:0] v;
      csr_wr_abs(RegTselect, n_trig, "tselect");
      csr_rd_abs(RegTselect, "tselect", v);
      chk_eq("tselect written one beyond the last selects the last", v, n_trig - 1);
      // Each trigger keeps its own tdata2.
      for (int unsigned i = 0; i < n_trig; i++) begin
        csr_wr_abs(RegTselect, i, "tselect");
        csr_wr_abs(RegTdata2, tdata2_vals[i], "tdata2");
      end
      for (int unsigned i = 0; i < n_trig; i++) begin
        csr_wr_abs(RegTselect, i, "tselect");
        csr_rd_abs(RegTdata2, "tdata2", v);
        chk_eq($sformatf("trigger %0d keeps its own tdata2", i), v, tdata2_vals[i]);
        csr_wr_abs(RegTdata2, 0, "tdata2");
      end
    end
    resume_checked("triggers: resume after counting");
    end_cmd(1'b1, ok);
    for (int unsigned t = 0; t < n_trig && t < 8; t++) begin
      bit [31:0] target, v;
      int unsigned n0;
      string what = $sformatf("trigger %0d on kernel %0d", t, kinds[t]);
      send_cmd(kinds[t], 0, ok);
      if (!ok) return;
      target = start_target;
      halt_checked({what, ": halt to set it"});
      csr_wr_abs(RegTselect, t, "tselect");
      csr_wr_abs(RegTdata2, target, "tdata2");
      csr_wr_abs(RegTdata1, Tdata1Exec, "tdata1");
      csr_rd_abs(RegTdata1, "tdata1", v);
      chk_eq({what, ": tdata1.type"}, v[31:28], 2);
      chk_eq({what, ": tdata1.dmode"}, v[27], 1);
      chk_eq({what, ": tdata1.hit (not supported)"}, v[20], 0);
      chk_eq({what, ": tdata1.action (enter debug mode)"}, v[15:12], 1);
      chk_eq({what, ": tdata1.execute"}, v[2], 1);
      chk_eq({what, ": tdata1.store (not supported)"}, v[1], 0);
      chk_eq({what, ": tdata1.load (not supported)"}, v[0], 0);
      csr_rd_abs(RegTdata2, "tdata2", v);
      chk_eq({what, ": tdata2"}, v, target);
      // Resume: the core runs to the trigger (at once if dpc is the trigger's address).
      n0 = debug_entries;
      cfg.debugger.set_resumereq(1);
      wait_debug_entry(n0, what);
      cfg.debugger.wait_cpu_halted();
      cfg.debugger.set_resumereq(0);
      chk_eq({what, ": the trigger's instruction had not retired (timing 0)"}, entry_pc_wdata,
             target);
      gpr_sample = 6;
      check_entry({what, ": entry"}, RvDebugCauseTrigger, 1'b1);
      chk_eq({what, ": dpc is the trigger's address"}, last_dpc, target, 1'b1);
      csr_wr_abs(RegTselect, t, "tselect");
      csr_rd_abs(RegTdata1, "tdata1", v);
      chk_eq({what, ": tdata1.hit after the match (not supported)"}, v[20], 0);
      csr_wr_abs(RegTdata1, 32'h0, "tdata1 (off)");
      csr_rd_abs(RegTdata1, "tdata1", v);
      chk_eq({what, ": tdata1.execute after it was cleared"}, v[2], 0);
      resume_checked({what, ": resume after the match"});
      end_cmd(1'b1, ok);
      if (sw_ended()) return;
    end
    finish(32'h18);
  endtask

endclass : chip_eg2_rv_dm_triggers_vseq
