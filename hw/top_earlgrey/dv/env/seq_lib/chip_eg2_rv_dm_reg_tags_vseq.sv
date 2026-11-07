// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_reg_tags (EG2 spec REQ_CDM_08; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_reg_tags.c, whose header lists the software's checks),
// CHERIoT mode only.
//
// Expected values (rv_dm theory_of_operation.md "CHERIoT Mode"): tags are not carried through
// data0/data1 in either direction, so a register written by an abstract command (aarsize 2, or
// aarsize 3 with the address in data0 and the metadata in data1) is untagged; abstract commands
// save and restore their scratch registers with CSpecialRW, so every other register keeps its
// capability and tag; "In debug mode, Ibex disables its CHERIoT bounds and permission checks".
//
// Phase 0 (abstract writes): the core waits in eg2_dmx_park with tagged capabilities in ca0 (the
// context), ca1 and ca2 (the software's, see the C header) and ca4 (the landing capability). The
// bench reads ca0 by aarsize 3 (data0 = its RVFI address), then writes the drawn subset of
// {ca1, ca2, ca4}, each by aarsize 2 (a drawn integer) or aarsize 3 (ca0's address and metadata),
// and reads each back (aarsize 3: the same data0 and data1). `cgettag` in the program buffer:
// every written register 0; ca0, csp and every unwritten one of ca1, ca2, ca4 1. RVFI: every
// register other than the written ones and the program buffer's t1 holds the capability (value and
// metadata, tag included) it held at the halt. What it wrote goes to scratch words 1-3 for the
// software's checks after resume.
// Phases 1 and 2 (SCRs from the program buffer, with and without SR): the core waits in
// eg2_dmx_park entered through a PCC with SR (1) or with SR cleared (2). The bench checks that
// DEPCC is tagged and has SR exactly in phase 1 (the halted code's PCC), then reads MTCC (28) and
// MScratchC (30) with `cspecialr` in the program buffer (into ca2; ct0 is the wait loop's) and by aarsize 3 (SCR regno 0x1c, 0x1e):
// cmderr 0, tagged, address = the value the software published (scratch words 1, 2).
// EXPECTATION FROM THE DOCUMENTS, NOT THE SPEC ROW: the spec's row says "CSpecialRW without SR
// traps in the program buffer"; rv_dm's documentation (above), the spec's own
// chip_eg2_rv_dm_cap_bypass row ("No capability fault in debug mode") and the CHERIoT debug ROM
// (which saves its scratch register with CSpecialRW at every halt, wherever the core halted) give
// no trap. This test expects cmderr 0 in phase 2; the disagreement is reported (dv_todo).
// +eg2_dm_bench_fail=1 corrupts the first primary check (dcsr.cause at the first halt).
class chip_eg2_rv_dm_reg_tags_vseq extends chip_eg2_rv_dm_cdm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_reg_tags_vseq)
  `uvm_object_new

  localparam bit [4:0] Targets[3] = '{XA1, XA2, XA4};
  localparam bit [4:0] ScrMtcc = 5'd28, ScrMscratchc = 5'd30;
  localparam int unsigned PermSrBit = 7;  // CHERIoT ISA architectural permission bit (SR)

  rand bit [2:0] wr_en;       // the targets written
  rand bit [2:0] wr_cap;      // written by aarsize 3 (else aarsize 2)
  rand bit [31:0] wr_val[3];  // the aarsize 2 values
  rand bit [1:0] scr_order;   // phase order: SR first or not

  constraint wr_c {
    wr_en != 0;
  }

  protected task phase_writes();
    bit ok;
    bit [2:0] cmderr;
    bit [31:0] c0_addr, c0_meta, v, m;
    bit [31:0] written[3];
    bit [4:0] tag_regs[$];
    park_and_halt("abstract writes", 8'd0, ok);
    if (!ok) return;
    cap_rd(RegGpr + XA0, c0_addr, c0_meta, cmderr);
    chk_eq("ca0 by aarsize 3: cmderr", cmderr, CmdErrNone, 1'b1);
    chk_eq("ca0 by aarsize 3: data0 = its address", c0_addr, snap[XA0].data);
    foreach (Targets[i]) begin
      string what = $sformatf("x%0d written by aarsize %0d", Targets[i], wr_cap[i] ? 3 : 2);
      if (!wr_en[i]) continue;
      if (wr_cap[i]) begin
        cap_wr(RegGpr + Targets[i], c0_addr, c0_meta, cmderr);
        chk_eq({what, ": cmderr"}, cmderr, CmdErrNone);
        cap_rd(RegGpr + Targets[i], v, m, cmderr);
        chk_eq({what, ": read back by aarsize 3: cmderr"}, cmderr, CmdErrNone);
        chk_eq({what, ": read back: data0 = the address written"}, v, c0_addr, 1'b1);
        chk_eq({what, ": read back: data1 = the metadata written"}, m, c0_meta);
        written[i] = c0_addr;
      end else begin
        wr_x(what, RegGpr + Targets[i], wr_val[i]);
        rd_x({what, ": read back"}, RegGpr + Targets[i], v);
        chk_eq({what, ": read back = the value written"}, v, wr_val[i], 1'b1);
        written[i] = wr_val[i];
      end
    end
    // Tags through the program buffer (t1 is the scratch register).
    tag_regs = '{XA0, XSp, XA1, XA2, XA4};
    foreach (tag_regs[k]) begin
      bit exp = 1'b1;
      foreach (Targets[i]) if (wr_en[i] && Targets[i] == tag_regs[k]) exp = 1'b0;
      expect_progbuf($sformatf("cgettag t1, x%0d", tag_regs[k]),
                     '{enc_cgettag(XT1, tag_regs[k]), InsnEbreak}, CmdErrNone);
      rd_x("t1", RegGpr + XT1, v);
      chk_eq($sformatf("x%0d's tag after the abstract writes (written: 0, else kept)",
                       tag_regs[k]), v, exp, 1'b1);
    end
    // RVFI: every other register as at the halt, capability metadata and tag included.
    for (int unsigned r = 1; r < 16; r++) begin
      bit skip = r == XT1;
      foreach (Targets[i]) if (wr_en[i] && Targets[i] == r) skip = 1'b1;
      if (skip || !snap[r].valid) continue;
      chk_eq($sformatf("x%0d unchanged by the abstract commands (value)", r), shadow[r].data,
             snap[r].data);
      chk_eq($sformatf("x%0d unchanged by the abstract commands (capability, tag)", r),
             shadow[r].cap, snap[r].cap);
    end
    // For the software: the targets written (bit i, aarsize 3 in bit 4 + i), the values.
    scratch_wr(ScratchArg1, {24'h0, 1'b0, wr_cap & wr_en, 1'b0, wr_en});
    scratch_wr(ScratchArg2, written[0]);
    scratch_wr(ScratchArg3, written[1]);
    resume_and_end("abstract writes", ok);
  endtask

  protected task phase_scr(bit with_sr);
    bit ok;
    bit [2:0] cmderr;
    bit [31:0] v, m, exp_addr[2];
    bit [4:0] scrs[2] = '{ScrMtcc, ScrMscratchc};
    string ph = with_sr ? "SCRs with SR" : "SCRs without SR";
    park_and_halt(ph, with_sr ? 8'd1 : 8'd2, ok);
    if (!ok) return;
    scratch_rd(ScratchArg1, exp_addr[0]);
    scratch_rd(ScratchArg2, exp_addr[1]);
    expect_progbuf({ph, ": DEPCC's tag and permissions"},
                   '{enc_cspecialrw(XA2, ScrDepcc, XZero), enc_cgetperm(XT1, XA2),
                     enc_cgettag(XT2, XA2), InsnEbreak}, CmdErrNone);
    rd_x({ph, ": DEPCC tag"}, RegGpr + XT2, v);
    chk_eq({ph, ": DEPCC (the halted code's PCC) tagged"}, v, 1);
    rd_x({ph, ": DEPCC permissions"}, RegGpr + XT1, v);
    chk_eq({ph, ": DEPCC's SR = the stage's PCC's (stimulus)"}, v[PermSrBit], with_sr);
    foreach (scrs[i]) begin
      string what = $sformatf("%s: SCR %0d", ph, scrs[i]);
      expect_progbuf({what, ": cspecialr in the program buffer"},
                     '{enc_cspecialrw(XA2, scrs[i], XZero), enc_cgettag(XT1, XA2),
                       enc_cgetaddr(XT2, XA2), InsnEbreak}, CmdErrNone);
      rd_x({what, ": tag"}, RegGpr + XT1, v);
      chk_eq({what, ": read in the program buffer: tagged"}, v, 1);
      rd_x({what, ": address"}, RegGpr + XT2, v);
      chk_eq({what, ": read in the program buffer: address"}, v, exp_addr[i]);
      cap_rd(16'(scrs[i]), v, m, cmderr);
      chk_eq({what, ": aarsize 3 read: cmderr"}, cmderr, CmdErrNone);
      if (cmderr == CmdErrNone) chk_eq({what, ": aarsize 3 read: data0 = the address"}, v,
                                       exp_addr[i]);
      else cmderr_clear();
    end
    resume_and_end(ph, ok);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    chk_true("the core runs in CHERIoT mode (this test's build)", cheriot_mode());
    `uvm_info(`gfn, $sformatf("EG2 DM reg_tags: written %b, by aarsize 3 %b", wr_en, wr_cap),
              UVM_LOW)
    phase_writes();
    if (!sw_ended()) phase_scr(scr_order[0]);
    if (!sw_ended()) phase_scr(!scr_order[0]);
    if (!sw_ended()) finish(32'h41);
  endtask

endclass : chip_eg2_rv_dm_reg_tags_vseq
