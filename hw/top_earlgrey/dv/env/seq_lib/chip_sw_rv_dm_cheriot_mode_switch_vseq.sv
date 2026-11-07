// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rv_dm_cheriot_mode_switch (EG2 spec REQ_CDM_04; "Debug Controller", "Debug in CHERIoT
// mode"; sw/device/tests/eg2_debug/eg2_rv_dm_cheriot_mode_switch.c). The `_cheriot` image is the
// spec's test (RISC-V, then CHERIoT); the RISC-V image is its control (no lock: base mode in
// every phase).
//
// The debug module follows the mode across every transition: reset (base), the CHERIoT lock
// (CHERIoT), ndmreset (base again while the module stays active), RFC CHERIoT Debug Module p3
// ("mode-switching mechanism"); rv_dm theory_of_operation.md "CHERIoT Mode" (CHERIoT mode only
// with cheriot_ena_i MuBi4True; Earl Grey switches from RV32I to CHERIoT once); Ibex cheriot.rst
// (one-way switch until reset; the core runs as RV32I while cheriot_enable_i is off).
//
// Phases (knob `phase`, uniform; the first is after_lock, where the software publishes its build's
// mode; every phase appears at least once):
//   after_lock    the running software (CHERIoT image: after its start-up lock) parks in
//                 eg2_dmx_park; the bench halts it there. Expected mode: the build's.
//   before_lock   ndmreset with haltreq held across it: the core halts at its first instruction
//                 (dpc = the reset vector, Ibex exception_interrupts.rst), before any lock store.
//                 Expected mode: base. Then resumed; the software boots again and locks again.
//   after_ndm     ndmreset without haltreq; the software boots (reset reason NDM) and parks; the
//                 bench halts it. Expected mode: the build's (the start-up code locked again).
// The mode switch itself always happens with the core running (#1015: a switch while halted is a
// known open finding; this test does not depend on it).
//
// At each halt (n_ops operations drawn by w_dbg_<o>):
//   halt and resume   resume, then halt again in the wait loop (not in before_lock: the core would
//                     boot); the halt's checks are the base's (dcsr.cause 3, dpc, GPRs = RVFI).
//   register read     a GPR x1-x15 by aarsize 2 = its RVFI value; then by aarsize 3: CHERIoT mode
//                     cmderr 0 with data0 = the same value (the capability's address), base mode
//                     cmderr 2 (REQ_CDM_03: aarsize 3 not supported in base mode).
//   register write    t2 by aarsize 2, read back; then `cgettag t1, t2` in the program buffer:
//                     CHERIoT mode cmderr 0 and t1 = 0 (a capability the debugger writes is
//                     untagged, rv_dm "CHERIoT Mode"); base mode cmderr 3 (a CHERIoT instruction is
//                     illegal in RV32I, RISC-V Debug Specification 3.7.1 cmderr 3 exception).
//   memory access     the program buffer's lw (and sw in the parked phases) through t1 = the
//                     address, against SBA: scratch word 5 of the software; in before_lock a load
//                     of the reset vector's word in ROM (main SRAM is not touched across ndmreset).
//   single step       dcsr.step: one instruction retired, dcsr.cause 4, the mode unchanged.
// After every halt: check_mode (the core's and rv_dm's mode = the phase's expected mode).
// +eg2_dm_bench_fail=1 corrupts the first primary check (dcsr.cause at the first halt).
class chip_sw_rv_dm_cheriot_mode_switch_vseq extends chip_eg2_rv_dm_cdm_base_vseq;
  `uvm_object_utils(chip_sw_rv_dm_cheriot_mode_switch_vseq)
  `uvm_object_new

  typedef enum int unsigned {PhAfterLock = 0, PhBeforeLock = 1, PhAfterNdm = 2} phase_e;
  typedef enum int unsigned {
    OpHaltResume = 0, OpRegRead = 1, OpRegWrite = 2, OpMem = 3, OpStep = 4
  } op_e;
  localparam int unsigned MaxPhases = 6;
  localparam int unsigned MaxOps = 8;
  localparam int unsigned ScratchMem = 5;

  rand int unsigned n_phases;
  rand phase_e phase[MaxPhases];
  rand int unsigned n_ops;
  rand int unsigned w_dbg_halt_resume, w_dbg_reg_read, w_dbg_reg_write, w_dbg_mem, w_dbg_step;
  rand int unsigned op_pick[MaxPhases * MaxOps];
  rand bit [3:0] reg_pick[MaxPhases * MaxOps];
  rand bit [31:0] wval[MaxPhases * MaxOps];

  constraint phases_c {
    n_phases inside {[3:MaxPhases]};
    phase[0] == PhAfterLock;
    foreach (phase[i]) phase[i] dist {PhAfterLock := 1, PhBeforeLock := 1, PhAfterNdm := 1};
    PhBeforeLock inside {phase[1], phase[2]} || (n_phases > 3 && PhBeforeLock == phase[3]);
    PhAfterNdm inside {phase[1], phase[2]} || (n_phases > 3 && PhAfterNdm == phase[3]);
  }
  constraint ops_c {
    n_ops inside {[3:MaxOps]};
    w_dbg_halt_resume inside {[1:10]};
    w_dbg_reg_read inside {[1:10]};
    w_dbg_reg_write inside {[1:10]};
    w_dbg_mem inside {[1:10]};
    w_dbg_step inside {[1:10]};
    solve w_dbg_halt_resume, w_dbg_reg_read, w_dbg_reg_write, w_dbg_mem, w_dbg_step before op_pick;
    foreach (op_pick[i]) {
      op_pick[i] < w_dbg_halt_resume + w_dbg_reg_read + w_dbg_reg_write + w_dbg_mem + w_dbg_step;
    }
    foreach (reg_pick[i]) reg_pick[i] inside {[1:15]};
  }

  bit cheriot_build;
  int unsigned op_n;

  protected function op_e op_of(int unsigned pick);
    int unsigned w[5] = '{w_dbg_halt_resume, w_dbg_reg_read, w_dbg_reg_write, w_dbg_mem,
                          w_dbg_step};
    foreach (w[i]) begin
      if (pick < w[i]) return op_e'(i);
      pick -= w[i];
    end
    return OpStep;
  endfunction

  // One drawn operation at a halt; `exp_cheriot` the phase's expected mode, `parked` the core
  // waits in the stage (else at the reset vector, `rom_addr`).
  protected task do_op(string ph, bit exp_cheriot, bit parked, bit [31:0] rom_addr);
    int unsigned k = op_n++ % (MaxPhases * MaxOps);
    op_e op = op_of(op_pick[k]);
    string what = $sformatf("%s op %0d %s", ph, k, op.name());
    bit [2:0] cmderr;
    bit [31:0] v, meta;
    if (op == OpHaltResume && !parked) op = OpRegRead;
    case (op)
      OpHaltResume: begin
        resume_plain({what, ": resume"});
        #(2us);
        gpr_sample = 6;
        halt_parked({what, ": halt again"}, ParkSpan);
        check_mode(what, exp_cheriot);
      end
      OpRegRead: begin
        bit [4:0] r = reg_pick[k];
        rd_x({what, $sformatf(": x%0d by aarsize 2", r)}, RegGpr + r, v);
        if (shadow[r].valid) chk_eq({what, $sformatf(": x%0d = its RVFI value", r)}, v,
                                    shadow[r].data);
        cap_rd(RegGpr + r, v, meta, cmderr);
        if (exp_cheriot) begin
          chk_eq({what, $sformatf(": x%0d by aarsize 3 in CHERIoT mode: cmderr", r)}, cmderr,
                 CmdErrNone, 1'b1);
          if (shadow[r].valid) chk_eq({what, ": aarsize 3 data0 = the register's address"}, v,
                                      shadow[r].data);
        end else begin
          chk_eq({what, $sformatf(": x%0d by aarsize 3 in base mode: cmderr (not supported)", r)},
                 cmderr, CmdErrNotSupported, 1'b1);
        end
        if (cmderr != 0) cmderr_clear();
      end
      OpRegWrite: begin
        wr_x({what, ": t2"}, RegGpr + XT2, wval[k]);
        rd_x({what, ": t2 read back"}, RegGpr + XT2, v);
        chk_eq({what, ": t2 read back = the written value"}, v, wval[k], 1'b1);
        try_progbuf({what, ": cgettag t1, t2"}, '{enc_cgettag(XT1, XT2), InsnEbreak}, cmderr);
        if (exp_cheriot) begin
          chk_eq({what, ": cgettag in CHERIoT mode: cmderr"}, cmderr, CmdErrNone, 1'b1);
          rd_x({what, ": t1"}, RegGpr + XT1, v);
          chk_eq({what, ": the debugger's write left t2 untagged"}, v, 0);
        end else begin
          chk_eq({what, ": cgettag in base mode: cmderr (illegal instruction)"}, cmderr,
                 CmdErrException, 1'b1);
        end
      end
      OpMem: begin
        sba_access_err_e st;
        bit [31:0] addr = parked ? scratch_addr + 4 * ScratchMem : rom_addr;
        bit [31:0] exp;
        if (parked) begin
          exp = wval[k] ^ 32'h5a5a_0000;
          sba_wr32(addr, exp, st);
          chk_eq({what, ": SBA write"}, st, SbaErrNone);
        end else begin
          sba_rd32(addr, exp, st);
          chk_eq({what, ": SBA read of the reset vector's word"}, st, SbaErrNone);
        end
        wr_x({what, ": t1 = the address"}, RegGpr + XT1, addr);
        expect_progbuf({what, ": lw t2, 0(t1)"}, '{enc_lw(XT2, XT1, 12'h0), InsnEbreak},
                       CmdErrNone);
        rd_x({what, ": t2"}, RegGpr + XT2, v);
        chk_eq({what, ": the program buffer's load = the SBA view"}, v, exp, 1'b1);
        if (parked) begin
          wr_x({what, ": t2 = the store data"}, RegGpr + XT2, wval[k]);
          expect_progbuf({what, ": sw t2, 0(t1)"}, '{enc_sw(XT2, XT1, 12'h0), InsnEbreak},
                         CmdErrNone);
          sba_rd32(addr, v, st);
          chk_eq({what, ": SBA read"}, st, SbaErrNone);
          chk_eq({what, ": the program buffer's store seen over SBA"}, v, wval[k]);
        end
      end
      default: begin
        step_once(what);
        check_mode(what, exp_cheriot);
      end
    endcase
  endtask

  // after_lock and after_ndm: the software parked, the core halted there.
  protected task parked_phase(string ph, output bit ok);
    bit [31:0] mode_word;
    bit exp;
    park_and_halt(ph, 8'd0, ok);
    if (!ok) return;
    scratch_rd(ScratchArg0, mode_word);
    if (op_n == 0) cheriot_build = mode_word == 1;
    chk_eq({ph, ": the software's build mode word"}, mode_word, cheriot_build);
    exp = cheriot_build;
    check_mode(ph, exp);
    for (int unsigned i = 0; i < n_ops && !sw_ended(); i++) do_op(ph, exp, 1'b1, 0);
    resume_and_end(ph, ok);
  endtask

  protected task before_lock_phase(string ph, output bit ok);
    int unsigned b0 = boots, r0 = readies;
    bit [31:0] rv = {rvfi.boot_addr[31:8], 8'h80};
    ndm_reset({ph, ": ndmreset with haltreq held"}, 1'b1, ok);
    rv = {rvfi.boot_addr[31:8], 8'h80};
    check_entry({ph, ": halt at the first instruction after ndmreset"}, RvDebugCauseHaltReq,
                1'b0, rv);
    // Base mode after reset, whatever the build (REQ_CDM_04).
    check_mode(ph, 1'b0);
    for (int unsigned i = 0; i < n_ops && !sw_ended(); i++) do_op(ph, 1'b0, 1'b0, rv);
    resume_plain({ph, ": resume"});
    ndm_reboot(ph, b0, r0, ok);
  endtask

  protected task after_ndm_phase(string ph, output bit ok);
    int unsigned b0 = boots, r0 = readies;
    ndm_reset({ph, ": ndmreset"}, 1'b0, ok);
    ndm_reboot(ph, b0, r0, ok);
    if (!ok) return;
    parked_phase(ph, ok);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
    `uvm_info(`gfn, $sformatf("EG2 DM mode switch: phases %p (%0d), %0d operations each",
                              phase, n_phases, n_ops), UVM_LOW)
    for (int unsigned i = 0; i < n_phases && !sw_ended(); i++) begin
      string ph = $sformatf("phase %0d %s", i, phase[i].name());
      case (phase[i])
        PhAfterLock: parked_phase(ph, ok);
        PhBeforeLock: before_lock_phase(ph, ok);
        default: after_ndm_phase(ph, ok);
      endcase
      if (!ok) return;
    end
    if (!sw_ended()) finish(32'h40);
  endtask

endclass : chip_sw_rv_dm_cheriot_mode_switch_vseq
