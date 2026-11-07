// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_cheriot_progbuf_cap (EG2 spec REQ_CDM_09; "Debug Controller", knobs in "Debug in
// CHERIoT mode"; sw/device/tests/eg2_debug/eg2_rv_dm_cheriot_progbuf_cap.c, whose header gives the
// region and the software's checks), CHERIoT mode only.
//
// REQ_CDM_09: CLC/CSC run by debug abstract commands or the program buffer keep tags and are not
// split by other traffic (RFC CHERIoT Memory Subsystem "Background": a capability access is one
// 64-bit access with its tag; rv_dm theory_of_operation.md "CHERIoT Mode": in debug mode Ibex
// applies no bounds or permission check, and SBA does not clear tags).
//
// The core waits in eg2_dmx_park with ca1 = an authority over the software's region: source slots
// (capability shapes, `cap_class`: a small data capability, one with LD and MC only, a sealed one,
// an untagged one) and destination slots. The bench runs `n_commands` commands drawn by
// `w_cmd_<c>`:
//   register read   a GPR x1-x15 by aarsize 2 = its RVFI value.
//   register write  t2 by aarsize 2, read back.
//   memory access   `lw t2, 0(t1)` of a junk word, against SBA.
//   CLC/CSC         src drawn by cap_class, dst drawn; t2 = a drawn count; the program buffer
//                   `clc ca2, src(ca1); csc ca2, dst(ca1); addi t2, t2, -1; bnez t2, -12;
//                   cgettag t1, ca2; ebreak` runs while the bench makes other traffic on the bus
//                   over SBA (reads of the destination's two words, writes of junk words) before
//                   it polls abstractcs. Checks: cmderr 0; t2 = 0; t1 = the source's tag; each SBA
//                   read of a destination word during the run is that word's old or new value;
//                   afterwards the destination's two words (SBA) are the source's two words.
//   step            one dcsr.step in the wait loop (cause 4).
// At the end the bench writes the destination map (scratch words 8-15: source + 1, or 0) and the
// junk words' last values (scratch words 16-19) for the software, which checks after resume that
// every destination holds its source exactly, tag included (CSetEqualExact), and the junk words.
// +eg2_dm_bench_fail=1 corrupts the first primary check (dcsr.cause at the first halt).
class chip_rv_dm_cheriot_progbuf_cap_vseq extends chip_eg2_rv_dm_cdm_base_vseq;
  `uvm_object_utils(chip_rv_dm_cheriot_progbuf_cap_vseq)
  `uvm_object_new

  typedef enum int unsigned {
    CmdRegRead = 0, CmdRegWrite = 1, CmdMem = 2, CmdCap = 3, CmdStep = 4
  } cmd_e;
  // Shared with eg2_rv_dm_cheriot_progbuf_cap.c.
  localparam int unsigned NumSrc = 4;
  localparam int unsigned NumDst = 8;
  localparam int unsigned NumJunk = 4;
  localparam bit [11:0] OffSrc = 12'h000;
  localparam bit [11:0] OffDst = 12'h040;
  localparam bit [11:0] OffJunk = 12'h080;
  localparam int unsigned ScratchMap = 8;
  localparam int unsigned ScratchJunk = 16;
  localparam int unsigned MaxCmds = 128;

  rand int unsigned n_commands;
  rand int unsigned w_cmd_reg_read, w_cmd_reg_write, w_cmd_mem, w_cmd_cap, w_cmd_step;
  rand int unsigned w_cap_small, w_cap_ld_mc, w_cap_sealed, w_cap_untagged;
  rand int unsigned cmd_pick[MaxCmds];
  rand int unsigned cap_pick[MaxCmds];
  rand bit [2:0] dst_pick[MaxCmds];
  rand bit [3:0] reg_pick[MaxCmds];
  rand bit [31:0] val[MaxCmds];
  rand int unsigned count[MaxCmds];
  rand int unsigned n_sba[MaxCmds];

  constraint cmds_c {
    n_commands inside {[8:MaxCmds]};
    w_cmd_reg_read inside {[1:10]};
    w_cmd_reg_write inside {[1:10]};
    w_cmd_mem inside {[1:10]};
    w_cmd_cap inside {[5:20]};
    w_cmd_step inside {[1:10]};
    w_cap_small inside {[1:10]};
    w_cap_ld_mc inside {[1:10]};
    w_cap_sealed inside {[1:10]};
    w_cap_untagged inside {[1:10]};
    solve w_cmd_reg_read, w_cmd_reg_write, w_cmd_mem, w_cmd_cap, w_cmd_step before cmd_pick;
    solve w_cap_small, w_cap_ld_mc, w_cap_sealed, w_cap_untagged before cap_pick;
    foreach (cmd_pick[i]) {
      cmd_pick[i] < w_cmd_reg_read + w_cmd_reg_write + w_cmd_mem + w_cmd_cap + w_cmd_step;
    }
    foreach (cap_pick[i]) cap_pick[i] < w_cap_small + w_cap_ld_mc + w_cap_sealed + w_cap_untagged;
    foreach (reg_pick[i]) reg_pick[i] inside {[1:15]};
    foreach (count[i]) count[i] dist {[1:8] := 1, [64:1024] := 3};
    foreach (n_sba[i]) n_sba[i] inside {[0:4]};
  }

  bit [31:0] region, tag_mask;
  bit [31:0] src_word[NumSrc][2];
  int dst_src[NumDst];          // -1: untouched (both words 0)
  bit [31:0] junk[NumJunk];

  protected function cmd_e cmd_of(int unsigned pick);
    int unsigned w[5] = '{w_cmd_reg_read, w_cmd_reg_write, w_cmd_mem, w_cmd_cap, w_cmd_step};
    foreach (w[i]) begin
      if (pick < w[i]) return cmd_e'(i);
      pick -= w[i];
    end
    return CmdStep;
  endfunction

  protected function int unsigned cap_of(int unsigned pick);
    int unsigned w[NumSrc] = '{w_cap_small, w_cap_ld_mc, w_cap_sealed, w_cap_untagged};
    foreach (w[i]) begin
      if (pick < w[i]) return i;
      pick -= w[i];
    end
    return NumSrc - 1;
  endfunction

  protected task rd_word(string what, bit [31:0] addr, output bit [31:0] v);
    sba_access_err_e st;
    sba_rd32(addr, v, st);
    chk_eq({what, ": SBA read status"}, st, SbaErrNone);
  endtask

  protected function bit [31:0] dst_word(int unsigned j, int unsigned h);
    return dst_src[j] < 0 ? 32'h0 : src_word[dst_src[j]][h];
  endfunction

  // CLC/CSC from the program buffer with SBA traffic while it runs.
  protected task cmd_cap(int unsigned k);
    int unsigned s = cap_of(cap_pick[k]);
    int unsigned j = dst_pick[k];
    bit [31:0] dst_addr = region + OffDst + 8 * j;
    bit [31:0] v, old_w[2];
    bit [2:0] cmderr;
    string what = $sformatf("cmd %0d: CLC/CSC source %0d -> destination %0d, %0d times", k, s, j,
                            count[k]);
    old_w[0] = dst_word(j, 0);
    old_w[1] = dst_word(j, 1);
    wr_x({what, ": t2 = the count"}, RegGpr + XT2, count[k]);
    // ca2, not ct0: the wait loop dereferences ct0 between its clc and lw.
    progbuf_wr('{enc_clc(XA2, XA1, OffSrc + 8 * s), enc_csc(XA2, XA1, OffDst + 8 * j),
                 enc_addi(XT2, XT2, 12'hfff), enc_bne(XT2, XZero, -13'sd12),
                 enc_cgettag(XT1, XA2), InsnEbreak});
    csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1)),
           .blocking(1), .predict(1));
    // Other traffic while the program buffer runs.
    for (int unsigned i = 0; i < n_sba[k]; i++) begin
      int unsigned h = (val[k] >> i) & 1;
      if (((val[k] >> (8 + i)) & 1) != 0) begin
        sba_access_err_e st;
        int unsigned jw = (val[k] >> (12 + 2 * i)) & (NumJunk - 1);
        junk[jw] = val[k] ^ (32'h1111_1111 * (i + 1));
        sba_wr32(region + OffJunk + 4 * jw, junk[jw], st);
        chk_eq({what, ": SBA write of a junk word"}, st, SbaErrNone);
      end else begin
        rd_word({what, ": SBA read of the destination during the run"}, dst_addr + 4 * h, v);
        chk_true($sformatf("%s: destination word %0d during the run (0x%08x): old or new value",
                           what, h, v), v == old_w[h] || v == src_word[s][h]);
      end
    end
    abs_wait(cmderr);
    chk_eq({what, ": cmderr"}, cmderr, CmdErrNone, 1'b1);
    if (cmderr != 0) begin
      cmderr_clear();
      return;
    end
    rd_x({what, ": t2"}, RegGpr + XT2, v);
    chk_eq({what, ": the loop ran to its end (t2 = 0)"}, v, 0);
    rd_x({what, ": t1"}, RegGpr + XT1, v);
    chk_eq({what, ": CLC in the program buffer: the source's tag"}, v, tag_mask[s], 1'b1);
    for (int unsigned h = 0; h < 2; h++) begin
      rd_word({what, ": SBA read of the destination"}, dst_addr + 4 * h, v);
      chk_eq($sformatf("%s: destination word %0d = the source's (whole)", what, h), v,
             src_word[s][h]);
    end
    dst_src[j] = s;
  endtask

  protected task do_cmd(int unsigned k);
    cmd_e c = cmd_of(cmd_pick[k]);
    string what = $sformatf("cmd %0d %s", k, c.name());
    bit [31:0] v;
    case (c)
      CmdRegRead: begin
        bit [4:0] r = reg_pick[k];
        rd_x(what, RegGpr + r, v);
        if (shadow[r].valid) chk_eq({what, $sformatf(": x%0d = its RVFI value", r)}, v,
                                    shadow[r].data);
      end
      CmdRegWrite: begin
        wr_x(what, RegGpr + XT2, val[k]);
        rd_x({what, ": read back"}, RegGpr + XT2, v);
        chk_eq({what, ": t2 read back"}, v, val[k]);
      end
      CmdMem: begin
        int unsigned jw = val[k] & (NumJunk - 1);
        wr_x({what, ": t1 = a junk word"}, RegGpr + XT1, region + OffJunk + 4 * jw);
        expect_progbuf({what, ": lw t2, 0(t1)"}, '{enc_lw(XT2, XT1, 12'h0), InsnEbreak},
                       CmdErrNone);
        rd_x({what, ": t2"}, RegGpr + XT2, v);
        chk_eq({what, ": the program buffer's load = the junk word"}, v, junk[jw]);
      end
      CmdCap: cmd_cap(k);
      default: begin
        step_once(what);
        chk_true({what, ": dpc in the wait loop"}, last_dpc >= wait_pc &&
                 last_dpc < wait_pc + ParkSpan);
      end
    endcase
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    chk_true("the core runs in CHERIoT mode (this test's build)", cheriot_mode());
    `uvm_info(`gfn, $sformatf("EG2 DM progbuf_cap: %0d commands", n_commands), UVM_LOW)
    park_and_halt("progbuf_cap", 8'd0, ok);
    if (!ok) return;
    scratch_rd(ScratchArg1, region);
    scratch_rd(ScratchArg2, tag_mask);
    foreach (dst_src[j]) dst_src[j] = -1;
    foreach (junk[i]) junk[i] = 0;
    for (int unsigned s = 0; s < NumSrc; s++) begin
      rd_word($sformatf("source %0d word 0", s), region + OffSrc + 8 * s, src_word[s][0]);
      rd_word($sformatf("source %0d word 1", s), region + OffSrc + 8 * s + 4, src_word[s][1]);
    end
    // ca1 is the region's authority (the software's construction).
    begin
      bit [31:0] v;
      rd_x("ca1's address", RegGpr + XA1, v);
      chk_eq("ca1 = the region's authority (stimulus)", v, region);
    end
    for (int unsigned k = 0; k < n_commands && !sw_ended(); k++) do_cmd(k);
    for (int unsigned j = 0; j < NumDst; j++) scratch_wr(ScratchMap + j, dst_src[j] + 1);
    for (int unsigned i = 0; i < NumJunk; i++) scratch_wr(ScratchJunk + i, junk[i]);
    resume_and_end("progbuf_cap", ok);
    if (!sw_ended()) finish(32'h42);
  endtask

endclass : chip_rv_dm_cheriot_progbuf_cap_vseq
