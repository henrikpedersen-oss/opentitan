// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of chip_eg2_rv_dm_cap_bypass, chip_eg2_rv_dm_halt_revocation and chip_eg2_rv_dm_priv
// (EG2 spec "Debug Controller" / "Corner cases"; software in sw/device/tests/eg2_debug, the stage
// protocol in eg2_dm_cap.h). Each phase is one command of eg2_dm_corners.h's protocol (kind 0x10,
// arg = the phase). In a phase the software runs a stage that publishes its wait-loop address in
// scratch word 0 and spins until `stop`; this class waits for that word over SBA, halts the core
// there, runs program buffers against the stage's registers, and resumes.
//
// Program-buffer encodings: RV32I and the CHERIoT ones of CHERIoT-Sail's encdec (the same as the
// core bench's core_ibex_dm_corners_test, which checks them against the assembler).
class chip_eg2_rv_dm_cap_base_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_cap_base_vseq)
  `uvm_object_new

  localparam bit [7:0] KindAction = 8'h10;

  // eg2_dm_cap.h: the context's offsets and the scratch words.
  localparam bit [11:0] CtxAuthOob = 12'd16;
  localparam bit [11:0] CtxAuthUt = 12'd24;
  localparam bit [11:0] CtxAuthNp = 12'd32;
  localparam bit [11:0] CtxAuthMark = 12'd40;
  localparam bit [11:0] CtxSealer = 12'd48;
  localparam bit [11:0] CtxOut = 12'd64;
  localparam int unsigned ScratchFlag = 0;
  localparam int unsigned ScratchArg0 = 1;
  localparam int unsigned ScratchArg1 = 2;
  localparam int unsigned ScratchArg2 = 3;
  localparam int unsigned ScratchArg3 = 4;

  // Registers (x numbers; CHERIoT capability registers have the same numbers).
  localparam bit [4:0] XZero = 5'd0, XT0 = 5'd5, XT1 = 5'd6, XT2 = 5'd7;
  localparam bit [4:0] XA0 = 5'd10, XA1 = 5'd11, XA2 = 5'd12, XA4 = 5'd14;
  localparam bit [15:0] RegGpr = 16'h1000;
  localparam bit [15:0] RegMisa = 16'h0301;
  localparam bit [15:0] RegMepc = 16'h0341;
  localparam bit [15:0] RegMcause = 16'h0342;
  localparam bit [31:0] InsnEcall = 32'h0000_0073;

  // The SCRs of the debug extension (core REQ_SCR_01) and the abstract command's cmderr values.
  localparam bit [4:0] ScrDepcc = 5'd24, ScrDscratchC0 = 5'd25, ScrDscratchC1 = 5'd26;
  localparam bit [2:0] CmdErrNone = 3'd0, CmdErrException = 3'd3;

  // The stage's wait-loop address (scratch word 0).
  bit [31:0] wait_pc;

  // ---------------------------------------------------------------- encodings

  protected function bit [31:0] enc_i(bit [11:0] imm, bit [4:0] rs1, bit [2:0] f3, bit [4:0] rd,
                                      bit [6:0] op);
    return {imm, rs1, f3, rd, op};
  endfunction

  protected function bit [31:0] enc_s(bit [11:0] imm, bit [4:0] rs2, bit [4:0] rs1, bit [2:0] f3);
    return {imm[11:5], rs2, rs1, f3, imm[4:0], 7'h23};
  endfunction

  protected function bit [31:0] enc_lw(bit [4:0] rd, bit [4:0] rs1, bit [11:0] imm);
    return enc_i(imm, rs1, 3'b010, rd, 7'h03);
  endfunction

  protected function bit [31:0] enc_sw(bit [4:0] rs2, bit [4:0] rs1, bit [11:0] imm);
    return enc_s(imm, rs2, rs1, 3'b010);
  endfunction

  protected function bit [31:0] enc_addi(bit [4:0] rd, bit [4:0] rs1, bit [11:0] imm);
    return enc_i(imm, rs1, 3'b000, rd, 7'h13);
  endfunction

  // CLC = the LD encoding, CSC = the SD encoding (CHERIoT mode).
  protected function bit [31:0] enc_clc(bit [4:0] cd, bit [4:0] cs1, bit [11:0] imm);
    return enc_i(imm, cs1, 3'b011, cd, 7'h03);
  endfunction

  protected function bit [31:0] enc_csc(bit [4:0] cs2, bit [4:0] cs1, bit [11:0] imm);
    return enc_s(imm, cs2, cs1, 3'b011);
  endfunction

  // Two-source CHERIoT instructions: funct7 @ rs2 @ rs1 @ 000 @ rd @ 0x5b.
  protected function bit [31:0] enc_cr(bit [6:0] f7, bit [4:0] rd, bit [4:0] rs1, bit [4:0] rs2);
    return {f7, rs2, rs1, 3'b000, rd, 7'h5b};
  endfunction

  // CSpecialRW cd, scr, cs1 (cs1 = c0: read; cd = c0: write).
  protected function bit [31:0] enc_cspecialrw(bit [4:0] cd, bit [4:0] scr, bit [4:0] cs1);
    return enc_cr(7'h01, cd, cs1, scr);
  endfunction

  protected function bit [31:0] enc_cseal(bit [4:0] cd, bit [4:0] cs1, bit [4:0] cs2);
    return enc_cr(7'h0b, cd, cs1, cs2);
  endfunction

  protected function bit [31:0] enc_candperm(bit [4:0] cd, bit [4:0] cs1, bit [4:0] rs2);
    return enc_cr(7'h0d, cd, cs1, rs2);
  endfunction

  // Single-source, funct7 0x7f, the operation in the rs2 slot.
  protected function bit [31:0] enc_cgetperm(bit [4:0] rd, bit [4:0] cs1);
    return enc_cr(7'h7f, rd, cs1, 5'd0);
  endfunction

  protected function bit [31:0] enc_cgettype(bit [4:0] rd, bit [4:0] cs1);
    return enc_cr(7'h7f, rd, cs1, 5'd1);
  endfunction

  protected function bit [31:0] enc_cgettag(bit [4:0] rd, bit [4:0] cs1);
    return enc_cr(7'h7f, rd, cs1, 5'd4);
  endfunction

  protected function bit [31:0] enc_ccleartag(bit [4:0] cd, bit [4:0] cs1);
    return enc_cr(7'h7f, cd, cs1, 5'd11);
  endfunction

  protected function bit [31:0] enc_cgetaddr(bit [4:0] rd, bit [4:0] cs1);
    return enc_cr(7'h7f, rd, cs1, 5'd15);
  endfunction

  // ---------------------------------------------------------------- scratch words (SBA)

  protected task scratch_rd(int unsigned idx, output bit [31:0] v);
    sba_access_err_e st;
    sba_rd32(scratch_addr + 4 * idx, v, st);
    chk_eq($sformatf("SBA read of scratch word %0d", idx), st, SbaErrNone);
  endtask

  protected task scratch_wr(int unsigned idx, bit [31:0] v);
    sba_access_err_e st;
    sba_wr32(scratch_addr + 4 * idx, v, st);
    chk_eq($sformatf("SBA write of scratch word %0d", idx), st, SbaErrNone);
  endtask

  // ---------------------------------------------------------------- phases

  // Starts phase `arg`: the flag cleared first (so a stale one is never taken), the command sent.
  protected task phase_begin(bit [7:0] arg, output bit ok);
    scratch_wr(ScratchFlag, 0);
    send_cmd(KindAction, arg, ok);
  endtask

  // Waits until the stage publishes its wait loop (scratch word 0 != 0).
  protected task wait_parked(string what, output bit ok, input int unsigned max_polls = 2000);
    bit [31:0] v;
    int unsigned n;
    do begin
      scratch_rd(ScratchFlag, v);
      n++;
    end while (v == 0 && n < max_polls && !sw_ended());
    ok = v != 0;
    if (!ok && !sw_ended()) `uvm_error(`gfn, $sformatf("EG2 DM %s: the stage never waited", what))
    wait_pc = v;
  endtask

  // Halts the core in the stage's wait loop of `span` bytes; the halt checked as in the base.
  protected task halt_parked(string what, int unsigned span);
    halt_checked(what);
    chk_true($sformatf("%s: dpc 0x%08x in the stage's wait loop [0x%08x, +%0d)", what, last_dpc,
                       wait_pc, span), last_dpc >= wait_pc && last_dpc < wait_pc + span);
  endtask

  // Resumes without the base's register comparison (the program buffers changed registers).
  protected task resume_plain(string what);
    int unsigned r0 = resumes;
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
  endtask

  // Loads and runs a program buffer (postexec, no transfer); returns cmderr.
  protected task run_progbuf(string what, bit [31:0] words[$], output bit [2:0] cmderr);
    if (words.size() == 0 || words.size() > 8 || words[$] != InsnEbreak) begin
      `uvm_fatal(`gfn, $sformatf("%s: a program buffer is 1-8 words ending in ebreak", what))
    end
    progbuf_wr(words);
    abs_cmd(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1), cmderr);
    `uvm_info(`gfn, $sformatf("EG2 DM program buffer '%s': cmderr %0d", what, cmderr), UVM_LOW)
  endtask

  // As run_progbuf, cmderr checked (a primary check) and cleared if set.
  protected task expect_progbuf(string what, bit [31:0] words[$], bit [2:0] exp_cmderr);
    bit [2:0] cmderr;
    run_progbuf(what, words, cmderr);
    chk_eq({what, ": cmderr"}, cmderr, exp_cmderr, 1'b1);
    if (cmderr != 0) cmderr_clear();
  endtask

  protected task rd_x(string what, bit [15:0] regno, output bit [31:0] v);
    bit [2:0] cmderr;
    gpr_rd(regno, v, cmderr);
    chk_eq({what, ": abstract read cmderr"}, cmderr, CmdErrNone);
  endtask

  protected task wr_x(string what, bit [15:0] regno, bit [31:0] v);
    bit [2:0] cmderr;
    gpr_wr(regno, v, cmderr);
    chk_eq({what, ": abstract write cmderr"}, cmderr, CmdErrNone);
  endtask

  protected function bit cheriot_mode();
    return rvfi.cheriot_ena === 1'b1;
  endfunction

endclass : chip_eg2_rv_dm_cap_base_vseq
