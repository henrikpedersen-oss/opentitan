// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_halt_corners (EG2 spec REQ_CDM_03, REQ_CDM_08; "Debug Controller" / "Corner
// cases"; sw/device/tests/eg2_debug/eg2_rv_dm_halt_corners.c), both modes.
//
// A halt request timed against each kernel of eg2_dm_corners.h: a divide (DIV), a load waiting
// on RRAM (LOAD), a store waiting on the always-on CDC (STORE), the two beats of a CLC/CSC (CAP;
// LW/SW in RISC-V mode), wfi (WFI), trap entry (TRAP: ecall), rv_plic's software interrupt
// arriving in the same cycle (IRQ) and the watchdog NMI (NMI); in CHERIoT mode also the start-up
// code's CHERIOT_LOCK store, reached again after an ndmreset (lock_tries).
//
// Timing: the bench does not force anything. It records when the kernel's timed instruction
// becomes the next to retire (RVFI), or when irq_software_i, irq_nm_i or core_sleep_o rise
// (ibex_top ports), estimates the kernel's period, and launches the haltreq write ahead of a
// coming event by the request's measured latency (JTAG write to debug_req_i), plus a drawn
// jitter of -JitterMax..+JitterMax core cycles. At the cycle debug_req_i rises it records
// whether the halt hit the state (the timed instruction in flight; for CAP, between the two
// beats: one data response since the last retirement; for wfi, core_sleep_o; for IRQ and NMI, the
// interrupt rising within two cycles). The hits per kernel are reported (`EG2 DM halt ... hit`);
// with +eg2_dm_require_hit=1 a kernel that was never hit fails the run.
//
// Every halt is checked (chip_eg2_rv_dm_corners_base_vseq: halt_checked's checks): dcsr.cause = 3
// (haltreq), dcsr.prv = the privilege before the halt, dpc = the next instruction by RVFI (or the
// trap vector when a trap or interrupt came first), every GPR read through abstract commands = its
// RVFI value; after resume the first instruction is at dpc, in the same privilege, and every
// register and its capability metadata (tag) are unchanged. The software checks each kernel ran
// undisturbed (checksum, exceptions and interrupts counted once: no interrupt lost); the
// scoreboard's instruction check runs across every halt.
//
// pwrmgr low-power sleep is not halted into: pwrmgr's documentation ("Debug") states that a debug
// session breaks when the system enters low power. The wfi kernel is the sleep case.
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no CHERIoT
// input, so a halt in CHERIoT mode runs the base debug ROM and the core is never reported halted
// (as chip_rv_dm_control_status_cheriot). The lock-store halts that land before the store (base
// mode) pass. Written to REQ_CDM_03 and REQ_CDM_08.
class chip_eg2_rv_dm_halt_corners_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_halt_corners_vseq)
  `uvm_object_new

  localparam int unsigned NumKernels = 8;  // DIV .. NMI (kinds 1..8)
  localparam int JitterMax = 6;

  rand bit [7:0] kind_order[NumKernels];
  rand int unsigned n_tries;
  rand int jitter[16];
  rand int unsigned lock_tries;
  rand int unsigned wfi_ticks;
  rand int unsigned nmi_bark;

  constraint order_c {
    foreach (kind_order[i]) kind_order[i] inside {[1:NumKernels]};
    unique {kind_order};
  }
  constraint knobs_c {
    n_tries inside {[2:4]};
    foreach (jitter[i]) jitter[i] inside {[-JitterMax:JitterMax]};
    lock_tries inside {[1:2]};
    wfi_ticks inside {[10:40]};
    nmi_bark inside {[2:8]};
  }

  bit require_hit;
  protected realtime lat_est;
  protected int unsigned hits[int unsigned], tries[int unsigned], sub_hits[int unsigned];

  virtual function void write_params();
    super.write_params();
    set_param("wfi_ticks", wfi_ticks);
    set_param("nmi_bark", nmi_bark);
  endfunction

  protected function string kind_name(int unsigned kind);
    case (kind)
      1: return "div";
      2: return "load";
      3: return "store";
      4: return "cap";
      5: return "wfi";
      6: return "trap";
      7: return "irq";
      8: return "nmi";
      default: return "lock";
    endcase
  endfunction

  // Kernels whose event is a port's rising edge (the others: the timed instruction becoming the
  // next to retire, by RVFI).
  protected function string event_port(int unsigned kind);
    case (kind)
      5: return "core_sleep_o";
      7: return "irq_software_i";
      8: return "irq_nm_i";
      default: return "";
    endcase
  endfunction

  // Waits for the next event of `kind` after now (at most `timeout`); returns its time, 0 if none.
  protected task next_event(int unsigned kind, output realtime t, input realtime timeout = 2ms);
    string port = event_port(kind);
    realtime t_end = $realtime + timeout;
    t = 0;
    if (port == "") begin
      int unsigned n0 = watch_times.size();
      while (watch_times.size() == n0 && $realtime < t_end) @(rvfi.monitor_cb);
      if (watch_times.size() > n0) t = watch_times[$];
    end else begin
      bit prev = probe(port);
      while ($realtime < t_end) begin
        bit cur;
        @(rvfi.monitor_cb);
        cur = probe(port);
        if (cur && !prev) begin
          t = $realtime;
          return;
        end
        prev = cur;
      end
    end
  endtask

  // The kernel's period: the median of the gaps between `n` events.
  protected task measure_period(int unsigned kind, output realtime period);
    realtime ev[$], gaps[$];
    period = 0;
    for (int i = 0; i < 6; i++) begin
      realtime t;
      next_event(kind, t);
      if (t == 0) break;
      ev.push_back(t);
    end
    for (int i = 1; i < ev.size(); i++) gaps.push_back(ev[i] - ev[i-1]);
    gaps.sort();
    if (gaps.size() > 0) period = gaps[gaps.size() / 2];
    `uvm_info(`gfn, $sformatf("EG2 DM halt %s: %0d events, period %0t", kind_name(kind),
                              ev.size(), period), UVM_LOW)
  endtask

  // From the launch of the haltreq write until debug_req_i rises (and two cycles more): the hit
  // classification. Returns the time debug_req_i rose.
  protected task watch_launch(int unsigned kind, bit [31:0] target, output realtime t_dbg,
                              output bit hit, output bit sub);
    string port = event_port(kind);
    int unsigned last_ret = retired, rv_since = 0;
    realtime t_rise = 0;
    bit prev = port == "" ? 1'b0 : probe(port);
    bit seen = 1'b0;
    int unsigned after = 0;
    hit = 1'b0;
    sub = 1'b0;
    t_dbg = 0;
    for (int unsigned i = 0; i < 40_000 && after < 3; i++) begin
      bit cur;
      @(rvfi.monitor_cb);
      if (retired != last_ret) begin
        last_ret = retired;
        rv_since = 0;
      end
      if (probe("data_rvalid_i")) rv_since++;
      if (port != "") begin
        cur = probe(port);
        if (cur && !prev) t_rise = $realtime;
        prev = cur;
      end
      if (seen) begin
        after++;
        continue;
      end
      if (!probe("debug_req_i")) continue;
      seen = 1'b1;
      t_dbg = $realtime;
      case (kind)
        1, 2, 3: begin
          hit = nd_pc_wdata == target;
          sub = hit && rv_since == 0;   // LOAD/STORE: no response yet (waiting on the bus)
        end
        4: begin
          hit = nd_pc_wdata == target;
          sub = hit && rv_since == 1;   // between the two beats
        end
        5: hit = probe("core_sleep_o");
        6: hit = nd_pc_wdata == target || (nd_trap && nd_pc_rdata == target);
        default: ;
      endcase
    end
    if (!seen) `uvm_fatal(`gfn, "EG2 DM: haltreq did not raise the core's debug_req_i")
    if (kind inside {7, 8}) begin
      hit = t_rise != 0 && (t_rise > t_dbg ? t_rise - t_dbg : t_dbg - t_rise) <= 2 * cpu_period;
    end
  endtask

  // One halt aimed at the next event of `kind` (period `period`), checked, and resumed.
  protected task aimed_halt(int unsigned kind, bit [31:0] target, realtime period, int jit);
    realtime t_e, t_launch, t_dbg, t0;
    bit hit, sub;
    int unsigned n0 = debug_entries;
    string what = $sformatf("halt %s try %0d", kind_name(kind), tries[kind]);
    next_event(kind, t_e);
    if (t_e != 0 && period > 0) begin
      int unsigned k = 1;
      while (k * period < lat_est + 4 * cpu_period) k++;
      t_launch = t_e + k * period - lat_est + jit * cpu_period;
      if (t_launch > $realtime) #(t_launch - $realtime);
    end
    t0 = $realtime;
    fork
      cfg.debugger.set_haltreq(1);
      watch_launch(kind, target, t_dbg, hit, sub);
    join
    lat_est = lat_est == 0 ? t_dbg - t0 : (lat_est + (t_dbg - t0)) / 2;
    tries[kind]++;
    if (hit) hits[kind]++;
    if (sub) sub_hits[kind]++;
    `uvm_info(`gfn, $sformatf("EG2 DM %s: request latency %0t, hit %0d, sub-state %0d", what,
                              t_dbg - t0, hit, sub), UVM_LOW)
    cfg.debugger.wait_cpu_halted();
    cfg.debugger.set_haltreq(0);
    wait_debug_entry(n0, what);
    // Every GPR at the first halt of a kernel, six drawn ones at the others.
    gpr_sample = tries[kind] == 1 ? 0 : 6;
    check_entry(what, RvDebugCauseHaltReq, 1'b1);
    resume_checked(what);
  endtask

  protected task run_kind(int unsigned kind);
    bit ok;
    realtime period;
    send_cmd(kind, 0, ok);
    if (!ok) return;
    watch_times.delete();
    watch_pc = event_port(kind) == "" ? start_target : 0;
    measure_period(kind, period);
    chk_true($sformatf("halt %s: the kernel's events seen", kind_name(kind)), period > 0);
    for (int unsigned t = 0; t < n_tries; t++) begin
      aimed_halt(kind, start_target, period, jitter[t % 16]);
    end
    watch_pc = 0;
    end_cmd(1'b1, ok);
  endtask

  // CHERIoT mode: halts aimed at the start-up code's CHERIOT_LOCK store, each after an ndmreset
  // (the store's time after reset release measured at the previous boot).
  protected task lock_halts();
    for (int unsigned i = 0; i < lock_tries; i++) begin
      int unsigned b0 = boots, r0 = readies, c0 = core_resets, n0;
      realtime t_lock, t_launch, t_dbg, t0;
      bit hit, sub, ok;
      string what = $sformatf("halt lock try %0d", i);
      if (!cfg_store_after_reset.exists(lock_addr)) begin
        `uvm_error(`gfn, $sformatf("EG2 DM: no store to CHERIOT_LOCK (0x%08x) seen at boot",
                                   lock_addr))
        return;
      end
      t_lock = cfg_store_after_reset[lock_addr];
      cfg.debugger.set_ndmreset(1);
      cfg.clk_rst_vif.wait_clks(20);
      cfg.debugger.set_ndmreset(0);
      wait (core_resets > c0);
      n0 = debug_entries;
      t_launch = reset_release_time + t_lock - lat_est + jitter[i] * 4 * cpu_period;
      if (t_launch > $realtime) #(t_launch - $realtime);
      t0 = $realtime;
      fork
        cfg.debugger.set_haltreq(1);
        wait_debug_req(t_dbg);
      join
      hit = !cfg_store_after_reset.exists(lock_addr);
      tries[9]++;
      if (hit) hits[9]++;
      `uvm_info(`gfn, $sformatf("EG2 DM %s: debug request %0t after reset, the store at %0t (%s)",
                                what, t_dbg - reset_release_time, t_lock,
                                hit ? "before the store: base mode" : "after it"), UVM_LOW)
      cfg.debugger.wait_cpu_halted();
      cfg.debugger.set_haltreq(0);
      wait_debug_entry(n0, what);
      check_entry(what, RvDebugCauseHaltReq, 1'b1);
      resume_checked(what);
      wait_boot_ready(b0, r0, ok);
      if (!ok) return;
      chk_eq({what, ": reset reason NDM (rstmgr RESET_INFO bit 7) after ndmreset"},
             boot_info, 32'h80);
    end
  endtask

  virtual task body();
    bit ok, cheriot;
    super.body();
    void'($value$plusargs("eg2_dm_require_hit=%0b", require_hit));
    corners_ready(ok);
    if (!ok) return;
    cheriot = rvfi.cheriot_ena === 1'b1;
    `uvm_info(`gfn, $sformatf("EG2 DM halt corners: %s mode, kernels %p, %0d tries each",
                              cheriot ? "CHERIoT" : "RISC-V", kind_order, n_tries), UVM_LOW)
    foreach (kind_order[i]) begin
      run_kind(kind_order[i]);
      if (sw_ended()) return;
    end
    if (cheriot) lock_halts();
    foreach (tries[k]) begin
      `uvm_info(`gfn, $sformatf("EG2 DM halt %s: hit %0d of %0d (sub-state %0d)", kind_name(k),
                                hits.exists(k) ? hits[k] : 0, tries[k],
                                sub_hits.exists(k) ? sub_hits[k] : 0), UVM_LOW)
      if (require_hit) begin
        chk_true($sformatf("halt %s: hit at least once", kind_name(k)),
                 hits.exists(k) && hits[k] > 0);
      end
    end
    finish(32'h10);
  endtask

endclass : chip_eg2_rv_dm_halt_corners_vseq
