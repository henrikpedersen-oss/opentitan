// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_cap_bypass (EG2 spec REQ_CDM_07, REQ_CDM_08, REQ_CMS_11; "Debug Controller" /
// "Corner cases"; sw/device/tests/eg2_debug/eg2_rv_dm_cap_bypass.c, whose header lists the phases
// and the software's checks), both modes. The chip form of the core specification's REQ_DBG_03
// ("In debug mode the core applies no CHERIoT capability check": no bounds, tag, seal or
// permission fault on a load, store, CHERI instruction or fetch; a CLC applies no tag strip or
// LG/LM attenuation; SCRs 24-26 accessible; outside debug mode they are illegal; after dret the
// checks apply from the first fetch) and REQ_DBG_05 (the debug memory is exempt from PMP, RISC-V
// Debug Specification A.2).
//
// This side, per phase (CHERIoT mode 0-5, RISC-V mode 6):
//   0  nothing: the software's own phase (SCRs 24-26 outside debug mode).
//   1  halted in the stage: for each authority (out of bounds, untagged, GL only) the program
//      buffer `clc a1, auth(a0); lw a2, 0x10(a1); sw t2, 0x14(a1); clc t0, 0x18(a1);
//      csc a0, 0x20(a1); cgettag t1, t0; csc t0, out(a0); ebreak` with t2 written first:
//      cmderr 0 (no exception in debug mode), a2 = the word the software put there, t1 = 1 (the
//      CLC kept the tag). Then `cspecialr t0, 24; cgettag t1, t0; cgetaddr t2, t0;
//      cspecialw 26, a0; cspecialr a2, 26; csc a2, out3(a0)` and the same for SCR 25: cmderr 0,
//      DEPCC tagged with address dpc.
//   2  halted: `clc a1, auth_mark(a0); clc t0, 0(a1); cgettag t1, t0; csc t0, out5(a0)`: cmderr
//      0, t1 = 0 (REQ_CMS_11: the load filter applies to every CLC, in debug mode too).
//   3-5 halted: DEPCC := the landing capability ca4 made untagged (ccleartag), sealed (cseal with
//      the context's sealer, object type 7; checked tagged and of type 7) or without EX (candperm
//      with ~0x100; checked tagged, EX clear); cmderr 0; resume. The software checks the fault.
//   6  RISC-V, PMP entry 0 locked over rv_dm's memory with no permission: the halt (dmstatus
//      allhalted, the base's dcsr/dpc/GPR checks), an abstract read and a program buffer
//      (`addi t1, t1, 1`) all succeed: cmderr 0, t1 incremented; resume.
// +eg2_dm_bench_fail=1 corrupts the first primary check: the base's dcsr.cause at the first halt.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
// input, so the halts of phases 1-5 run the base debug ROM.
class chip_eg2_rv_dm_cap_bypass_vseq extends chip_eg2_rv_dm_cap_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_cap_bypass_vseq)
  `uvm_object_new

  // Shared with eg2_rv_dm_cap_bypass.c.
  localparam bit [31:0] Words[3] = '{32'h0b0b_0001, 32'h0b0b_0002, 32'h0b0b_0003};
  localparam bit [31:0] Stores[3] = '{32'h5700_00a1, 32'h5700_00a2, 32'h5700_00a3};
  localparam bit [11:0] Auths[3] = '{CtxAuthOob, CtxAuthUt, CtxAuthNp};
  localparam string AuthNames[3] = '{"beyond auth_oob's top", "untagged auth_ut",
                                     "auth_np with GL only"};
  localparam bit [31:0] SealOtype = 32'd7;
  localparam int unsigned ParkSpan = 12;  // eg2_dmx_park's wait loop: three instructions

  protected task phase_bypass();
    bit ok;
    bit [31:0] v, dpc;
    phase_begin(8'd1, ok);
    if (!ok) return;
    wait_parked("phase 1", ok);
    if (!ok) return;
    halt_parked("phase 1: halt", ParkSpan);
    dpc = last_dpc;
    for (int k = 0; k < 3; k++) begin
      string what = {"program buffer through ", AuthNames[k]};
      wr_x({what, ": t2"}, RegGpr + XT2, Stores[k]);
      expect_progbuf(what, '{enc_clc(XA1, XA0, Auths[k]), enc_lw(XA2, XA1, 12'h10),
                             enc_sw(XT2, XA1, 12'h14), enc_clc(XT0, XA1, 12'h18),
                             enc_csc(XA0, XA1, 12'h20), enc_cgettag(XT1, XT0),
                             enc_csc(XT0, XA0, CtxOut + 8 * k), InsnEbreak}, CmdErrNone);
      rd_x({what, ": a2"}, RegGpr + XA2, v);
      chk_eq({what, ": lw loaded the word"}, v, Words[k]);
      rd_x({what, ": t1"}, RegGpr + XT1, v);
      chk_eq({what, ": the CLC kept the tag"}, v, 1);
    end
    expect_progbuf("DEPCC read, DScratchC1 written and read",
                   '{enc_cspecialrw(XT0, ScrDepcc, XZero), enc_cgettag(XT1, XT0),
                     enc_cgetaddr(XT2, XT0), enc_cspecialrw(XZero, ScrDscratchC1, XA0),
                     enc_cspecialrw(XA2, ScrDscratchC1, XZero), enc_csc(XA2, XA0, CtxOut + 24),
                     InsnEbreak}, CmdErrNone);
    rd_x("DEPCC tag", RegGpr + XT1, v);
    chk_eq("DEPCC read in debug mode: tagged", v, 1);
    rd_x("DEPCC address", RegGpr + XT2, v);
    chk_eq("DEPCC read in debug mode: address = dpc", v, dpc);
    expect_progbuf("DScratchC0 written and read",
                   '{enc_cspecialrw(XZero, ScrDscratchC0, XA0),
                     enc_cspecialrw(XA2, ScrDscratchC0, XZero), enc_csc(XA2, XA0, CtxOut + 32),
                     InsnEbreak}, CmdErrNone);
    resume_plain("phase 1: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_marked();
    bit ok;
    bit [31:0] v;
    phase_begin(8'd2, ok);
    if (!ok) return;
    wait_parked("phase 2", ok);
    if (!ok) return;
    halt_parked("phase 2: halt", ParkSpan);
    expect_progbuf("CLC of a marked capability",
                   '{enc_clc(XA1, XA0, CtxAuthMark), enc_clc(XT0, XA1, 12'h0),
                     enc_cgettag(XT1, XT0), enc_csc(XT0, XA0, CtxOut + 40), InsnEbreak},
                   CmdErrNone);
    rd_x("marked CLC's tag", RegGpr + XT1, v);
    chk_eq("CLC in debug mode of a capability based in a marked granule: tag 0 (REQ_CMS_11)", v, 0);
    resume_plain("phase 2: resume");
    end_cmd(1'b1, ok);
  endtask

  protected task phase_depcc(bit [7:0] phase);
    bit ok;
    bit [31:0] v, landing;
    bit [31:0] words[$];
    string what = phase == 3 ? "untagged DEPCC" : phase == 4 ? "sealed DEPCC" : "DEPCC without EX";
    phase_begin(phase, ok);
    if (!ok) return;
    wait_parked(what, ok);
    if (!ok) return;
    halt_parked({what, ": halt"}, ParkSpan);
    rd_x("landing address (a4)", RegGpr + XA4, landing);
    case (phase)
      3: words = '{enc_ccleartag(XT0, XA4), enc_cspecialrw(XZero, ScrDepcc, XT0), InsnEbreak};
      4: words = '{enc_clc(XT1, XA0, CtxSealer), enc_cseal(XT0, XA4, XT1),
                   enc_cspecialrw(XZero, ScrDepcc, XT0), enc_cgettag(XT2, XT0),
                   enc_cgettype(XA2, XT0), InsnEbreak};
      default: words = '{enc_addi(XT1, XZero, 12'heff), enc_candperm(XT0, XA4, XT1),
                         enc_cspecialrw(XZero, ScrDepcc, XT0), enc_cgettag(XT2, XT0),
                         enc_cgetperm(XA2, XT0), InsnEbreak};
    endcase
    expect_progbuf({what, ": set in the program buffer"}, words, CmdErrNone);
    if (phase != 3) begin
      rd_x({what, ": tag"}, RegGpr + XT2, v);
      chk_eq({what, ": construction: still tagged"}, v, 1);
      rd_x({what, ": type or permissions"}, RegGpr + XA2, v);
      if (phase == 4) chk_eq({what, ": construction: object type"}, v, SealOtype);
      else chk_eq({what, ": construction: EX clear"}, v[8], 0);
    end
    resume_plain({what, ": resume"});
    `uvm_info(`gfn, $sformatf("EG2 DM %s: first retirement after resume at 0x%08x (landing 0x%08x)",
                              what, post_pc, landing), UVM_LOW)
    end_cmd(1'b1, ok);
  endtask

  protected task phase_pmp();
    bit ok;
    bit [31:0] v0, v1;
    phase_begin(8'd6, ok);
    if (!ok) return;
    wait_parked("phase 6", ok);
    if (!ok) return;
    // The halt itself runs the debug ROM from the PMP-denied memory (RISC-V Debug Spec A.2).
    halt_parked("phase 6: halt under a locked PMP entry over the debug memory", ParkSpan);
    rd_x("t1 before", RegGpr + XT1, v0);
    expect_progbuf("program buffer under a locked PMP entry over the debug memory",
                   '{enc_addi(XT1, XT1, 12'h1), InsnEbreak}, CmdErrNone);
    rd_x("t1 after", RegGpr + XT1, v1);
    chk_eq("program buffer ran from the PMP-denied debug memory: t1 + 1", v1, v0 + 1);
    resume_plain("phase 6: resume");
    end_cmd(1'b1, ok);
  endtask

  virtual task body();
    bit ok, cheriot;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    cheriot = cheriot_mode();
    `uvm_info(`gfn, $sformatf("EG2 DM cap_bypass: %s mode", cheriot ? "CHERIoT" : "RISC-V"),
              UVM_LOW)
    if (cheriot) begin
      phase_begin(8'd0, ok);
      if (ok) end_cmd(1'b0, ok);
      if (!sw_ended()) phase_bypass();
      if (!sw_ended()) phase_marked();
      for (int p = 3; p <= 5 && !sw_ended(); p++) phase_depcc(p);
    end else begin
      phase_pmp();
    end
    if (!sw_ended()) finish(32'h30);
  endtask

endclass : chip_eg2_rv_dm_cap_bypass_vseq
