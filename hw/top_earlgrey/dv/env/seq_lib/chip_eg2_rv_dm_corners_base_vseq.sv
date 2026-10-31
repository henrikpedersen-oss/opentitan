// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of the debug module's corner tests (EG2 spec "Debug Controller" / "Corner cases",
// chip_eg2_rv_dm_*): software in sw/device/tests/eg2_debug (eg2_dm_corners.h states the
// contract). Runs with +use_jtag_dmi=1 in the RVFI build (en_eg2_cosim).
//
// What it gives the tests:
//  - The software's lines (`EG2 DEBUG boot/ready/start/end/...`), recorded by a monitor as they
//    are printed (line_monitor), and the command protocol: send_cmd() writes `cmd` over SBA and
//    waits for the start line; stop_cmd() writes `stop` and waits for the end line, whose verdict
//    must be 1.
//  - The core's RVFI (eg2_rvfi_if, the scoreboard's source), as a shadow register file with the
//    capability metadata: snapshot at debug entry, compared again at the first instruction after
//    resume. halt_checked() / check_entry() / resume_checked() check a halt against the RISC-V
//    Debug Specification 0.13.2 (4.8.1 dcsr, 4.8.2 dpc) and Ibex's documentation (cs_registers.rst
//    dcsr and dpc: "dpc is updated with the address of the next instruction that would be
//    executed"; "When resuming, the PC is set to the address stored in dpc"):
//      dcsr.cause, dcsr.xdebugver = 4, dcsr.prv = the privilege of the last instruction before the
//      halt; dpc = the next instruction (the last retired instruction's pc_wdata, or the trap
//      vector when a trap or interrupt was taken in between); every GPR read through abstract
//      commands = the RVFI value at entry; after resume, the first instruction at dpc in the same
//      privilege, and every register's value and capability metadata (tag included) as at entry.
//  - Core probes: ibex_top's ports (debug_req_i, data_req_o, data_gnt_i, data_rvalid_i,
//    core_sleep_o, irq_software_i, irq_nm_i), read with uvm_hdl_read, for timing a request against
//    a pipeline state; nothing is forced.
//  - Abstract command and SBA helpers on the raw DMI registers.
//  - The bench's must-fail knob +eg2_dm_bench_fail=1: the first primary check (dcsr.cause at a
//    halt, cmderr, sbaddress0) has its expected value's bit 0 flipped, so the run must fail with
//    `EG2 DM <what>: got ...`.
//    TODO(inject-registry): announce it once eg2_inject_registry exists.
class chip_eg2_rv_dm_corners_base_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_corners_base_vseq)

  // Hierarchy of the probes (tb.sv's dut; earlgrey_pd_main's instances).
  localparam string PdHier = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string CoreHier = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_core";
  localparam string SbaReqPath = "tb.dut.top_earlgrey.earlgrey_pd_main.main_tl_rv_dm__sba_req";
  localparam string SbaRspPath = "tb.dut.top_earlgrey.earlgrey_pd_main.main_tl_rv_dm__sba_rsp";

  // RV32I instructions for the program buffer (the same encodings in CHERIoT mode).
  localparam bit [31:0] InsnEbreak = 32'h0010_0073;
  localparam bit [31:0] InsnIllegal = 32'h0000_0000;
  localparam bit [31:0] InsnAddiA0 = 32'h0015_0513;   // addi a0, a0, 1
  localparam bit [31:0] InsnLuiT1 = 32'h0000_1337;    // lui t1, 1 (4096)
  localparam bit [31:0] InsnAddiT1M1 = 32'hfff3_0313; // addi t1, t1, -1
  localparam bit [31:0] InsnBnezT1 = 32'hfe03_1ee3;   // bnez t1, -4
  localparam bit [15:0] RegA0 = 16'h100a;
  localparam bit [15:0] RegT1 = 16'h1006;
  localparam bit [15:0] RegDcsr = 16'h07b0;
  localparam bit [15:0] RegDpc = 16'h07b1;

  // From the software's lines.
  bit [31:0] cmd_addr, stop_addr, vec_addr, lock_addr;
  int unsigned boots, readies, starts, ends, armed, late_pending, late_enabled, caps, revbms;
  bit [31:0] boot_info, start_seq, start_kind, start_target, end_seq, end_ok;
  bit [31:0] cap_slot_addr, cap_base_addr, revbm_off, revbm_word;
  realtime start_time;
  int unsigned cmd_seq;
  protected int unsigned ends_at_send;

  // The bench's must-fail knob.
  bit bench_fail;
  protected bit bench_fail_used;

  // The RVFI shadow.
  typedef struct packed {
    bit        valid;
    bit [63:0] cap;
    bit [31:0] data;
  } shadow_reg_t;
  eg2_rvfi_vif rvfi;
  shadow_reg_t shadow[32], snap[32], post_snap[32];
  bit in_debug;
  int unsigned debug_entries, resumes, retired, nd_retired;
  // The last instruction retired outside debug mode, and its copy at debug entry.
  bit nd_valid, nd_trap, nd_intr;
  bit [31:0] nd_pc_rdata, nd_pc_wdata;
  bit [1:0] nd_mode;
  realtime nd_time;
  bit entry_valid, entry_trap, entry_intr, entry_cheriot;
  bit [31:0] entry_pc_rdata, entry_pc_wdata;
  bit [1:0] entry_mode;
  realtime entry_time;
  // The first instruction retired outside debug mode after a resume.
  bit [31:0] post_pc;
  bit post_intr;
  bit [1:0] post_mode;
  // Retirements whose pc_wdata is watch_pc (the next instruction is the watched one).
  bit [31:0] watch_pc;
  realtime watch_times[$];
  // Stores to rv_core_ibex.cfg since the last reset release: address -> time after it.
  realtime cfg_store_after_reset[bit [31:0]];
  realtime reset_release_time;
  int unsigned core_resets;
  realtime cpu_period = 10ns;
  bit [31:0] last_dpc;

  function new(string name = "");
    super.new(name);
    void'($value$plusargs("eg2_dm_bench_fail=%0b", bench_fail));
  endfunction

  // ---------------------------------------------------------------- checks

  protected function bit [63:0] expect_val(bit [63:0] v);
    if (bench_fail && !bench_fail_used) begin
      bench_fail_used = 1'b1;
      // TODO(inject-registry): announce this corrupted expectation.
      `uvm_info(`gfn, "EG2 DM bench_fail: the next check's expected value has bit 0 flipped",
                UVM_LOW)
      return v ^ 64'h1;
    end
    return v;
  endfunction

  // `primary`: a check of the documents' behaviour, the one +eg2_dm_bench_fail corrupts.
  protected function void chk_eq(string what, bit [63:0] got, bit [63:0] exp, bit primary = 1'b0);
    bit [63:0] e = primary ? expect_val(exp) : exp;
    if (got !== e) begin
      `uvm_error(`gfn, $sformatf("EG2 DM %s: got 0x%0h, expected 0x%0h", what, got, e))
    end else begin
      `uvm_info(`gfn, $sformatf("EG2 DM %s: 0x%0h", what, got), UVM_MEDIUM)
    end
  endfunction

  protected function void chk_true(string what, bit cond);
    if (!cond) `uvm_error(`gfn, $sformatf("EG2 DM %s: false", what))
  endfunction

  protected function bit sw_ended();
    return cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusPassed, SwTestStatusFailed};
  endfunction

  // ---------------------------------------------------------------- the software's lines

  protected task line_monitor();
    forever begin
      string s;
      @(cfg.sw_logger_vif.printed_log_event);
      s = string'(cfg.sw_logger_vif.printed_log);
      if (s.len() < 10 || s.substr(0, 9) != "EG2 DEBUG ") continue;
      if ($sscanf(s, "EG2 DEBUG boot 0x%h", boot_info) == 1) begin
        boots++;
      end else if ($sscanf(s, "EG2 DEBUG ready 0x%h 0x%h 0x%h 0x%h 0x%h 0x%h", done_addr,
                           scratch_addr, cmd_addr, stop_addr, vec_addr, lock_addr) == 6) begin
        readies++;
        // The words the bench writes over SBA while the software polls them (done, command, stop):
        // an SBA write reaches the model only when its JTAG access returns, after the software may
        // have read the new value, so the model takes the chip's value for them (bench mailboxes).
        eg2_iss_wrapper::backdoor_mailbox(done_addr, 4);
        eg2_iss_wrapper::backdoor_mailbox(cmd_addr, 4);
        eg2_iss_wrapper::backdoor_mailbox(stop_addr, 4);
      end else if ($sscanf(s, "EG2 DEBUG start 0x%h 0x%h 0x%h", start_seq, start_kind,
                           start_target) == 3) begin
        start_time = $realtime;
        starts++;
      end else if ($sscanf(s, "EG2 DEBUG end 0x%h 0x%h", end_seq, end_ok) == 2) begin
        ends++;
      end else if ($sscanf(s, "EG2 DEBUG cap 0x%h 0x%h", cap_slot_addr, cap_base_addr) == 2) begin
        caps++;
      end else if ($sscanf(s, "EG2 DEBUG revbm 0x%h 0x%h", revbm_off, revbm_word) == 2) begin
        revbms++;
      end else if (s.len() >= 15 && s.substr(0, 14) == "EG2 DEBUG armed") begin
        armed++;
      end else if (s == "EG2 DEBUG late pending") begin
        late_pending++;
      end else if (s == "EG2 DEBUG late enabled") begin
        late_enabled++;
      end
    end
  endtask

  // The line counters wait_lines() watches.
  typedef enum int unsigned {
    LineBoot, LineReady, LineStart, LineEnd, LineArmed, LineLatePending, LineLateEnabled,
    LineCap, LineRevbm
  } line_e;

  protected function int unsigned line_count(line_e kind);
    case (kind)
      LineBoot: return boots;
      LineReady: return readies;
      LineStart: return starts;
      LineEnd: return ends;
      LineArmed: return armed;
      LineLatePending: return late_pending;
      LineLateEnabled: return late_enabled;
      LineCap: return caps;
      default: return revbms;
    endcase
  endfunction

  // Waits until the counter of `kind` exceeds `n0`; 0 if the software ended or the wait timed out
  // (an error unless the software ended: a failing `_inject_fail` build ends on its own verdict).
  protected task wait_lines(string what, line_e kind, int unsigned n0, output bit ok,
                            input int unsigned timeout_us = 20_000);
    realtime t_end = $realtime + timeout_us * 1us;
    while (line_count(kind) <= n0 && !sw_ended() && $realtime < t_end) begin
      #(1us);
    end
    ok = line_count(kind) > n0;
    if (!ok && !sw_ended()) begin
      `uvm_error(`gfn, $sformatf("EG2 DM: no '%s' line within %0d us", what, timeout_us))
    end
  endtask

  // Waits for the boot and ready lines after `boots0`/`readies0` lines (the first boot: 0, 0).
  protected task wait_boot_ready(int unsigned boots0, int unsigned readies0, output bit ok);
    wait_lines("EG2 DEBUG boot", LineBoot, boots0, ok, 200_000);
    if (!ok) return;
    wait_lines("EG2 DEBUG ready", LineReady, readies0, ok);
    if (ok) begin
      `uvm_info(`gfn, $sformatf({"EG2 DM software: reset info 0x%08x, done 0x%08x, scratch ",
                                 "0x%08x, cmd 0x%08x, stop 0x%08x, vector 0x%08x, lock 0x%08x"},
                                boot_info, done_addr, scratch_addr, cmd_addr, stop_addr, vec_addr,
                                lock_addr), UVM_LOW)
    end
  endtask

  // The first boot: the lines, the drawn delay of the base, then the debug module activated.
  protected task corners_ready(output bit ok);
    int unsigned weights[3] = '{w_when_at_ready, w_when_short, w_when_long};
    wait_boot_ready(0, 0, ok);
    if (!ok) return;
    if (when_pick >= weights[0] + weights[1]) begin
      #(delay_long_ns * 1ns);
    end else if (when_pick >= weights[0]) begin
      #(delay_short_ns * 1ns);
    end
    cfg.debugger.set_dmactive(1);
  endtask

  // A command: seq << 16 | arg << 8 | kind into `cmd` over SBA; waits for its start line.
  protected task send_cmd(bit [7:0] kind, bit [7:0] arg, output bit ok);
    sba_access_err_e st;
    int unsigned s0 = starts;
    cmd_seq++;
    ends_at_send = ends;
    sba_wr32(cmd_addr, {cmd_seq[15:0], arg, kind}, st);
    chk_eq("SBA write of the command word", st, SbaErrNone);
    wait_lines("EG2 DEBUG start", LineStart, s0, ok);
    if (ok) chk_eq("start line's sequence number", start_seq, cmd_seq);
  endtask

  // Waits for the command's end line (after `stop` if `do_stop`); its verdict must be 1.
  protected task end_cmd(bit do_stop, output bit ok);
    sba_access_err_e st;
    if (do_stop) begin
      sba_wr32(stop_addr, 1, st);
      chk_eq("SBA write of the stop word", st, SbaErrNone);
    end
    wait_lines("EG2 DEBUG end", LineEnd, ends_at_send, ok, 100_000);
    if (!ok) return;
    chk_eq("end line's sequence number", end_seq, cmd_seq);
    chk_eq("the software's checks of the command", end_ok, 1);
  endtask

  // ---------------------------------------------------------------- RVFI

  protected task rvfi_monitor();
    bit was_reset = 1'b1;
    forever begin
      @(rvfi.monitor_cb);
      if (rvfi.monitor_cb.rst_n !== 1'b1) begin
        foreach (shadow[i]) shadow[i].valid = 1'b0;
        in_debug = 1'b0;
        nd_valid = 1'b0;
        was_reset = 1'b1;
        continue;
      end
      if (was_reset) begin
        was_reset = 1'b0;
        reset_release_time = $realtime;
        cfg_store_after_reset.delete();
        core_resets++;
      end
      if (rvfi.monitor_cb.valid === 1'b1) begin
        bit dbg = rvfi.monitor_cb.ext_debug_mode === 1'b1;
        bit [4:0] rd = rvfi.monitor_cb.rd_addr;
        bit [31:0] maddr = rvfi.monitor_cb.mem_addr;
        retired++;
        if (dbg && !in_debug) begin
          in_debug = 1'b1;
          snap = shadow;
          entry_valid = nd_valid;
          entry_trap = nd_trap;
          entry_intr = nd_intr;
          entry_pc_rdata = nd_pc_rdata;
          entry_pc_wdata = nd_pc_wdata;
          entry_mode = nd_mode;
          entry_time = $realtime;
          entry_cheriot = rvfi.monitor_cb.cheriot_ena === 1'b1;
          debug_entries++;
        end else if (!dbg && in_debug) begin
          in_debug = 1'b0;
          post_snap = shadow;
          post_pc = rvfi.monitor_cb.pc_rdata;
          post_intr = rvfi.monitor_cb.intr === 1'b1;
          post_mode = rvfi.monitor_cb.mode;
          resumes++;
        end
        if (!dbg) begin
          nd_valid = 1'b1;
          nd_trap = rvfi.monitor_cb.trap === 1'b1;
          nd_intr = rvfi.monitor_cb.intr === 1'b1;
          nd_pc_rdata = rvfi.monitor_cb.pc_rdata;
          nd_pc_wdata = rvfi.monitor_cb.pc_wdata;
          nd_mode = rvfi.monitor_cb.mode;
          nd_time = $realtime;
          nd_retired++;
          if (watch_pc != 0 && nd_pc_wdata == watch_pc) watch_times.push_back($realtime);
          if (rvfi.monitor_cb.mem_wmask != 0 &&
              maddr >= top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR &&
              maddr < top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_BASE_ADDR +
                      top_earlgrey_pkg::TOP_EARLGREY_RV_CORE_IBEX_CFG_SIZE_BYTES &&
              !cfg_store_after_reset.exists(maddr)) begin
            cfg_store_after_reset[maddr] = $realtime - reset_release_time;
          end
        end
        if (rd != 0 && rvfi.monitor_cb.trap !== 1'b1) begin
          shadow[rd].valid = 1'b1;
          shadow[rd].data = rvfi.monitor_cb.rd_wdata;
          shadow[rd].cap = 64'(rvfi.monitor_cb.rd_wcap);
        end
      end
    end
  endtask

  protected task measure_cpu_period();
    realtime t0;
    @(rvfi.monitor_cb);
    t0 = $realtime;
    @(rvfi.monitor_cb);
    cpu_period = $realtime - t0;
    `uvm_info(`gfn, $sformatf("EG2 DM: core clock period %0t", cpu_period), UVM_LOW)
  endtask

  // ---------------------------------------------------------------- probes

  protected function bit probe(string sig);
    uvm_hdl_data_t v;
    string path = {CoreHier, ".", sig};
    if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
    return v[0];
  endfunction

  protected function bit sba_a_valid();
    uvm_hdl_data_t v;
    tlul_pkg::tl_h2d_t h;
    string path = SbaReqPath;
    if (!uvm_hdl_read(path, v)) `uvm_fatal(`gfn, {"cannot read ", path})
    h = tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
    return h.a_valid;
  endfunction

  // Waits until debug_req_i is 1 (at most `max_cycles` core cycles); returns the time.
  protected task wait_debug_req(output realtime t, input int unsigned max_cycles = 20_000);
    for (int unsigned i = 0; i < max_cycles; i++) begin
      if (probe("debug_req_i")) begin
        t = $realtime;
        return;
      end
      @(rvfi.monitor_cb);
    end
    `uvm_fatal(`gfn, "EG2 DM: haltreq did not raise the core's debug_req_i")
  endtask

  // ---------------------------------------------------------------- halt and resume

  protected function bit in_vector(bit [31:0] pc);
    return pc >= vec_addr && pc < vec_addr + 32'h80;
  endfunction

  protected task wait_debug_entry(int unsigned n0, string what);
    fork : isolation_fork
      begin
        fork
          wait (debug_entries > n0);
          #(500us);
        join_any
        disable fork;
      end
    join
    chk_true({what, ": RVFI shows the debug entry"}, debug_entries > n0);
  endtask

  // The core halted by haltreq; the entry checked.
  protected task halt_checked(string what, bit check_gprs = 1'b1);
    int unsigned n0 = debug_entries;
    cfg.debugger.set_haltreq(1);
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_haltreq(0);
    wait_debug_entry(n0, what);
    check_entry(what, RvDebugCauseHaltReq, check_gprs);
  endtask

  // dcsr, dpc and the GPRs against the RVFI at entry. `exp_dpc_reset`: the core halted at its
  // first instruction after reset, whose address is the reset vector.
  protected task check_entry(string what, rv_core_csr_dcsr_cause_e exp_cause, bit check_gprs,
                             bit [31:0] exp_dpc_reset = '0);
    rv_core_csr_dcsr_t dcsr;
    logic [31:0] dpc;
    cfg.debugger.read_dcsr(dcsr);
    chk_eq({what, ": dcsr.cause"}, dcsr.cause, exp_cause, 1'b1);
    chk_eq({what, ": dcsr.xdebugver"}, dcsr.xdebugver, 4);
    cfg.debugger.read_pc(dpc);
    last_dpc = dpc;
    if (exp_dpc_reset != 0) begin
      chk_true({what, ": no instruction retired before the halt after reset"}, !entry_valid);
      chk_eq({what, ": dpc is the reset vector"}, dpc, exp_dpc_reset);
      chk_eq({what, ": dcsr.prv (M after reset)"}, dcsr.prv, 3);
    end else if (entry_valid) begin
      // A trap or an interrupt taken between the last retirement and the halt: dpc is its
      // vector entry (the kernels' vector; resume_checked() then sees the handler run).
      bit ok = dpc == entry_pc_wdata || (in_vector(dpc) && dpc[1:0] == 0);
      chk_eq({what, ": dcsr.prv = the privilege before the halt"}, dcsr.prv, entry_mode);
      if (!ok) begin
        `uvm_error(`gfn, $sformatf({"EG2 DM %s: dpc 0x%08x is not the next instruction after ",
                                    "0x%08x (0x%08x) nor a trap vector entry"}, what, dpc,
                                   entry_pc_rdata, entry_pc_wdata))
      end
    end
    if (check_gprs) check_gprs_at_entry(what);
  endtask

  // The GPRs read through abstract commands at a halt: all of them, or `gpr_sample` drawn ones
  // (a run with many halts keeps its JTAG time down).
  int unsigned gpr_sample;

  protected task check_gprs_at_entry(string what);
    int unsigned nregs = entry_cheriot ? 16 : 32;
    int unsigned regs[$];
    for (int unsigned r = 1; r < nregs; r++) regs.push_back(r);
    if (gpr_sample != 0 && gpr_sample < regs.size()) begin
      regs.shuffle();
      regs = regs[0:gpr_sample-1];
    end
    foreach (regs[i]) begin
      int unsigned r = regs[i];
      logic [31:0] q[$];
      abstract_cmd_err_e st;
      cfg.debugger.abstract_cmd_reg_read(.regno(16'h1000 + r), .value_q(q), .status(st));
      chk_eq($sformatf("%s: abstract read of x%0d status", what, r), st, AbstractCmdErrNone);
      if (st == AbstractCmdErrNone && snap[r].valid) begin
        chk_eq($sformatf("%s: x%0d through data0 = its value at the halt", what, r), q[0],
               snap[r].data);
      end
    end
  endtask

  // Resumes; the first instruction outside debug mode is at dpc in the same privilege, and every
  // register (value and capability metadata) is as at the halt.
  protected task resume_checked(string what);
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
    if (resumes <= r0) return;
    if (post_intr && in_vector(post_pc)) begin
      `uvm_info(`gfn, $sformatf("EG2 DM %s: an interrupt was taken at resume (0x%08x)", what,
                                post_pc), UVM_LOW)
    end else begin
      chk_eq({what, ": the first instruction after resume is at dpc"}, post_pc, last_dpc);
    end
    chk_eq({what, ": privilege after resume"}, post_mode, entry_valid ? entry_mode : 2'b11);
    foreach (snap[r]) begin
      if (r == 0 || !snap[r].valid) continue;
      chk_eq($sformatf("%s: x%0d after halt and resume", what, r), post_snap[r].data,
             snap[r].data);
      chk_eq($sformatf("%s: x%0d capability metadata (tag) after halt and resume", what, r),
             post_snap[r].cap, snap[r].cap);
    end
  endtask

  // ---------------------------------------------------------------- abstract commands

  protected function bit [31:0] reg_cmd(bit [15:0] regno, bit write, bit transfer = 1'b1,
                                        bit postexec = 1'b0, bit [2:0] aarsize = 3'd2);
    return {8'h00, 1'b0, aarsize, 1'b0, postexec, transfer, write, regno};
  endfunction

  // Waits until abstractcs.busy is 0 (bounded); returns cmderr.
  protected task abs_wait(output bit [2:0] cmderr, input int unsigned max_polls = 4000);
    uvm_reg_data_t v;
    int unsigned n;
    do begin
      csr_rd(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(v), .blocking(1));
      n++;
    end while (get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, v) && n < max_polls);
    chk_true("abstract command finished (abstractcs.busy 0)",
             !get_field_val(cfg.jtag_dmi_ral.abstractcs.busy, v));
    cmderr = get_field_val(cfg.jtag_dmi_ral.abstractcs.cmderr, v);
  endtask

  protected task abs_cmd(bit [31:0] cmd, output bit [2:0] cmderr);
    csr_wr(.ptr(cfg.jtag_dmi_ral.command), .value(cmd), .blocking(1), .predict(1));
    abs_wait(cmderr);
  endtask

  protected task cmderr_clear();
    uvm_reg_data_t v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.abstractcs.cmderr, 0, 7);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractcs), .value(v), .blocking(1), .predict(0));
  endtask

  protected task data0_wr(bit [31:0] v);
    csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[0]), .value(v), .blocking(1), .predict(1));
  endtask

  protected task data0_rd(output bit [31:0] v);
    uvm_reg_data_t d;
    csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[0]), .value(d), .blocking(1));
    v = d[31:0];
  endtask

  protected task gpr_rd(bit [15:0] regno, output bit [31:0] v, output bit [2:0] cmderr);
    abs_cmd(reg_cmd(regno, 1'b0), cmderr);
    data0_rd(v);
  endtask

  protected task gpr_wr(bit [15:0] regno, bit [31:0] v, output bit [2:0] cmderr);
    data0_wr(v);
    abs_cmd(reg_cmd(regno, 1'b1), cmderr);
  endtask

  protected task progbuf_wr(bit [31:0] words[$]);
    foreach (words[i]) begin
      csr_wr(.ptr(cfg.jtag_dmi_ral.progbuf[i]), .value(words[i]), .blocking(1), .predict(1));
    end
  endtask

  // ---------------------------------------------------------------- SBA

  // sbcs with the given fields (sbaccess, sbautoincrement, sbreadonaddr, sbreadondata) and the
  // sticky errors cleared (sbbusyerror and sberror are write-1-to-clear).
  protected task sbcs_wr(bit [2:0] sbaccess, bit autoinc = 0, bit readonaddr = 0,
                         bit readondata = 0, bit clear_errors = 1);
    uvm_reg_data_t v = 0;
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sbaccess, v, sbaccess);
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sbautoincrement, v, autoinc);
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sbreadonaddr, v, readonaddr);
    v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sbreadondata, v, readondata);
    if (clear_errors) begin
      v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sberror, v, 7);
      v = get_csr_val_with_updated_field(cfg.jtag_dmi_ral.sbcs.sbbusyerror, v, 1);
    end
    csr_wr(.ptr(cfg.jtag_dmi_ral.sbcs), .value(v), .blocking(1), .predict(1));
  endtask

  protected task sbcs_rd(output uvm_reg_data_t v);
    csr_rd(.ptr(cfg.jtag_dmi_ral.sbcs), .value(v), .blocking(1));
  endtask

  // Polls sbcs until sbbusy is 0 (bounded); returns sbcs.
  protected task sba_wait(output uvm_reg_data_t v, input int unsigned max_polls = 4000);
    int unsigned n;
    do begin
      sbcs_rd(v);
      n++;
    end while (get_field_val(cfg.jtag_dmi_ral.sbcs.sbbusy, v) && n < max_polls);
    chk_true("SBA access finished (sbcs.sbbusy 0)", !get_field_val(cfg.jtag_dmi_ral.sbcs.sbbusy,
                                                                    v));
  endtask

  protected task sbaddr_wr(bit [31:0] a);
    csr_wr(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(a), .blocking(1), .predict(1));
  endtask

  protected task sbaddr_rd(output bit [31:0] a);
    uvm_reg_data_t v;
    csr_rd(.ptr(cfg.jtag_dmi_ral.sbaddress0), .value(v), .blocking(1));
    a = v[31:0];
  endtask

  protected task sbdata_wr(bit [31:0] d);
    csr_wr(.ptr(cfg.jtag_dmi_ral.sbdata0), .value(d), .blocking(1), .predict(1));
  endtask

  protected task sbdata_rd(output bit [31:0] d);
    uvm_reg_data_t v;
    csr_rd(.ptr(cfg.jtag_dmi_ral.sbdata0), .value(v), .blocking(1));
    d = v[31:0];
  endtask

  // One SBA access of `size` (0: 8, 1: 16, 2: 32 bits) on the raw registers; returns sberror and
  // the read data (right-aligned in sbdata0, debug specification 3.12.18-19).
  protected task sba_raw(bit write, bit [31:0] addr, bit [2:0] size, bit [31:0] wdata,
                         output bit [2:0] sberror, output bit [31:0] rdata);
    uvm_reg_data_t v;
    sbcs_wr(size, 0, !write, 0);
    sbaddr_wr(addr);
    if (write) sbdata_wr(wdata);
    sba_wait(v);
    sberror = get_field_val(cfg.jtag_dmi_ral.sbcs.sberror, v);
    rdata = '0;
    if (!write) sbdata_rd(rdata);
  endtask

  // ---------------------------------------------------------------- the run

  virtual task body();
    super.body();
    if (!uvm_config_db#(eg2_rvfi_vif)::get(null, "uvm_test_top.env.m_eg2_rvfi_agent",
                                           "eg2_rvfi_vif", rvfi) || rvfi == null) begin
      `uvm_fatal(`gfn, "EG2 DM: the corner tests need the RVFI build (en_eg2_cosim)")
    end
    fork
      line_monitor();
      rvfi_monitor();
    join_none
    measure_cpu_period();
  endtask

endclass : chip_eg2_rv_dm_corners_base_vseq
