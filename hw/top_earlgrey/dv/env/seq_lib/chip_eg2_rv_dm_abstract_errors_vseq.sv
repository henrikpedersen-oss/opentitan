// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_abstract_errors (EG2 spec REQ_CDM_03; "Debug Controller" / "Corner cases";
// sw/device/tests/eg2_debug/eg2_rv_dm_abstract_errors.c), both modes. The core runs the PARK
// kernel (a counting loop whose a0 and t1 are the debugger's).
//
// Expected values from the RISC-V Debug Specification 0.13.2: 3.12.6 abstractcs.cmderr ("1
// busy: an abstract command was executing while command, abstractcs or abstractauto was written,
// or when one of the data or progbuf registers was read or written; ... only written if cmderr
// contains 0"; "2 not supported"; "3 exception"; "4 halt/resume: the hart was not in the required
// state"; "If cmderr is non-zero, writes to this register [command] are ignored"; W1C), 3.6.1.1
// Access Register ("If any of these operations fail, cmderr is set and none of the remaining
// steps are executed"; "If the failure is that the requested register does not exist in the
// hart, cmderr must be set to 3"; "If aarsize specifies a size larger than the register's actual
// size, then the access must fail"), 3.12.7 abstractauto, 4.1 ("Exceptions don't update any
// registers. That includes cause, epc, tval, dpc, and mstatus"); rv_dm's documentation
// (pulp_riscv_dbg debug-system.md: "Non-intrusive access to core registers: not supported").
//
//  | case                  | stimulus                                         | expected      |
//  |-----------------------|--------------------------------------------------|---------------|
//  | running               | register read with the core running              | 4; data0 kept |
//  | aarsize               | a0 written and read with aarsize 3 and 4         | 2; a0 kept;   |
//  |                       |   (with postexec: the program buffer not run)    |   no postexec |
//  | cmdtype               | a reserved cmdtype (3-255)                       | 2             |
//  | no such register      | a CSR Ibex does not have, an FPR, 0xc000-0xffff  | 3             |
//  | cmderr set            | a register write while cmderr is 3               | ignored       |
//  | progbuf exception     | an illegal instruction in the program buffer,    | 3; halted;    |
//  |                       |   alone and after a register write               |   dpc, mcause |
//  |                       |                                                  |   kept; the   |
//  |                       |                                                  |   write done  |
//  | postexec              | a0 written, program buffer `addi a0, a0, 1`      | a0 = value+1  |
//  | autoexecdata          | data0 read n times, the command reading a0 with  | V, V+1, ...   |
//  |                       |   postexec `addi`                                |               |
//  | autoexecprogbuf       | progbuf0 written n times                         | a0 + n        |
//  | busy                  | command and data0 written while a long program   | 1; the        |
//  |                       |   buffer loop runs                               |   refused     |
//  |                       |                                                  |   write not   |
//  |                       |                                                  |   done        |
//
// The halt and resume are checked as in chip_eg2_rv_dm_corners_base_vseq (registers and tags
// unchanged once a0 and t1 are restored); the software checks the loop ran undisturbed.
//
// CHERIoT mode: aarsize 3 is expected to fail with 2 there too (RFC CHERIoT Debug Module
// "Architecture": no full-capability register access in a first step). EXPECTED FAILURE in CHERIoT
// mode at the halt until ticket F13 (rv_dm has no CHERIoT input); the running-core case passes.
class chip_eg2_rv_dm_abstract_errors_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_abstract_errors_vseq)
  `uvm_object_new

  localparam bit [15:0] RegMcause = 16'h0342;

  rand bit [31:0] vals[6];
  rand bit [15:0] bad_csr;
  rand bit [7:0] bad_cmdtype;
  rand int unsigned n_auto;

  constraint knobs_c {
    // CSR numbers Ibex does not implement (cs_registers.rst): the unprivileged and supervisor
    // custom ranges.
    bad_csr inside {[16'h05c0:16'h05ff], [16'h08c0:16'h08ff]};
    bad_cmdtype inside {[3:255]};
    n_auto inside {[2:6]};
    foreach (vals[i]) vals[i][31:28] != 4'hf;
  }

  protected task expect_cmderr(string what, bit [2:0] got, bit [2:0] exp);
    chk_eq({what, ": cmderr"}, got, exp, 1'b1);
    if (got != 0) cmderr_clear();
  endtask

  protected task abstractauto_wr(bit data0, bit progbuf0);
    uvm_reg_data_t v = 0;
    bit [2:0] err;
    abs_wait(err);
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.abstractauto.autoexecdata, v, data0);
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.abstractauto.autoexecprogbuf, v,
                                       progbuf0);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractauto), .value(v), .blocking(1), .predict(1));
  endtask

  virtual task body();
    bit ok;
    bit [2:0] err;
    bit [31:0] a0_saved, t1_saved, v, dpc0, dpc1, mcause0, mcause1;
    uvm_reg_data_t st;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    send_cmd(9, 0, ok);  // PARK
    if (!ok) return;

    // 4: a register access on a running core; data0 kept.
    data0_wr(vals[0]);
    abs_cmd(reg_cmd(RegA0, 1'b0), err);
    expect_cmderr("register read on a running core", err, 4);
    data0_rd(v);
    chk_eq("data0 after the refused read", v, vals[0]);

    halt_checked("abstract errors: halt");
    gpr_rd(RegA0, a0_saved, err);
    expect_cmderr("a0 read", err, 0);
    gpr_rd(RegT1, t1_saved, err);
    expect_cmderr("t1 read", err, 0);

    // 2: aarsize larger than the register; with postexec the program buffer must not run.
    progbuf_wr({InsnAddiA0, InsnEbreak});
    data0_wr(vals[1]);
    abs_cmd(reg_cmd(RegA0, 1'b1, 1'b1, 1'b0, 3'd3), err);
    expect_cmderr("a0 write with aarsize 3", err, 2);
    abs_cmd(reg_cmd(RegA0, 1'b1, 1'b1, 1'b1, 3'd3), err);
    expect_cmderr("a0 write with aarsize 3 and postexec", err, 2);
    abs_cmd(reg_cmd(RegA0, 1'b0, 1'b1, 1'b0, 3'd4), err);
    expect_cmderr("a0 read with aarsize 4", err, 2);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after the refused writes (no transfer, no postexec)", v, a0_saved);

    // 2: a reserved command type.
    abs_cmd({bad_cmdtype, 24'h0}, err);
    expect_cmderr($sformatf("cmdtype %0d", bad_cmdtype), err, 2);

    // 3: registers the hart does not have.
    abs_cmd(reg_cmd(bad_csr, 1'b0), err);
    expect_cmderr($sformatf("CSR 0x%03x read", bad_csr), err, 3);
    abs_cmd(reg_cmd(16'h1020, 1'b0), err);
    expect_cmderr("FPR f0 read (no F extension)", err, 3);
    abs_cmd(reg_cmd(16'hc000, 1'b0), err);
    expect_cmderr("register 0xc000 read", err, 3);

    // A command written while cmderr is set is ignored.
    abs_cmd(reg_cmd(16'h1020, 1'b0), err);
    chk_eq("cmderr before the ignored write", err, 3);
    data0_wr(vals[2]);
    abs_cmd(reg_cmd(RegA0, 1'b1), err);
    expect_cmderr("cmderr after a command written while it was set", err, 3);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after the command written while cmderr was set", v, a0_saved);

    // 3: an exception in the program buffer: still halted, dpc and mcause unchanged.
    gpr_rd(RegDpc, dpc0, err);
    expect_cmderr("dpc read", err, 0);
    gpr_rd(RegMcause, mcause0, err);
    expect_cmderr("mcause read", err, 0);
    progbuf_wr({InsnIllegal, InsnEbreak});
    abs_cmd(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1), err);
    expect_cmderr("illegal instruction in the program buffer", err, 3);
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(st), .blocking(1));
    chk_eq("dmstatus.allhalted after the exception", get_field_val(
           cfg.jtag_dmi_ral.dmstatus.allhalted, st), 1);
    gpr_rd(RegDpc, dpc1, err);
    expect_cmderr("dpc read", err, 0);
    chk_eq("dpc after the exception in debug mode", dpc1, dpc0);
    gpr_rd(RegMcause, mcause1, err);
    expect_cmderr("mcause read", err, 0);
    chk_eq("mcause after the exception in debug mode", mcause1, mcause0);
    data0_wr(vals[3]);
    abs_cmd(reg_cmd(RegA0, 1'b1, 1'b1, 1'b1), err);
    expect_cmderr("a0 write, then an illegal instruction", err, 3);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 written before the program buffer's exception", v, vals[3]);

    // postexec.
    progbuf_wr({InsnAddiA0, InsnEbreak});
    data0_wr(vals[4]);
    abs_cmd(reg_cmd(RegA0, 1'b1, 1'b1, 1'b1), err);
    expect_cmderr("a0 write with postexec", err, 0);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after postexec addi", v, vals[4] + 1);

    // autoexecdata: each data0 read returns the last result and runs the command again.
    abs_cmd(reg_cmd(RegA0, 1'b0, 1'b1, 1'b1), err);
    expect_cmderr("a0 read with postexec", err, 0);
    abstractauto_wr(1'b1, 1'b0);
    for (int unsigned i = 0; i < n_auto; i++) begin
      data0_rd(v);
      chk_eq($sformatf("data0 read %0d with autoexecdata", i), v, vals[4] + 1 + i);
      abs_wait(err);
      expect_cmderr($sformatf("autoexecdata run %0d", i), err, 0);
    end
    abstractauto_wr(1'b0, 1'b0);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after the autoexecdata runs", v, vals[4] + 2 + n_auto);

    // autoexecprogbuf: each progbuf0 write runs the command (the program buffer) again.
    data0_wr(vals[5]);
    abs_cmd(reg_cmd(RegA0, 1'b1), err);
    expect_cmderr("a0 write", err, 0);
    abs_cmd(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1), err);
    expect_cmderr("program buffer run", err, 0);
    abstractauto_wr(1'b0, 1'b1);
    for (int unsigned i = 0; i < n_auto; i++) begin
      progbuf_wr({InsnAddiA0});
      abs_wait(err);
      expect_cmderr($sformatf("autoexecprogbuf run %0d", i), err, 0);
    end
    abstractauto_wr(1'b0, 1'b0);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after the autoexecprogbuf runs", v, vals[5] + 1 + n_auto);

    // 1: command and data0 written while a long program buffer loop runs.
    progbuf_wr({InsnLuiT1, InsnAddiT1M1, InsnBnezT1, InsnEbreak});
    csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(reg_cmd(16'h0, 1'b0, 1'b0, 1'b1)),
           .blocking(1), .predict(1));
    data0_wr(vals[0]);
    csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(reg_cmd(RegA0, 1'b1)), .blocking(1),
           .predict(1));
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(st), .blocking(1));
    chk_true("the program buffer loop still ran at the second access (stimulus)",
             get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, st));
    abs_wait(err);
    expect_cmderr("command and data0 written while busy", err, 1);
    gpr_rd(RegA0, v, err);
    expect_cmderr("a0 read", err, 0);
    chk_eq("a0 after the command refused while busy", v, vals[5] + 1 + n_auto);

    // a0 and t1 back, then resume: every register and tag as at the halt.
    gpr_wr(RegA0, a0_saved, err);
    expect_cmderr("a0 restored", err, 0);
    gpr_wr(RegT1, t1_saved, err);
    expect_cmderr("t1 restored", err, 0);
    resume_checked("abstract errors: resume");
    end_cmd(1'b1, ok);
    finish(32'h11);
  endtask

endclass : chip_eg2_rv_dm_abstract_errors_vseq
