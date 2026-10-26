// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_step_corners (EG2 spec REQ_CDM_03, REQ_CDM_08; "Debug Controller" / "Corner
// cases"; sw/device/tests/eg2_debug/eg2_rv_dm_step_corners.c), both modes.
//
// Single steps through the kernels of eg2_dm_corners.h, so the steps cross taken branches (every
// kernel's loop), a divide (DIV), CLC/CSC (CAP), wfi (WFI), an ecall's trap entry, its handler
// and its mret (TRAP), and rv_plic's software interrupt pending while stepping (IRQ). Each step:
// resumereq with dcsr.step set. Expected (RISC-V Debug Specification 0.13.2 4.8.1 dcsr.step: the
// hart executes one instruction and enters debug mode, "If the instruction being stepped over
// causes an exception, the hart enters Debug Mode with dpc set to the exception handler";
// dcsr.cause 4; Ibex cs_registers.rst dcsr: stepie is not implemented, "Other bit fields read as
// zero", so interrupts are not taken while stepping, as the specification's stepie = 0 states):
//  - exactly one instruction retired outside debug mode per step (RVFI), in the same privilege;
//  - dcsr.cause = 4, dpc = that instruction's next pc (or the trap vector);
//  - no instruction retired with an interrupt while stepping;
//  - dcsr.stepie written 1 reads 0;
//  - registers checked at each halt against RVFI (six drawn GPRs), and after the final resume
//    every register and tag as at the last step's halt; the pending interrupt is taken after the
//    resume (the software's IRQ count).
// CJALR through each kind of sentry (CHERIoT) is not stepped yet: listed in status/dv_todo.md.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
// input.
class chip_eg2_rv_dm_step_corners_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_step_corners_vseq)
  `uvm_object_new

  localparam int unsigned NumKinds = 6;
  localparam bit [7:0] Kinds[NumKinds] = '{8'd1, 8'd4, 8'd5, 8'd6, 8'd7, 8'd9};

  rand bit [7:0] kind_order[NumKinds];
  rand int unsigned n_steps[NumKinds];
  rand int unsigned wfi_ticks;

  constraint knobs_c {
    foreach (kind_order[i]) kind_order[i] inside {Kinds};
    unique {kind_order};
    foreach (n_steps[i]) n_steps[i] inside {[8:24]};
    wfi_ticks inside {[10:40]};
  }

  virtual function void write_params();
    super.write_params();
    set_param("wfi_ticks", wfi_ticks);
  endfunction

  protected task step_checked(string what);
    int unsigned n0 = debug_entries, nd0 = nd_retired;
    cfg.debugger.set_resumereq(1);
    wait_debug_entry(n0, what);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_resumereq(0);
    chk_eq({what, ": instructions retired by one step"}, nd_retired - nd0, 1);
    chk_true({what, ": no interrupt taken while stepping (stepie 0)"}, !post_intr);
    gpr_sample = 6;
    check_entry(what, RvDebugCauseStep, 1'b1);
  endtask

  protected task step_kind(bit [7:0] kind, int unsigned steps);
    bit ok;
    rv_core_csr_dcsr_t dcsr;
    send_cmd(kind, 0, ok);
    if (!ok) return;
    #(5us);
    gpr_sample = 0;
    halt_checked($sformatf("step kind %0d: halt", kind));
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b1;
    dcsr.stepie = 1'b1;
    cfg.debugger.write_dcsr(dcsr);
    cfg.debugger.read_dcsr(dcsr);
    chk_eq("dcsr.stepie written 1 (not implemented: reads 0)", dcsr.stepie, 0);
    chk_eq("dcsr.step written 1", dcsr.step, 1);
    for (int unsigned i = 0; i < steps; i++) begin
      step_checked($sformatf("step kind %0d step %0d", kind, i));
    end
    cfg.debugger.read_dcsr(dcsr);
    dcsr.step = 1'b0;
    cfg.debugger.write_dcsr(dcsr);
    resume_checked($sformatf("step kind %0d: resume", kind));
    end_cmd(1'b1, ok);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    foreach (kind_order[i]) begin
      step_kind(kind_order[i], n_steps[i]);
      if (sw_ended()) return;
    end
    finish(32'h15);
  endtask

endclass : chip_eg2_rv_dm_step_corners_vseq
