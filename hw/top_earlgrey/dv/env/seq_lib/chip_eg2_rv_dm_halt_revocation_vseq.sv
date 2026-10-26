// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_halt_revocation (EG2 spec REQ_CDM_08, REQ_CMS_12; "Debug Controller" / "Corner
// cases"; sw/device/tests/eg2_debug/eg2_rv_dm_halt_revocation.c, whose header lists the phases and
// the software's checks), CHERIoT mode only.
//
// Expected values: the RISC-V Debug Specification 0.13.2 (4.1: interrupts, NMI included, are not
// taken in debug mode; 4.8.1 dcsr.step: one instruction per step, dcsr.cause 4, dpc the next
// instruction), cheriot_mem_sys theory_of_operation.md (the engine sweeps on its own, `tbre_done`
// when it stops; the bitmap window takes word writes from any host) and eg2_revoke.h's load-filter
// expression (a capability based in a marked granule loads untagged).
//
// This side, per phase:
//   0  The sweep: TBRE_STATUS.busy polled over SBA until set, then the halt. Checked: busy still
//      set after the halt (the halt landed during the sweep), busy clears while the core stays
//      halted, the PLIC source of tbre_done pending while halted, no instruction retired outside
//      debug mode while halted (RVFI: the interrupt was not taken), and after resume the first
//      retirement is the interrupt's (RVFI intr). The software checks the rest.
//   1  `n_clc_halts` halts at drawn moments of the CLC loop, each checked as the base does (dcsr,
//      dpc, GPRs; every register and tag unchanged after resume). Aim: at the debug request, the
//      core's bitmap port has a request out (ibex_top trvk_revbm_req_o); reported as `EG2 DM CLC
//      halt: hit n of m`, an error with +eg2_dm_require_hit=1 if none hit.
//   2  Halted at the stage's wait (dpc = its address); t1 := 1; dcsr.step := 1; four steps, each
//      dcsr.cause = 4 and dpc = the next instruction; after each CLC the program buffer's
//      `cgettag t1, <loaded register>` gives 0, 1, 0 (marked, unmarked, marked).
//   3  Halted: the bitmap word (published by the software) read and written back over SBA with the
//      object's bit set (no SBA error, the bit read back); resume (registers unchanged).
// The primary check (+eg2_dm_bench_fail=1 corrupts the first) is the dcsr.cause of the first halt.
//
// EXPECTED FAILURE until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT input, so every
// halt in CHERIoT mode runs the base debug ROM.
class chip_eg2_rv_dm_halt_revocation_vseq extends chip_eg2_rv_dm_cap_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_halt_revocation_vseq)

  localparam int unsigned ParkSpan = 12;
  localparam int unsigned ClcLoopSpan = 44;  // eg2_dmx_clc_loop: eleven instructions

  rand int unsigned n_clc_halts;
  rand int unsigned clc_delay_ns[8];
  bit require_hit;

  constraint knobs_c {
    n_clc_halts inside {[3:8]};
    foreach (clc_delay_ns[i]) clc_delay_ns[i] inside {[200:20_000]};
  }

  function new(string name = "");
    super.new(name);
    void'($value$plusargs("eg2_dm_require_hit=%0b", require_hit));
  endfunction

  protected task phase_sweep();
    bit ok;
    bit [31:0] status_addr, busy_mask, ip_addr, ip_mask, v;
    sba_access_err_e st;
    int unsigned n, nd0;
    phase_begin(8'd0, ok);
    if (!ok) return;
    wait_parked("phase 0", ok);
    if (!ok) return;
    scratch_rd(ScratchArg0, status_addr);
    scratch_rd(ScratchArg1, busy_mask);
    scratch_rd(ScratchArg2, ip_addr);
    scratch_rd(ScratchArg3, ip_mask);
    do begin
      sba_rd32(status_addr, v, st);
      n++;
    end while ((v & busy_mask) == 0 && n < 200);
    chk_true("phase 0: TBRE_STATUS.busy seen before the halt", (v & busy_mask) != 0);
    // The halt by hand: TBRE_STATUS is read as soon as the core reports halted, before the slower
    // dcsr and dpc checks.
    n = debug_entries;
    cfg.debugger.set_haltreq(1);
    cfg.debugger.wait_cpu_halted();
    sba_rd32(status_addr, v, st);
    cfg.debugger.set_haltreq(0);
    wait_debug_entry(n, "phase 0: halt during the sweep");
    check_entry("phase 0: halt during the sweep", RvDebugCauseHaltReq, 1'b0);
    chk_true("phase 0: dpc in the stage's wait loop",
             last_dpc >= wait_pc && last_dpc < wait_pc + ParkSpan);
    nd0 = nd_retired;
    chk_true("phase 0: the halt landed during the sweep (busy after the halt)",
             (v & busy_mask) != 0);
    n = 0;
    do begin
      sba_rd32(status_addr, v, st);
      n++;
    end while ((v & busy_mask) != 0 && n < 20000);
    chk_eq("phase 0: the sweep finished while the core was halted (busy)", v & busy_mask, 0);
    sba_rd32(ip_addr, v, st);
    chk_eq("phase 0: tbre_done pending at the PLIC while halted", v & ip_mask, ip_mask);
    chk_true("phase 0: still in debug mode (RVFI)", in_debug);
    chk_eq("phase 0: no instruction retired outside debug mode while halted (no interrupt taken)",
           nd_retired - nd0, 0);
    resume_plain("phase 0: resume");
    chk_true("phase 0: tbre_done taken at resume (the first retirement is the interrupt's)",
             post_intr);
    end_cmd(1'b1, ok);
  endtask

  protected task phase_clc_loop();
    bit ok;
    int unsigned hits;
    phase_begin(8'd1, ok);
    if (!ok) return;
    wait_parked("phase 1", ok);
    if (!ok) return;
    for (int unsigned i = 0; i < n_clc_halts && !sw_ended(); i++) begin
      bit hit, sampled;
      #(clc_delay_ns[i] * 1ns);
      // The aim, sampled when the request reaches the core (not with `wait fork`: the base's
      // monitors are this thread's children too).
      fork
        begin
          realtime t;
          wait_debug_req(t);
          hit = probe("trvk_revbm_req_o");
          sampled = 1'b1;
        end
      join_none
      halt_parked($sformatf("phase 1: CLC halt %0d", i), ClcLoopSpan);
      wait (sampled);
      if (hit) hits++;
      resume_checked($sformatf("phase 1: CLC halt %0d: resume", i));
    end
    `uvm_info(`gfn, $sformatf("EG2 DM CLC halt: hit %0d of %0d (a bitmap read out at the request)",
                              hits, n_clc_halts), UVM_LOW)
    if (require_hit && hits == 0) begin
      `uvm_error(`gfn, "EG2 DM CLC halt: no halt landed on a CLC waiting for its bitmap read")
    end
    end_cmd(1'b1, ok);
  endtask

  protected task step_one(string what, bit [31:0] exp_dpc);
    int unsigned n0 = debug_entries;
    rv_core_csr_dcsr_t dcsr;
    logic [31:0] dpc;
    cfg.debugger.set_resumereq(1);
    wait_debug_entry(n0, what);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_resumereq(0);
    cfg.debugger.read_dcsr(dcsr);
    chk_eq({what, ": dcsr.cause (step)"}, dcsr.cause, RvDebugCauseStep);
    cfg.debugger.read_pc(dpc);
    chk_eq({what, ": dpc = the next instruction"}, dpc, exp_dpc);
  endtask

  protected task tag_of(string what, bit [4:0] cs, bit [31:0] exp);
    bit [31:0] v;
    expect_progbuf({what, ": cgettag"}, '{enc_cgettag(XT1, cs), InsnEbreak}, CmdErrNone);
    rd_x({what, ": tag"}, RegGpr + XT1, v);
    chk_eq({what, ": the loaded tag follows the bitmap"}, v, exp);
  endtask

  protected task phase_step();
    bit ok;
    rv_core_csr_dcsr_t dcsr;
    phase_begin(8'd2, ok);
    if (!ok) return;
    wait_parked("phase 2", ok);
    if (!ok) return;
    halt_parked("phase 2: halt", 4);
    wr_x("phase 2: release (t1)", RegGpr + XT1, 1);
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b1;
    cfg.debugger.write_dcsr(dcsr);
    step_one("phase 2: step over the wait", wait_pc + 4);
    step_one("phase 2: step over the CLC of the marked capability", wait_pc + 8);
    tag_of("phase 2: marked", XA2, 0);
    step_one("phase 2: step over the CLC of the unmarked capability", wait_pc + 12);
    tag_of("phase 2: unmarked", XT0, 1);
    step_one("phase 2: step over the CLC of the marked capability again", wait_pc + 16);
    tag_of("phase 2: marked again", XT2, 0);
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b0;
    cfg.debugger.write_dcsr(dcsr);
    resume_plain("phase 2: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_sba_mark();
    bit ok;
    bit [31:0] word_addr, mask, v;
    sba_access_err_e st;
    phase_begin(8'd3, ok);
    if (!ok) return;
    wait_parked("phase 3", ok);
    if (!ok) return;
    scratch_rd(ScratchArg0, word_addr);
    scratch_rd(ScratchArg1, mask);
    halt_parked("phase 3: halt", ParkSpan);
    sba_rd32(word_addr, v, st);
    chk_eq("phase 3: SBA read of the bitmap word", st, SbaErrNone);
    sba_wr32(word_addr, v | mask, st);
    chk_eq("phase 3: SBA write of the bitmap word while halted", st, SbaErrNone);
    sba_rd32(word_addr, v, st);
    chk_eq("phase 3: the bitmap bit set over SBA", v & mask, mask);
    resume_checked("phase 3: resume");
    end_cmd(1'b1, ok);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    chk_true("chip_eg2_rv_dm_halt_revocation runs in CHERIoT mode", cheriot_mode());
    phase_sweep();
    if (!sw_ended()) phase_clc_loop();
    if (!sw_ended()) phase_step();
    if (!sw_ended()) phase_sba_mark();
    if (!sw_ended()) finish(32'h31);
  endtask

endclass : chip_eg2_rv_dm_halt_revocation_vseq
