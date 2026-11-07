// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_perform_debug and chip_rv_dm_perform_debug_cheriot (EG2 spec REQ_CDM_08,
// REQ_PBF_20; "Debug Controller"; knobs under "Debug in CHERIoT mode";
// sw/device/tests/eg2_debug/eg2_rv_dm_perform_debug.c), both modes.
//
// The procedure of OpenTitan's chip_rv_dm_perform_debug testpoint (rom_e2e_jtag_debug_*,
// chip_sw_rom_e2e_jtag_debug_vseq.sv: halt, every hardware breakpoint, single step including over
// wfi, registers and memory read and written through the core, resume) run with the stock
// debugger model (jtag_rv_debugger) against this image, so that in CHERIoT mode it runs after
// the lock. The life-cycle state is drawn among the debug-enabled ones (chip_eg2_rv_dm_base_vseq:
// TEST_UNLOCKED0-7, DEV with late debug enabled by OTP or software, RMA). The phases run in a
// drawn order (the spec's w_dbg_<o>, uniform):
//  - ACCESS (the software's PD_TAGS action, PARK underneath): halt; every register of the
//    procedure written with a drawn value, read back, restored and read back (RISC-V mode: x1-x31
//    but s0 and a0, dscratch0/1, mscratch, mcycleh, minstreth, as the ROM test; CHERIoT mode: a0
//    and t1, the PARK kernel's free registers, and mcycleh, minstreth: a register written through
//    data0 is untagged, rv_dm theory_of_operation "CHERIoT Mode", so the others are only read);
//    scratch words 16-19 written through the core (program buffer `sw`; in CHERIoT mode with a0
//    and t1, as the stock route saves s0/s1 with 32-bit accesses, which clears their tags, same
//    document) and words 24-27 over SBA with the same values xor 0xa5a5a5a5, each read back over
//    the other path; a device register (rv_timer COMPARE_UPPER0_0) written, read back through the
//    core and over SBA, restored; resume. The software then checks the words and its capability.
//  - BREAKPOINTS (PARK): all 4 hardware breakpoints (DbgHwBreakNum; Ibex cs_registers.rst: "break
//    before the instruction") on the first 4 instructions of PARK's loop; the core run into each of
//    them in loop order and once more around the loop. Each hit: dcsr.cause = 2 (trigger), dpc =
//    the breakpoint's address (RISC-V Debug Specification 0.13.2 4.8.1, 4.8.2), the GPRs against
//    RVFI. Then every breakpoint deleted (none listed) and resume.
//  - STEP (WFI): single steps to the kernel's wfi and one step over it: one instruction retired per
//    step, dcsr.cause = 4 (step), dpc = the next instruction (debug specification 4.8.1 step: the
//    hart executes one instruction and enters Debug Mode), so after the wfi dpc = its address + 4.
//  - CAP (CAP): halt and cap_steps single steps among CLC/CSC; the software's CAP checks keep the
//    capabilities' tags.
// Every halt and resume is checked by chip_eg2_rv_dm_corners_base_vseq (dcsr, dpc, the GPRs, and
// after resume every register's value and capability metadata, tag included, as at the halt:
// REQ_CDM_08 "debug entry and exit keep every capability register and its tag").
// Not done (the ROM test's): setting the PC and `call` (REQ_CDM_08 does not list them), and ROM
// code: this image runs from RRAM. +eg2_dm_bench_fail=1 corrupts the first halt's dcsr.cause.
class chip_eg2_rv_dm_perform_debug_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_perform_debug_vseq)
  `uvm_object_new

  localparam int unsigned NumBreakpoints = 4;
  localparam bit [7:0] KindCap = 8'd4;
  localparam bit [7:0] KindWfi = 8'd5;
  localparam bit [7:0] KindPark = 8'd9;
  localparam bit [7:0] ActTags = 8'h10;
  localparam bit [31:0] InsnSwT1A0 = 32'h0065_2023;  // sw t1, 0(a0)
  localparam bit [31:0] InsnLwT1A0 = 32'h0005_2303;  // lw t1, 0(a0)
  localparam bit [31:0] SbaXor = 32'ha5a5_a5a5;
  localparam int unsigned CoreWord = 16, SbaWord = 24, NumWords = 4;
  localparam bit [31:0] TimerCmpHi = top_earlgrey_pkg::TOP_EARLGREY_RV_TIMER_BASE_ADDR + 32'h11c;
  localparam bit [15:0] CsrDscratch0 = 16'h07b2, CsrDscratch1 = 16'h07b3, CsrMscratch = 16'h0340;
  localparam bit [15:0] CsrMcycleh = 16'h0b80, CsrMinstreth = 16'h0b82;

  typedef enum int unsigned {PhaseAccess, PhaseBreakpoints, PhaseStep, PhaseCap} phase_e;

  rand phase_e phase_order[4];
  rand bit [31:0] mem_vals[NumWords];
  rand bit [31:0] dev_val;
  rand int unsigned wfi_ticks;
  rand int unsigned cap_steps;

  constraint knobs_c {
    unique {phase_order};
    foreach (mem_vals[i]) mem_vals[i] != 0;
    wfi_ticks inside {[10:40]};
    cap_steps inside {[2:8]};
  }

  bit cheriot;

  virtual function void write_params();
    super.write_params();
    set_param("wfi_ticks", wfi_ticks);
  endfunction

  protected task wait_entries(int unsigned n, string what);
    fork : isolation_fork
      begin
        fork
          wait (debug_entries >= n);
          #(500us);
        join_any
        disable fork;
      end
    join
    chk_true({what, ": RVFI shows the debug entries"}, debug_entries >= n);
  endtask

  // One single step (dcsr.step already 1): one instruction retired, cause step.
  protected task step_checked(string what);
    int unsigned n0 = debug_entries, nd0 = nd_retired;
    cfg.debugger.set_resumereq(1);
    wait_debug_entry(n0, what);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_resumereq(0);
    chk_eq({what, ": instructions retired by one step"}, nd_retired - nd0, 1);
    gpr_sample = 4;
    check_entry(what, RvDebugCauseStep, 1'b1);
  endtask

  protected task set_step(bit v);
    rv_core_csr_dcsr_t dcsr;
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = v;
    cfg.debugger.write_dcsr(dcsr);
  endtask

  // ---------------------------------------------------------------- memory through the core

  protected task core_wr(bit [31:0] addr, bit [31:0] v, string what);
    if (!cheriot) begin
      byte st;
      cfg.debugger.mem_write(.addr(addr), .value_q({v}), .status(st),
                             .route(jtag_rv_debugger_pkg::MemAccessViaProgbuf));
      chk_eq({what, ": write through the core, status"}, st, 0);
    end else begin
      bit [2:0] err;
      progbuf_wr('{InsnSwT1A0, InsnEbreak});
      gpr_wr(RegA0, addr, err);
      chk_eq({what, ": a0 written"}, err, 0);
      data0_wr(v);
      abs_cmd(reg_cmd(RegT1, 1'b1, 1'b1, 1'b1), err);
      chk_eq({what, ": t1 written and the program buffer's sw run"}, err, 0);
    end
  endtask

  protected task core_rd(bit [31:0] addr, output bit [31:0] v, input string what);
    if (!cheriot) begin
      byte st;
      logic [31:0] q[$];
      cfg.debugger.mem_read(.addr(addr), .value_q(q), .status(st),
                            .route(jtag_rv_debugger_pkg::MemAccessViaProgbuf));
      chk_eq({what, ": read through the core, status"}, st, 0);
      v = q.size() ? q[0] : '0;
    end else begin
      bit [2:0] err;
      progbuf_wr('{InsnLwT1A0, InsnEbreak});
      data0_wr(addr);
      abs_cmd(reg_cmd(RegA0, 1'b1, 1'b1, 1'b1), err);
      chk_eq({what, ": a0 written and the program buffer's lw run"}, err, 0);
      gpr_rd(RegT1, v, err);
      chk_eq({what, ": t1 read"}, err, 0);
    end
  endtask

  // ---------------------------------------------------------------- phases

  protected task regs_access();
    bit [15:0] regs[$];
    bit [31:0] orig[bit [15:0]], vals[bit [15:0]];
    if (cheriot) begin
      regs = '{RegA0, RegT1, CsrMcycleh, CsrMinstreth};
    end else begin
      regs = '{CsrDscratch0, CsrDscratch1, CsrMscratch, CsrMcycleh, CsrMinstreth};
      for (int unsigned r = 1; r < 32; r++) begin
        if (r == 8 || r == 10) continue;  // s0 and a0, as the ROM test
        regs.push_back(16'h1000 + r);
      end
    end
    regs.shuffle();
    foreach (regs[i]) begin
      bit [2:0] err;
      bit [31:0] v;
      gpr_rd(regs[i], v, err);
      chk_eq($sformatf("access: read of 0x%0h, cmderr", regs[i]), err, 0);
      orig[regs[i]] = v;
    end
    regs.shuffle();
    foreach (regs[i]) begin
      bit [2:0] err;
      vals[regs[i]] = $urandom;
      gpr_wr(regs[i], vals[regs[i]], err);
      chk_eq($sformatf("access: write of 0x%0h, cmderr", regs[i]), err, 0);
    end
    regs.shuffle();
    foreach (regs[i]) begin
      bit [2:0] err;
      bit [31:0] v;
      gpr_rd(regs[i], v, err);
      chk_eq($sformatf("access: 0x%0h reads the value written", regs[i]), v, vals[regs[i]]);
    end
    foreach (regs[i]) begin
      bit [2:0] err;
      gpr_wr(regs[i], orig[regs[i]], err);
      chk_eq($sformatf("access: restore of 0x%0h, cmderr", regs[i]), err, 0);
    end
    foreach (regs[i]) begin
      bit [2:0] err;
      bit [31:0] v;
      gpr_rd(regs[i], v, err);
      chk_eq($sformatf("access: 0x%0h restored", regs[i]), v, orig[regs[i]]);
    end
  endtask

  protected task mem_access();
    bit [2:0] err;
    bit [31:0] a0, t1, v, dev_orig;
    sba_access_err_e st;
    gpr_rd(RegA0, a0, err);
    gpr_rd(RegT1, t1, err);
    for (int unsigned i = 0; i < NumWords; i++) begin
      core_wr(scratch_addr + 4 * (CoreWord + i), mem_vals[i], $sformatf("memory word %0d", i));
      sba_wr32(scratch_addr + 4 * (SbaWord + i), mem_vals[i] ^ SbaXor, st);
      chk_eq($sformatf("memory word %0d: SBA write status", i), st, SbaErrNone);
    end
    for (int unsigned i = 0; i < NumWords; i++) begin
      core_rd(scratch_addr + 4 * (SbaWord + i), v, $sformatf("memory word %0d", i));
      chk_eq($sformatf("memory word %0d written over SBA, read through the core", i), v,
             mem_vals[i] ^ SbaXor);
      sba_rd32(scratch_addr + 4 * (CoreWord + i), v, st);
      chk_eq($sformatf("memory word %0d written through the core, read over SBA", i), v,
             mem_vals[i]);
    end
    core_rd(TimerCmpHi, dev_orig, "device register");
    core_wr(TimerCmpHi, dev_val, "device register");
    core_rd(TimerCmpHi, v, "device register");
    chk_eq("device register written through the core, read through the core", v, dev_val);
    sba_rd32(TimerCmpHi, v, st);
    chk_eq("device register written through the core, read over SBA", v, dev_val);
    core_wr(TimerCmpHi, dev_orig, "device register restore");
    if (cheriot) begin
      gpr_wr(RegA0, a0, err);
      chk_eq("memory: a0 restored", err, 0);
      gpr_wr(RegT1, t1, err);
      chk_eq("memory: t1 restored", err, 0);
    end
  endtask

  protected task phase_access(output bit ok);
    int unsigned c0 = caps;
    send_cmd(ActTags, 0, ok);
    if (!ok) return;
    wait_lines("EG2 DEBUG cap", LineCap, c0, ok);
    if (!ok) return;
    #(5us);
    gpr_sample = 0;
    halt_checked("access: halt");
    regs_access();
    mem_access();
    resume_checked("access: resume");
    end_cmd(1'b1, ok);
  endtask

  protected function bit [31:0] loop_next(bit [31:0] park, bit [31:0] pc);
    // PARK's loop (eg2_dm_kernels.S, norvc): addi a3; KTAIL: addi a2; lw t0; bnez t0; bltu back.
    return pc == park + 16 ? park : pc + 4;
  endfunction

  protected task phase_breakpoints(output bit ok);
    breakpoint_t bps[$];
    bit [31:0] park;
    send_cmd(KindPark, 0, ok);
    if (!ok) return;
    park = start_target;
    #(5us);
    gpr_sample = 0;
    halt_checked("breakpoints: halt");
    cfg.debugger.info_breakpoints(bps);
    chk_eq("breakpoints listed before any is set", bps.size(), 0);
    for (int unsigned i = 0; i < NumBreakpoints; i++) begin
      cfg.debugger.add_breakpoint(.addr(park + 4 * i));
    end
    cfg.debugger.info_breakpoints(bps);
    chk_eq("breakpoints listed: every hardware breakpoint (DbgHwBreakNum)", bps.size(),
           NumBreakpoints);
    for (int unsigned h = 0; h <= NumBreakpoints; h++) begin
      logic [31:0] pc;
      bit [31:0] q;
      bit on_bp;
      int unsigned n0;
      string what = $sformatf("breakpoint hit %0d", h);
      cfg.debugger.read_pc(pc);
      on_bp = pc >= park && pc < park + 4 * NumBreakpoints && pc[1:0] == 0;
      q = on_bp ? loop_next(park, pc) : pc;
      if (q < park || q > park + 16) q = park;
      while (q >= park + 4 * NumBreakpoints) q = loop_next(park, q);
      n0 = debug_entries;
      // The debugger's continue: over a breakpoint at dpc it steps first (one more entry).
      cfg.debugger.continue_execution();
      wait_entries(n0 + (on_bp ? 2 : 1), what);
      cfg.debugger.wait_cpu_halted();
      cfg.debugger.set_resumereq(0);
      gpr_sample = 6;
      check_entry(what, RvDebugCauseTrigger, 1'b1);
      chk_eq({what, ": dpc is the next breakpoint's address"}, last_dpc, q, 1'b1);
    end
    cfg.debugger.info_breakpoints(.breakpoints(bps), .delete_breakpoints(1));
    cfg.debugger.info_breakpoints(bps);
    chk_eq("breakpoints listed after every one is deleted", bps.size(), 0);
    gpr_sample = 0;
    resume_checked("breakpoints: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_step(output bit ok);
    bit [31:0] wfi_pc;
    send_cmd(KindWfi, 0, ok);
    if (!ok) return;
    wfi_pc = start_target;
    #(5us);
    gpr_sample = 0;
    halt_checked("step: halt in the wfi kernel");
    set_step(1'b1);
    for (int unsigned i = 0; i < 40 && last_dpc != wfi_pc; i++) begin
      step_checked($sformatf("step %0d towards the wfi", i));
    end
    chk_eq("step: dpc reached the wfi", last_dpc, wfi_pc);
    step_checked("step over the wfi");
    chk_eq("step over the wfi: dpc is the next instruction", last_dpc, wfi_pc + 4, 1'b1);
    set_step(1'b0);
    gpr_sample = 0;
    resume_checked("step: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_cap(output bit ok);
    send_cmd(KindCap, 0, ok);
    if (!ok) return;
    #(5us);
    gpr_sample = 0;
    halt_checked("cap: halt");
    set_step(1'b1);
    for (int unsigned i = 0; i < cap_steps; i++) step_checked($sformatf("cap step %0d", i));
    set_step(1'b0);
    gpr_sample = 0;
    resume_checked("cap: resume");
    end_cmd(1'b1, ok);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    cheriot = rvfi.cheriot_ena === 1'b1;
    `uvm_info(`gfn, $sformatf("EG2 DM perform_debug: %s mode, life cycle %s, phases %p",
                              cheriot ? "CHERIoT" : "RISC-V", lc_state.name(), phase_order),
              UVM_LOW)
    foreach (phase_order[i]) begin
      case (phase_order[i])
        PhaseAccess: phase_access(ok);
        PhaseBreakpoints: phase_breakpoints(ok);
        PhaseStep: phase_step(ok);
        default: phase_cap(ok);
      endcase
      if (!ok || sw_ended()) return;
    end
    finish(32'h40);
  endtask

endclass : chip_eg2_rv_dm_perform_debug_vseq
