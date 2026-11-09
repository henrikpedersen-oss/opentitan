// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// RTL side of the C = RTL proof of rv_dm's Access Register abstract commands.
//
// dm_mem (EG2 parameters) gets one Access Register command. The ten instruction words of its
// abstract-command program are read back over dm_mem's own hart bus port, as the hart reads them,
// one word per cycle. A symbolic interpreter, written from the RISC-V and CHERIoT instruction set
// documents and not from dm_mem, runs those words on an EG2 hart (RV32I, Zicsr, CHERIoT
// CSpecialRW, no F) whose register values are symbols: "the original x5", "the original CSR 0x300",
// "the value the debugger put in data0", "the address of the program". It gives the effect of the
// program: which GPR, CSR or SCR changed and to what, what landed in data0, and how it stopped.
// That effect is compared with the documented effect from dm_ar_semantic.c (exp_i, from Bambu).

module dm_ar_check (
  input  logic        clk_i,
  input  logic        rst_ni,
  input  logic [15:0] regno_i,
  input  logic [2:0]  aarsize_i,
  input  logic        write_i,
  input  logic        postinc_i,
  input  logic        postexec_i,
  input  logic        cheriot_i,
  input  logic        restrict_i,  // only the documented, implemented register numbers
  input  logic [31:0] exp_i,       // dm_ar_semantic() result
  output logic        words_valid_o,
  output logic        excluded_o,
  output logic        ok_o
);
  // ------------------------------------------------------------------------------------------
  // dm_mem with the EG2 parameters (rv_dm.sv: NrHarts 1, BusWidth 32, DmBaseAddress 1)
  // ------------------------------------------------------------------------------------------
  localparam logic [31:0] AbsCmdAddr = 32'(dm::DataAddr) - 32'(4 * dm::ProgBufSize) - 32'd40;

  logic [3:0]  cnt_q;
  logic [31:0] words_q [10];
  logic [31:0] rdata;
  logic        cmderr_valid;
  dm::cmderr_e cmderr;
  dm::command_t cmd;

  assign cmd.cmdtype = dm::AccessRegister;
  assign cmd.control = {1'b0, aarsize_i, postinc_i, postexec_i, 1'b1, write_i, regno_i};

  dm_mem #(
    .NrHarts        (1),
    .BusWidth       (32),
    .SelectableHarts(1'b1),
    .DmBaseAddress  (1),
    .CheriotSupport (1'b1)
  ) u_dm_mem (
    .clk_i,
    .rst_ni,
    .debug_req_o      (),
    .cheriot_enable_i (cheriot_i ? prim_mubi_pkg::MuBi4True : prim_mubi_pkg::MuBi4False),
    .ndmreset_i       (1'b0),
    .hartsel_i        ('0),
    .haltreq_i        ('0),
    .resumereq_i      ('0),
    .clear_resumeack_i(1'b0),
    .halted_o         (),
    .resuming_o       (),
    .progbuf_i        ('0),
    .data_i           ('0),
    .data_o           (),
    .data_valid_o     (),
    .cmd_valid_i      (1'b1),
    .cmd_i            (cmd),
    .cmderror_valid_o (cmderr_valid),
    .cmderror_o       (cmderr),
    .cmdbusy_o        (),
    .req_i            (cnt_q < 4'd10),
    .we_i             (1'b0),
    .addr_i           (AbsCmdAddr + {26'b0, cnt_q, 2'b00}),
    .wdata_i          ('0),
    .be_i             ('1),
    .rdata_o          (rdata),
    .err_o            ()
  );

  // Word k is requested in cycle k and comes back (registered) in cycle k + 1.
  always_ff @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      cnt_q <= '0;
      for (int k = 0; k < 10; k++) words_q[k] <= '0;
    end else if (cnt_q != 4'd11) begin
      cnt_q <= cnt_q + 4'd1;
      if (cnt_q != 4'd0) words_q[cnt_q - 4'd1] <= rdata;
    end
  end
  assign words_valid_o = (cnt_q == 4'd11);

  // ------------------------------------------------------------------------------------------
  // Symbolic EG2 hart
  // ------------------------------------------------------------------------------------------
  typedef enum logic [3:0] {
    KGpr     = 4'd0,  // original value of GPR idx
    KPc      = 4'd1,  // address of the instruction (auipc rd, 0)
    KPcShr   = 4'd2,  // that address >> 12
    KBase    = 4'd3,  // that address with the low 12 bits cleared: the debug memory base
    KData    = 4'd4,  // the debugger's data0 (and data1) value, loaded with width w
    KCsr     = 4'd5,  // original value of CSR idx
    KScr     = 4'd6,  // original value of SCR idx
    KData0   = 4'd7,  // data0 left as the debugger wrote it
    KScratch = 4'd8,  // original content of a debug scratch register
    KJunk    = 4'd9   // anything else
  } kind_e;

  typedef struct packed {
    kind_e       kind;
    logic [11:0] idx;
    logic [2:0]  w;
  } val_t;

  typedef struct packed {
    val_t [31:0] x;
    val_t        d0;       // the data0/data1 window at DataAddr
    val_t [1:0]  scratch;  // the DM's scratch registers: dscratch0/1, or SCR 25/26 in CHERIoT
    logic        eff;      // a CSR or SCR other than the scratch registers was written
    logic        eff2;     // more than one such write
    logic [2:0]  eff_cls;
    logic [11:0] eff_idx;
    val_t        eff_val;
    logic        stop;
    logic        brk;      // stopped at ebreak
    logic        exc;      // the hart takes an exception (illegal instruction)
    logic        unk;      // an instruction or address the interpreter does not know
  } st_t;

  localparam logic [2:0] ClsGpr = 3'd1, ClsCsr = 3'd2, ClsScr = 3'd3;

  function automatic val_t mk(kind_e k, logic [11:0] i, logic [2:0] w);
    mk.kind = k;
    mk.idx  = i;
    mk.w    = w;
  endfunction

  function automatic val_t rd_x(st_t s, logic [4:0] r);
    return (r == 5'd0) ? mk(KGpr, 12'd0, 3'd0) : s.x[r];
  endfunction

  function automatic st_t wr_x(st_t s, logic [4:0] r, val_t v);
    st_t n = s;
    if (r != 5'd0) n.x[r] = v;
    return n;
  endfunction

  function automatic st_t effect(st_t s, logic [2:0] cls, logic [11:0] idx, val_t v);
    st_t n = s;
    n.eff2    = s.eff;
    n.eff     = 1'b1;
    n.eff_cls = cls;
    n.eff_idx = idx;
    n.eff_val = v;
    return n;
  endfunction

  function automatic st_t step(st_t s, logic [31:0] insn, logic cheriot);
    st_t         n      = s;
    logic [6:0]  opcode = insn[6:0];
    logic [4:0]  rd     = insn[11:7];
    logic [2:0]  f3     = insn[14:12];
    logic [4:0]  rs1    = insn[19:15];
    logic [4:0]  rs2    = insn[24:20];
    logic [11:0] imm_i  = insn[31:20];
    logic [11:0] imm_s  = {insn[31:25], insn[11:7]};
    logic        csr_scratch;
    logic        scr_scratch;
    logic        size_ok;
    val_t        v;
    if (s.stop) return s;
    // dscratch0/1 (0x7b2/0x7b3) are the DM's scratch registers in RV32 mode, SCR 25/26 in CHERIoT
    csr_scratch = !cheriot && (imm_i == 12'h7b2 || imm_i == 12'h7b3);
    scr_scratch = cheriot && (rs2 == 5'd25 || rs2 == 5'd26);
    // RV32: byte, half, word. CHERIoT adds clc/csc (funct3 3, a 64-bit capability).
    size_ok     = (f3 <= 3'd2) || (cheriot && f3 == 3'd3);
    if (insn == 32'h0010_0073) begin                       // ebreak
      n.stop = 1'b1;
      n.brk  = 1'b1;
    end else if (insn == 32'h0000_0000) begin              // illegal
      n.stop = 1'b1;
      n.exc  = 1'b1;
    end else if (insn == 32'h0000_0013) begin              // nop
    end else if (opcode == 7'h17 && insn[31:12] == '0) begin  // auipc rd, 0
      n = wr_x(s, rd, mk(KPc, '0, '0));
    end else if (opcode == 7'h13 && f3 == 3'd5 && insn[31:20] == 12'd12) begin  // srli rd, rs1, 12
      n = wr_x(s, rd, rd_x(s, rs1).kind == KPc ? mk(KPcShr, '0, '0) : mk(KJunk, '0, '0));
    end else if (opcode == 7'h13 && f3 == 3'd1 && insn[31:20] == 12'd12) begin  // slli rd, rs1, 12
      n = wr_x(s, rd, rd_x(s, rs1).kind == KPcShr ? mk(KBase, '0, '0) : mk(KJunk, '0, '0));
    end else if (opcode == 7'h73 && f3 == 3'd1 && rd == 5'd0) begin  // csrw imm_i, rs1
      if (csr_scratch) n.scratch[imm_i[0]] = rd_x(s, rs1);
      else             n = effect(s, ClsCsr, imm_i, rd_x(s, rs1));
    end else if (opcode == 7'h73 && f3 == 3'd2 && rs1 == 5'd0) begin  // csrr rd, imm_i
      n = wr_x(s, rd, csr_scratch ? s.scratch[imm_i[0]] : mk(KCsr, imm_i, '0));
    end else if (cheriot && opcode == 7'h5b && f3 == 3'd0 && insn[31:25] == 7'h01) begin
      // cspecialrw rd, scr(rs2), rs1: rd gets the old SCR; the SCR gets rs1 unless rs1 is c0
      v = scr_scratch ? s.scratch[rs2 == 5'd26] : mk(KScr, {7'd0, rs2}, '0);
      if (rs1 != 5'd0) begin
        if (scr_scratch) n.scratch[rs2 == 5'd26] = rd_x(s, rs1);
        else             n = effect(s, ClsScr, {7'd0, rs2}, rd_x(s, rs1));
      end
      n = wr_x(n, rd, v);
    end else if (opcode == 7'h03 && size_ok && rd_x(s, rs1).kind == KBase &&
                 imm_i == dm::DataAddr) begin               // load from data0
      n = wr_x(s, rd, s.d0.kind == KData0 ? mk(KData, '0, f3) : mk(KJunk, '0, '0));
    end else if (opcode == 7'h23 && size_ok && rd_x(s, rs1).kind == KBase &&
                 imm_s == dm::DataAddr) begin               // store to data0
      v      = rd_x(s, rs2);
      n.d0   = mk(v.kind, v.idx, f3);
    end else if (opcode == 7'h07 || opcode == 7'h27 || opcode == 7'h03 && f3 == 3'd3) begin
      // FP loads and stores (no F extension), ld/sd (RV32): illegal on the EG2 hart
      n.stop = 1'b1;
      n.exc  = 1'b1;
    end else begin
      n.stop = 1'b1;
      n.unk  = 1'b1;
    end
    return n;
  endfunction

  st_t st [11];
  always_comb begin
    st[0] = '0;
    for (int i = 0; i < 32; i++) st[0].x[i] = mk(KGpr, 12'(i), '0);
    st[0].d0         = mk(KData0, '0, '0);
    st[0].scratch[0] = mk(KScratch, 12'd0, '0);
    st[0].scratch[1] = mk(KScratch, 12'd1, '0);
    for (int k = 0; k < 10; k++) st[k + 1] = step(st[k], words_q[k], cheriot_i);
  end

  // ------------------------------------------------------------------------------------------
  // Compare with the documented effect
  // ------------------------------------------------------------------------------------------
  logic        e_sup, e_wr, e_pe;
  logic [2:0]  e_cls, e_w;
  logic [11:0] e_idx;
  st_t         f;
  val_t        e_src;
  logic        gprs_ok, d0_ok, eff_ok, stop_ok, err_ok;

  assign e_sup = exp_i[0];
  assign e_wr  = exp_i[1];
  assign e_cls = exp_i[4:2];
  assign e_idx = exp_i[16:5];
  assign e_w   = exp_i[19:17];
  assign e_pe  = exp_i[20];
  assign f     = st[10];

  always_comb begin
    gprs_ok = 1'b1;
    for (int i = 1; i < 32; i++) begin
      if (e_sup && e_wr && e_cls == ClsGpr && e_idx == 12'(i)) begin
        gprs_ok &= (f.x[i] == mk(KData, '0, e_w));
      end else begin
        gprs_ok &= (f.x[i] == mk(KGpr, 12'(i), '0));
      end
    end
    unique case (e_cls)
      ClsGpr:  e_src = mk(KGpr, e_idx, e_w);
      ClsCsr:  e_src = mk(KCsr, e_idx, e_w);
      default: e_src = mk(KScr, e_idx, e_w);
    endcase
    d0_ok  = (e_sup && !e_wr) ? (f.d0 == e_src) : (f.d0 == mk(KData0, '0, '0));
    eff_ok = (e_sup && e_wr && e_cls != ClsGpr) ?
             (f.eff && !f.eff2 && f.eff_cls == e_cls && f.eff_idx == e_idx &&
              f.eff_val == mk(KData, '0, e_w)) :
             !f.eff;
    // Supported: the program ends at ebreak, or runs into the program buffer for postexec.
    // Not supported: it stops at ebreak and dm_mem reports cmderr 2 (not supported).
    stop_ok = !f.unk && !f.exc && ((e_sup && e_pe) ? !f.stop : f.brk);
    err_ok  = (cmderr_valid && cmderr == dm::CmdErrNotSupported) == !e_sup;
  end

  assign ok_o = gprs_ok && d0_ok && eff_ok && stop_ok && err_ok;

  // The DM keeps a0 and s0 in its scratch registers while the program runs, so the debug spec
  // (4.8.3, 4.8.4) leaves them to the DM: commands naming them are outside the comparison.
  // restrict_i keeps to the register numbers the documents name and EG2 implements, without
  // aarpostincrement (a finding: dm_mem runs the access and ignores it, see proof 4).
  assign excluded_o =
      (!cheriot_i && regno_i inside {16'h07b2, 16'h07b3}) ||
      (cheriot_i && aarsize_i == 3'd3 && regno_i inside {16'h0019, 16'h001a}) ||
      (restrict_i && (postinc_i || !(regno_i < 16'h1020 || regno_i >= 16'hc000)));
endmodule
