// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_priv (EG2 spec REQ_CDM_08; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_priv.c), both modes. The chip form of the core
// specification's REQ_DBG_04 ("dret restores the hart to the privilege mode stored in dcsr.prv").
//
// Expected values: the RISC-V Debug Specification 0.13.2 (4.8.1 dcsr.prv: "Not all privilege
// levels are supported on all harts. If the encoding written is not supported or the debugger is
// not allowed to change to it, the hart may change to any supported privilege level"; 3.12.6
// cmderr 3 "An exception occurred while executing the command"; 4.1 "Exceptions don't update any
// registers. That includes cause, epc, tval, dpc, and mstatus"); the core specification's privilege
// modes (CHERIoT mode: M only, misa.U = 0; RISC-V mode: M and U; S in neither).
//
// This side:
//   phase 4 (first): halted in the stage; misa read (S = 0, U = 1 in RISC-V mode only); mcause,
//     mepc and dpc read; an `ecall` and an all-zero (illegal) word in the program buffer: cmderr 3
//     each, the hart still halted, mcause, mepc and dpc unchanged; resume with
//     every register as before (the base's resume check).
//   phases 0-3: halted; dcsr.prv = the privilege of the halted code (M); dcsr.prv := the phase;
//     read back: a level the mode implements, and the written one when the mode implements it;
//     published to the software (scratch word 1); resume: the first instruction after resume
//     retires at that level (RVFI mode). The software checks its ecall's cause.
// `mret` in the program buffer is not run: no document defines it in debug mode, and it would
// jump out of the program buffer while in debug mode.
// +eg2_dm_bench_fail=1 corrupts the first primary check: the base's dcsr.cause at the first halt.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
// input, so every halt in CHERIoT mode runs the base debug ROM.
class chip_eg2_rv_dm_priv_vseq extends chip_eg2_rv_dm_cap_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_priv_vseq)
  `uvm_object_new

  localparam int unsigned ParkSpan = 12;
  localparam bit [31:0] PrvPublished = 32'h5052_5600;  // eg2_rv_dm_priv.c kPrvPublished << 4
  localparam bit [7:0] PhProbes = 8'd4;

  protected task phase_probes(bit cheriot);
    bit ok;
    bit [31:0] misa, mcause0, mepc0, v;
    logic [31:0] dpc;
    uvm_reg_data_t dmstatus;
    phase_begin(PhProbes, ok);
    if (!ok) return;
    wait_parked("probes", ok);
    if (!ok) return;
    halt_parked("probes: halt", ParkSpan);
    rd_x("misa", RegMisa, misa);
    chk_eq("misa.S (no S-mode in either mode)", misa[18], 0);
    chk_eq("misa.U (U-mode in RISC-V mode only)", misa[20], !cheriot);
    rd_x("mcause", RegMcause, mcause0);
    rd_x("mepc", RegMepc, mepc0);
    expect_progbuf("ecall in the program buffer", '{InsnEcall, InsnEbreak}, CmdErrException);
    expect_progbuf("illegal instruction in the program buffer", '{InsnIllegal, InsnEbreak},
                   CmdErrException);
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(dmstatus), .blocking(1));
    chk_eq("hart still halted after the exceptions",
           get_field_val(cfg.jtag_dmi_ral.dmstatus.allhalted, dmstatus), 1);
    rd_x("mcause after", RegMcause, v);
    chk_eq("mcause unchanged by exceptions in debug mode", v, mcause0);
    rd_x("mepc after", RegMepc, v);
    chk_eq("mepc unchanged by exceptions in debug mode", v, mepc0);
    cfg.debugger.read_pc(dpc);
    chk_eq("dpc unchanged by exceptions in debug mode", dpc, last_dpc);
    resume_checked("probes: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_prv(bit [1:0] v, bit cheriot);
    bit ok, legal;
    rv_core_csr_dcsr_t dcsr;
    bit [1:0] r;
    int unsigned r0;
    string what = $sformatf("dcsr.prv := %0d", v);
    phase_begin({6'h0, v}, ok);
    if (!ok) return;
    wait_parked(what, ok);
    if (!ok) return;
    halt_parked({what, ": halt"}, ParkSpan);
    cfg.debugger.read_dcsr(dcsr);
    chk_eq({what, ": dcsr.prv at the halt (the stage runs at M)"}, dcsr.prv, 3);
    dcsr.prv = rv_privilege_level_e'(v);
    cfg.debugger.write_dcsr(dcsr);
    cfg.debugger.read_dcsr(dcsr);
    r = dcsr.prv;
    legal = r == 2'd3 || (r == 2'd0 && !cheriot);
    chk_true($sformatf("%s: read back %0d, a level the %s mode implements", what, r,
                       cheriot ? "CHERIoT" : "RISC-V"), legal);
    if (v == 2'd3 || (v == 2'd0 && !cheriot)) begin
      chk_eq({what, ": an implemented level reads back as written"}, r, v);
    end
    scratch_wr(ScratchArg0, PrvPublished | r);
    // Resume: the first instruction retires at the level read back.
    r0 = resumes;
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
    chk_eq({what, ": the privilege after dret (RVFI mode)"}, post_mode, r);
    end_cmd(1'b1, ok);
  endtask

  virtual task body();
    bit ok, cheriot;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    cheriot = cheriot_mode();
    `uvm_info(`gfn, $sformatf("EG2 DM priv: %s mode", cheriot ? "CHERIoT" : "RISC-V"), UVM_LOW)
    phase_probes(cheriot);
    for (int v = 0; v < 4 && !sw_ended(); v++) phase_prv(v, cheriot);
    if (!sw_ended()) finish(32'h32);
  endtask

endclass : chip_eg2_rv_dm_priv_vseq
