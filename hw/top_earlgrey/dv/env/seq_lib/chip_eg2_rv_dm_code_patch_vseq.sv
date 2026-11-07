// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_code_patch (EG2 spec REQ_CDM_08; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_code_patch.c), both modes.
//
// The debugger writes code over SBA into a slot in main SRAM that the instruction cache holds
// (the software called it with the cache on). Per round, a drawn variant (each at least once):
//  - CHANGE: `addi a0, a0, k_new` over the slot's `addi a0, a0, k_old`. The software's calls
//    before its `fence.i` may run either (the documents oblige nothing there: Ibex icache.rst,
//    "the core must never fetch from an address with the cache enabled after modifying the data
//    at that address, without first starting a cache invalidation"); after it, the new code
//    (icache.rst: FENCE.I raises icache_inval_i and branches). The software checks the results.
//  - EBREAK: the core halted, dcsr.ebreakm written 1 and read back (RISC-V Debug Specification
//    0.13.2 4.8.1; Ibex cs_registers.rst dcsr: ebreakm RW), resumed; `ebreak` over the slot. The
//    call that reaches it enters debug mode, checked: dcsr.cause = 1 (ebreak), dcsr.ebreakm 1,
//    dcsr.prv = M, dpc = the slot (4.8.2: "the address of the ebreak instruction"), the GPRs
//    through abstract commands as at entry; exactly one such entry. The bench then writes
//    `addi a0, a0, k_new` over SBA and runs `fence.i ; ebreak` from the program buffer (the
//    debug specification: a debugger that writes code makes the hart's instruction fetch see it,
//    fence.i), cmderr 0, clears ebreakm and resumes: the first instruction after resume is at dpc,
//    registers unchanged (chip_eg2_rv_dm_corners_base_vseq resume_checked), and the software's
//    calls return x + k_new.
// Both: the slot's first word read over SBA before the patch is `addi a0, a0, k_old` (the knob
// the bench wrote); the software's end line carries its checks' verdict.
//
// The instruction check: the instruction-set model has no instruction cache. A call that runs the
// old code after the SBA write mismatches it; with stale_calls > 0 the window is announced to the
// injection registry ("insn", may diverge, need not be seen). A halt stops the model anyway until
// the next reset (eg2_iss_wrapper debug_session).
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
// input, so a halt in CHERIoT mode runs the base debug ROM (the EBREAK variant).
class chip_eg2_rv_dm_code_patch_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_code_patch_vseq)
  `uvm_object_new

  localparam bit [7:0] ActionPatch = 8'h10;
  localparam int unsigned TestId = 32'h40;
  localparam int unsigned MaxRounds = 4;
  localparam bit [31:0] ScratchSlot = 0, ScratchOld = 4, ScratchFlag = 8, ScratchKNew = 12;

  typedef enum bit [7:0] {VariantChange = 0, VariantEbreak = 1} variant_e;

  rand int unsigned n_rounds;
  rand variant_e variants[MaxRounds];
  rand bit [9:0] k_old;
  rand bit [9:0] k_new[MaxRounds];
  rand int unsigned warm_calls, stale_calls, check_calls;

  constraint rounds_c {
    n_rounds inside {[2:MaxRounds]};
    variants[0] != variants[1];
  }
  constraint knobs_c {
    k_old != 0;
    foreach (k_new[i]) k_new[i] != k_old && k_new[i] != 0;
    warm_calls inside {[1:8]};
    stale_calls dist {0 := 2, [1:4] := 8};
    check_calls inside {[1:4]};
  }

  virtual function void write_params();
    super.write_params();
    set_param("k_old", k_old);
    set_param("warm_calls", warm_calls);
    set_param("stale_calls", stale_calls);
    set_param("check_calls", check_calls);
  endfunction

  protected function bit [31:0] insn_addi_a0(bit [11:0] k);
    return {k, 20'h50513};
  endfunction

  protected task sba_rd_chk(bit [31:0] addr, output bit [31:0] v);
    sba_access_err_e st;
    sba_rd32(addr, v, st);
    chk_eq($sformatf("SBA read of 0x%08x", addr), st, SbaErrNone);
  endtask

  protected task sba_wr_chk(bit [31:0] addr, bit [31:0] v);
    sba_access_err_e st;
    sba_wr32(addr, v, st);
    chk_eq($sformatf("SBA write of 0x%08x", addr), st, SbaErrNone);
  endtask

  protected task set_ebreakm(bit v, string what);
    rv_core_csr_dcsr_t dcsr;
    cfg.debugger.read_dcsr(dcsr);
    dcsr.ebreakm = v;
    cfg.debugger.write_dcsr(dcsr);
    cfg.debugger.read_dcsr(dcsr);
    chk_eq({what, ": dcsr.ebreakm written and read back"}, dcsr.ebreakm, v);
  endtask

  // The ebreak's debug entry, checked; the slot repaired and fence.i run from the program buffer.
  protected task ebreak_entry(string what, bit [31:0] slot, bit [31:0] new_word, int unsigned n0);
    rv_core_csr_dcsr_t dcsr;
    logic [31:0] dpc;
    abstract_cmd_err_e st;
    logic [31:0] pb[$];
    realtime t_end = $realtime + 5ms;
    while (debug_entries == n0 && !sw_ended() && $realtime < t_end) #(1us);
    chk_eq({what, ": debug entries by the ebreak"}, debug_entries - n0, 1);
    if (debug_entries == n0) return;
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.read_dcsr(dcsr);
    chk_eq({what, ": dcsr.cause (ebreak)"}, dcsr.cause, RvDebugCauseEbreak, 1'b1);
    chk_eq({what, ": dcsr.ebreakm"}, dcsr.ebreakm, 1);
    chk_eq({what, ": dcsr.xdebugver"}, dcsr.xdebugver, 4);
    chk_eq({what, ": dcsr.prv (M)"}, dcsr.prv, 3);
    cfg.debugger.read_pc(dpc);
    chk_eq({what, ": dpc is the ebreak's address (the slot)"}, dpc, slot);
    last_dpc = dpc;
    gpr_sample = 0;
    check_gprs_at_entry(what);
    // The repair: the new instruction over SBA, then fence.i from the program buffer.
    sba_wr_chk(slot, new_word);
    pb.push_back(FenceI);
    pb.push_back(Ebreak);
    cfg.debugger.abstract_cmd_exec_progbuf(pb, st);
    chk_eq({what, ": program buffer fence.i ; ebreak (cmderr)"}, st, AbstractCmdErrNone);
    set_ebreakm(1'b0, what);
    resume_checked({what, ": resume at the repaired slot"});
  endtask

  protected task patch_round(int unsigned r);
    bit ok;
    bit [31:0] slot, w, new_word;
    int unsigned a0 = armed, n0;
    variant_e v = variants[r];
    string what = $sformatf("patch round %0d (%s)", r, v.name());
    send_cmd(ActionPatch, v, ok);
    if (!ok) return;
    wait_lines("EG2 DEBUG armed", LineArmed, a0, ok);
    if (!ok) return;
    sba_rd_chk(scratch_addr + ScratchSlot, slot);
    sba_rd_chk(slot, w);
    chk_eq({what, ": the slot's first word over SBA (addi a0, a0, k_old)"}, w,
           insn_addi_a0(k_old));
    // The bench's words the software polls: the model takes the chip's value (mailboxes).
    eg2_iss_wrapper::backdoor_mailbox(scratch_addr + ScratchFlag, 4);
    eg2_iss_wrapper::backdoor_mailbox(scratch_addr + ScratchKNew, 4);
    new_word = insn_addi_a0(k_new[r]);
    `uvm_info(`gfn, $sformatf("EG2 DM %s: slot 0x%08x, k_old 0x%0h, k_new 0x%0h", what, slot,
                              k_old, k_new[r]), UVM_LOW)
    if (v == VariantEbreak) begin
      halt_checked({what, ": halt to set dcsr.ebreakm"});
      set_ebreakm(1'b1, what);
      resume_checked({what, ": resume with ebreakm"});
      sba_wr_chk(scratch_addr + ScratchKNew, k_new[r]);
      n0 = debug_entries;
      sba_wr_chk(slot, Ebreak);
      sba_wr_chk(scratch_addr + ScratchFlag, 1);
      ebreak_entry(what, slot, new_word, n0);
      n0 = debug_entries;
      end_cmd(1'b0, ok);
      chk_eq({what, ": no further debug entry after the repair"}, debug_entries, n0);
    end else begin
      sba_wr_chk(scratch_addr + ScratchKNew, k_new[r]);
      if (stale_calls > 0) begin
        // TODO(inject-registry): a window, not a fault; announced as one until the registry has a
        // kind for behaviour the documents leave open.
        void'(eg2_inject_registry::announce({get_type_name(), " SBA code patch"}, "insn",
            "code changed under the instruction cache: the core may run the old code until fence.i",
            $realtime, eg2_inject_registry::UntilConsumed, 1'b0, stale_calls, 1'b1));
      end
      sba_wr_chk(slot, new_word);
      sba_wr_chk(scratch_addr + ScratchFlag, 1);
      end_cmd(1'b0, ok);
    end
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    `uvm_info(`gfn, $sformatf("EG2 DM code patch: %s mode, %0d rounds %p, stale calls %0d",
                              rvfi.cheriot_ena === 1'b1 ? "CHERIoT" : "RISC-V", n_rounds,
                              variants, stale_calls), UVM_LOW)
    for (int unsigned r = 0; r < n_rounds; r++) begin
      patch_round(r);
      if (sw_ended()) return;
    end
    finish(TestId);
  endtask

endclass : chip_eg2_rv_dm_code_patch_vseq
