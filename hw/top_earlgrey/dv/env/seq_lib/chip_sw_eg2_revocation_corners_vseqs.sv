// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The vseqs of the later revocation corner cases (eg2_top_verification, spec "Revocation", "Corner
// cases"; images in //sw/device/tests/eg2_cheriot, their shared steps in eg2_revoke.h).
//
// chip_sw_eg2_revocation_corner_bench_vseq adds bench steps to chip_sw_eg2_revocation_bench_vseq
// (EG2 BENCH <n> <op> <a> <b>):
//
//   race_toggle_begin <word> <bit>
//                           SBA writes of bitmap word address <word> (the revbm window),
//                           alternately 1 << bit and 0, a random 0-40 cycles apart, until
//                           race_toggle_end. Meanwhile the bench records, for each of the core's
//                           bitmap reads of that word (corerevbm, A then D), bit <bit> of its
//                           response, and classifies where each SBA write lands on the revbm
//                           port: while a read of the word is outstanding, after a core data read
//                           (a CLC in flight) before its bitmap read, or between CLCs (logged).
//   race_toggle_end 0 0     Stops the writes, writes the word 0; answers the reads recorded.
//   race_toggle_end 1 0     Answers the recorded bits (read k in bit k, k < 32).
//   tbre_hold_arm <addr> <release>
//                           When the TBRE presents a read at or above <addr> (cheriot_mem_sys's
//                           tbre port), the bench holds it: a_valid towards the crossbar and
//                           a_ready towards the TBRE are held at 0 (back pressure, the request is
//                           kept). It then writes kEg2RvHoldMagic into kEg2BenchAck[1]. A core
//                           write to <release> (core data port) ends the hold.
//   tbre_hold_end 0 0       Answers the hold's length in cycles (0: never held).
//   tbre_fault_arm <kind> <addr>
//                           One fault on the response to a TBRE request: kind 0 d_error with
//                           correct integrity, 1 one integrity bit flipped, on the read of swept
//                           word <addr> (tbre port); kind 2 and 3 the same on the TBRE's bitmap
//                           lookup of the word covering granule <addr>
//                           (cheriot_mem_sys.tbre_revbm_tl_d2h). The response is the one with the
//                           request's source; it is changed for the cycle it is taken.
//   tbre_fault_end 0 0      Answers 1 if the fault took effect (the TBRE took the changed
//                           response), 0 otherwise.
//   sba_hit_begin <lo> <hi> SBA rewrites, until sba_hit_end, of a random word of an odd granule of
//                           [lo, hi) with the value it holds (read, then write).
//   sba_hit_end 0 0         Answers the rewrites done.
//   cap_halves_count <p> 0  The cap-halves assertion's counts on port p (0 core, 1 TBRE):
//                           {failures[31:20], capability loads checked[19:0]}.
//
// Each test's vseq holds its knobs as rand fields and writes them into the image (write_params);
// `model_gaps` is never written: the chip makes every check.
class chip_sw_eg2_revocation_corner_bench_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_corner_bench_vseq)
  `uvm_object_new

  localparam string Cms = {PdMain, ".u_cheriot_mem_sys"};
  localparam string RevbmPort = "cheriot_mem_sys_revbm_tl_d";
  localparam string TbreRevbmReq = {Cms, ".tbre_revbm_tl_h2d"};
  localparam string TbreRevbmRsp = {Cms, ".tbre_revbm_tl_d2h"};
  localparam bit [31:0] HoldMagic = 32'h484f_4c44;
  localparam int unsigned HoldTimeout = 5_000_000;

  // race_toggle
  protected bit toggle_on, toggle_busy, toggle_mon_on;
  protected bit [31:0] toggle_addr;
  protected int unsigned toggle_bit, toggle_reads, toggle_writes;
  protected bit [31:0] toggle_seen;
  protected int unsigned land_during, land_before, land_between;

  // tbre_hold
  protected bit hold_armed, hold_done;
  protected bit [31:0] hold_lo, hold_rel;
  protected int unsigned hold_cycles;

  // tbre_fault
  protected bit fault_stop, fault_done, fault_hit;
  protected bit [31:0] fault_kind, fault_addr;

  // sba_hit
  protected bit hit_on, hit_busy;
  protected bit [31:0] hit_lo, hit_hi;
  protected int unsigned hits;

  protected function tlul_pkg::tl_h2d_t rd_h2d_path(string path);
    uvm_hdl_data_t v = rd(path);
    return tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
  endfunction

  protected function tlul_pkg::tl_d2h_t rd_d2h_path(string path);
    uvm_hdl_data_t v = rd(path);
    return tlul_pkg::tl_d2h_t'(v[$bits(tlul_pkg::tl_d2h_t)-1:0]);
  endfunction

  // D-channel handshake on `port` in the current cycle.
  protected function bit d_fire(string port, output bit [31:0] data);
    tlul_pkg::tl_h2d_t h = rd_h2d(port);
    tlul_pkg::tl_d2h_t d = rd_d2h(port);
    data = d.d_data;
    return d.d_valid && h.d_ready;
  endfunction

  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    result = 0;
    case (op)
      "race_toggle_begin": begin
        toggle_addr = a;
        toggle_bit = b % 32;
        toggle_reads = 0;
        toggle_seen = 0;
        toggle_writes = 0;
        land_during = 0;
        land_before = 0;
        land_between = 0;
        toggle_on = 1;
        toggle_mon_on = 1;
        fork toggler(); join_none
        fork toggle_monitor(); join_none
      end
      "race_toggle_end": begin
        if (a == 0) begin
          bit [31:0] err;
          toggle_on = 0;
          wait (!toggle_busy);
          sba_access_w(1'b1, toggle_addr, 32, 32'h0, err);
          toggle_mon_on = 0;
          cycle();
          cycle();
          `uvm_info(`gfn, $sformatf({"EG2 bench: race_toggle 0x%08x bit %0d: %0d SBA writes ",
                                     "(%0d while a bitmap read was outstanding, %0d in a CLC ",
                                     "before its bitmap read, %0d between CLCs); %0d bitmap reads, ",
                                     "seen 0x%08x"}, toggle_addr, toggle_bit, toggle_writes,
                                    land_during, land_before, land_between, toggle_reads,
                                    toggle_seen), UVM_LOW)
          result = toggle_reads;
        end else begin
          result = toggle_seen;
        end
      end
      "tbre_hold_arm": begin
        hold_lo = a;
        hold_rel = b;
        hold_cycles = 0;
        hold_done = 0;
        hold_armed = 1;
        fork tbre_hold(); join_none
      end
      "tbre_hold_end": begin
        if (hold_armed) wait (hold_done);
        hold_armed = 0;
        result = hold_cycles;
      end
      "tbre_fault_arm": begin
        fault_kind = a;
        fault_addr = b;
        fault_hit = 0;
        fault_done = 0;
        fault_stop = 0;
        fork tbre_fault(); join_none
      end
      "tbre_fault_end": begin
        fault_stop = 1;
        wait (fault_done);
        result = fault_hit;
      end
      "sba_hit_begin": begin
        hit_lo = a;
        hit_hi = b;
        hits = 0;
        hit_on = 1;
        fork sba_hit(); join_none
      end
      "sba_hit_end": begin
        hit_on = 0;
        wait (!hit_busy);
        `uvm_info(`gfn, $sformatf("EG2 bench: %0d SBA rewrites of [0x%08x, 0x%08x)", hits, hit_lo,
                                  hit_hi), UVM_LOW)
        result = hits;
      end
      "cap_halves_count": begin
        // {failures[31:20], capability loads checked[19:0]} of the core (a 0) or TBRE (a 1) port.
        string chk = a == 0 ? "tb.eg2_ch_core" : "tb.eg2_ch_tbre";
        uvm_hdl_data_t caps = rd({chk, ".caps"});
        uvm_hdl_data_t fails = rd({chk, ".fails"});
        if (rd("tb.eg2_ch_on") == 0) begin
          `uvm_error(`gfn, "EG2 bench: cap_halves_count without +eg2_cap_halves=1")
        end
        result = (32'(fails > 'hfff ? 'hfff : fails) << 20) | 32'(caps > 'hfffff ? 'hfffff : caps);
      end
      default: super.bench_op(op, a, b, result);
    endcase
  endtask

  // --------------------------------------------------------------------------- race_toggle

  protected task toggler();
    bit set = 1'b1;
    while (toggle_on) begin
      bit [31:0] err;
      toggle_busy = 1;
      sba_access_w(1'b1, toggle_addr, 32, set ? (32'd1 << toggle_bit) : 32'd0, err);
      toggle_writes++;
      set = !set;
      toggle_busy = 0;
      repeat ($urandom_range(0, 40)) cycle();
    end
  endtask

  protected task toggle_monitor();
    bit match[$];
    bit clc_in_flight;
    bit [31:0] addr, data;
    bit write;
    int unsigned word = (toggle_addr & 'hfff) >> 2;
    while (toggle_mon_on) begin
      cycle();
      // An SBA (or any) write of the word lands on the revbm window.
      if (a_fire(RevbmPort, addr, write) && write && addr == toggle_addr) begin
        if (match.size() && match[0]) land_during++;
        else if (clc_in_flight) land_before++;
        else land_between++;
      end
      if (a_fire(CoreData, addr, write) && !write) clc_in_flight = 1;
      if (a_fire(CoreRevbm, addr, write)) match.push_back(((addr & 'hfff) >> 2) == word);
      if (d_fire(CoreRevbm, data) && match.size()) begin
        if (match.pop_front()) begin
          if (toggle_reads < 32) toggle_seen[toggle_reads] = data[toggle_bit];
          toggle_reads++;
          clc_in_flight = 0;
        end
      end
    end
  endtask

  // --------------------------------------------------------------------------- tbre_hold

  protected task tbre_hold();
    string req = {PdMain, ".", TbrePort, "_req"};
    string rsp = {PdMain, ".", TbrePort, "_rsp"};
    tlul_pkg::tl_h2d_t h;
    bit [7:0] ack_bytes[8];
    bit [31:0] n, addr;
    bit write, released;
    for (int unsigned i = 0; i < HoldTimeout; i++) begin
      cycle();
      h = rd_h2d(TbrePort);
      if (h.a_valid && h.a_address >= hold_lo && h.a_address < hold_lo + 32'h1_0000) break;
    end
    if (!(h.a_valid && h.a_address >= hold_lo)) begin
      `uvm_error(`gfn, $sformatf("EG2 bench: the TBRE never read at or above 0x%08x", hold_lo))
      hold_done = 1;
      return;
    end
    // Back pressure only: the TBRE keeps its request, the crossbar does not see it.
    if (!(uvm_hdl_force({req, ".a_valid"}, 0))) `uvm_fatal(`gfn, $sformatf("cannot force %s", req))
    if (!(uvm_hdl_force({rsp, ".a_ready"}, 0))) `uvm_fatal(`gfn, $sformatf("cannot force %s", rsp))
    `uvm_info(`gfn, $sformatf("EG2 bench: TBRE held at its read of 0x%08x", h.a_address), UVM_LOW)
    // kEg2BenchAck[1] = HoldMagic, [0] unchanged (the arm step's number).
    sw_symbol_backdoor_read("kEg2BenchAck", ack_bytes);
    for (int i = 0; i < 4; i++) n[8 * i +: 8] = ack_bytes[i];
    ack(n, HoldMagic);
    released = 0;
    for (int unsigned i = 0; i < HoldTimeout && !released; i++) begin
      cycle();
      hold_cycles++;
      if (a_fire(CoreData, addr, write) && write && addr == hold_rel) released = 1;
    end
    if (!(uvm_hdl_release({rsp, ".a_ready"}))) `uvm_fatal(`gfn, $sformatf("cannot release %s", rsp))
    if (!(uvm_hdl_release({req, ".a_valid"}))) `uvm_fatal(`gfn, $sformatf("cannot release %s", req))
    if (!released) begin
      `uvm_error(`gfn, $sformatf("EG2 bench: no core write to 0x%08x within %0d cycles of the hold",
                                 hold_rel, HoldTimeout))
    end
    `uvm_info(`gfn, $sformatf("EG2 bench: TBRE released after %0d cycles", hold_cycles), UVM_LOW)
    hold_done = 1;
  endtask

  // --------------------------------------------------------------------------- sba_hit

  // SBA rewrites of a random word of an odd granule of [hit_lo, hit_hi) with the value it holds.
  protected task sba_hit();
    int unsigned granules = (hit_hi - hit_lo) / 8;
    while (hit_on && granules >= 2) begin
      bit [31:0] addr, v, err;
      hit_busy = 1;
      addr = hit_lo + 8 * (2 * $urandom_range(0, granules / 2 - 1) + 1) + 4 * $urandom_range(0, 1);
      sba_read32(addr, v, err);
      if (err == 0) sba_access_w(1'b1, addr, 32, v, err);
      hits++;
      hit_busy = 0;
      repeat ($urandom_range(0, 20)) cycle();
    end
  endtask

  // --------------------------------------------------------------------------- tbre_fault

  protected function bit fault_match(bit [31:0] addr);
    if (fault_kind < 2) return addr == fault_addr;
    return ((addr & 'hfff) >> 2) == (((fault_addr - HeapBase) >> 8) & 'h3ff);
  endfunction

  protected task tbre_fault();
    string req = fault_kind < 2 ? {PdMain, ".", TbrePort, "_req"} : TbreRevbmReq;
    string rsp = fault_kind < 2 ? {PdMain, ".", TbrePort, "_rsp"} : TbreRevbmRsp;
    tlul_pkg::tl_h2d_t h;
    tlul_pkg::tl_d2h_t d;
    bit found;
    bit [top_pkg::TL_AIW-1:0] src;
    while (!fault_stop) begin
      cycle();
      h = rd_h2d_path(req);
      d = rd_d2h_path(rsp);
      if (h.a_valid && d.a_ready && h.a_opcode == tlul_pkg::Get && fault_match(h.a_address)) begin
        src = h.a_source;
        found = 1;
        break;
      end
    end
    while (found && !fault_stop) begin
      cycle();
      d = rd_d2h_path(rsp);
      h = rd_h2d_path(req);
      if (d.d_valid && d.d_source == src) begin
        tlul_pkg::tl_d2h_t bad = d;
        int unsigned b = $urandom_range(0, 6);
        if (fault_kind % 2 == 0) begin
          bad.d_error = 1'b1;
          bad.d_user.rsp_intg = tlul_pkg::get_rsp_intg(bad);
        end else if ($urandom_range(0, 1)) begin
          bad.d_user.rsp_intg[b] = !bad.d_user.rsp_intg[b];
        end else begin
          bad.d_user.data_intg[b] = !bad.d_user.data_intg[b];
        end
        // TODO(inject-registry): announce this fault (source this vseq, target `rsp`, kind
        // fault_kind, now, one response).
        `uvm_info("EG2_INJECT", $sformatf("EG2 fault: %s kind %0d on the response to 0x%08x",
                                          rsp, fault_kind, fault_addr), UVM_LOW)
        `DV_CHECK_FATAL(uvm_hdl_force(rsp, uvm_hdl_data_t'(bad)), rsp)
        fault_hit = h.d_ready;
        cpu_clk();
        `DV_CHECK_FATAL(uvm_hdl_release(rsp), rsp)
        break;
      end
    end
    if (!fault_hit) begin
      `uvm_info(`gfn, $sformatf("EG2 bench: TBRE fault kind %0d at 0x%08x did not take effect",
                                fault_kind, fault_addr), UVM_LOW)
    end
    fault_done = 1;
  endtask
endclass

// chip_sw_cheriot_revoke_store_clear (REQ_CMS_10, REQ_CMS_19): software only.
class chip_sw_eg2_revoke_store_clear_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revoke_store_clear_vseq)
  `uvm_object_new
  rand int unsigned n_cases;
  constraint knobs_c {n_cases inside {[32:128]};}
  virtual function void write_params();
    super.write_params();
    set_param("n_cases", n_cases);
  endfunction
endclass

// chip_sw_cheriot_revocation_race (REQ_CMS_11, REQ_CMS_12): race_toggle (SBA: use_jtag,
// +use_jtag_dmi=1), tbre_hold and tbre_watch are bench steps.
class chip_sw_eg2_revocation_race_vseq extends chip_sw_eg2_revocation_corner_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_race_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_sba_batches, spin_max, n_hold;
  constraint knobs_c {
    n_sba_batches inside {[4:12]};
    spin_max dist {[1:50] :/ 30, [51:400] :/ 50, [401:1500] :/ 20};
    n_hold inside {[2:4]};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_sba_batches", n_sba_batches);
    set_param("spin_max", spin_max);
    set_param("n_hold", n_hold);
  endfunction
endclass

// chip_sw_cheriot_tbre_fault_<kind> (REQ_CMS_12, REQ_CMS_18): tbre_fault_arm and tbre_fault_end are
// bench steps; +eg2_param_fault=<1-3> pins the second case.
class chip_sw_eg2_tbre_fault_vseq extends chip_sw_eg2_revocation_corner_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_fault_vseq)
  `uvm_object_new
  rand int unsigned fault;
  constraint knobs_c {fault inside {[1:3]};}
  virtual function void write_params();
    super.write_params();
    set_param("fault", fault);
  endfunction
endclass

// chip_sw_cheriot_revocation_irq (REQ_CIB_05, REQ_CMS_11, REQ_CMS_12): software only (rv_timer,
// the TBRE's interrupt and the watchdog's NMI are the software's).
class chip_sw_eg2_revocation_irq_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_irq_vseq)
  `uvm_object_new
  rand int unsigned n_rounds, period, nmi_bark;
  constraint knobs_c {
    n_rounds inside {[8:24]};
    period dist {[150:400] :/ 40, [401:2000] :/ 60};
    nmi_bark inside {[2:8]};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_rounds", n_rounds);
    set_param("period", period);
    set_param("nmi_bark", nmi_bark);
  endfunction
endclass

// chip_sw_cheriot_fetch_backpressure (REQ_CIB_06, REQ_CMS_11): the response delays and the SBA reads
// are bench steps (use_jtag, +use_jtag_dmi=1); the code region, the instruction cache and the marks
// are drawn by the software per run.
class chip_sw_eg2_fetch_backpressure_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_fetch_backpressure_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_runs;
  constraint knobs_c {n_runs inside {[4:12]};}
  virtual function void write_params();
    super.write_params();
    set_param("n_runs", n_runs);
  endfunction
endclass

// chip_cheriot_cap_halves (REQ_CMS_10, REQ_CDM_09): the assertion is tb/eg2_cap_halves_chk.sv (on
// with +eg2_cap_halves=1); sba_hit (use_jtag, +use_jtag_dmi=1), the response delays and
// cap_halves_count are bench steps.
class chip_sw_eg2_cap_halves_vseq extends chip_sw_eg2_revocation_corner_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_cap_halves_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_runs;
  constraint knobs_c {n_runs inside {[4:12]};}
  virtual function void write_params();
    super.write_params();
    set_param("n_runs", n_runs);
  endfunction
endclass

// chip_sw_cheriot_tbre_progress (REQ_CIB_06, REQ_CMS_12): software only.
class chip_sw_eg2_tbre_progress_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_progress_vseq)
  `uvm_object_new
  rand int unsigned n_words, idle_passes;
  constraint knobs_c {
    n_words inside {[16:256]};
    idle_passes inside {[4:16]};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_words", n_words);
    set_param("idle_passes", idle_passes);
  endfunction
endclass

// chip_sw_cheriot_tbre_corners (REQ_CMS_12, REQ_CMS_15): the response delays and the SBA reads are
// bench steps (use_jtag, +use_jtag_dmi=1); the resets are the software's.
class chip_sw_eg2_tbre_corners_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_corners_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned contention;
  constraint knobs_c {contention dist {0 := 10, 1 := 30, 2 := 30, 3 := 30};}
  virtual function void write_params();
    super.write_params();
    set_param("contention", contention);
  endfunction
endclass

// chip_sw_cheriot_revocation_sleep (REQ_CMS_15, REQ_PBF_18): software only (the AON timer wakes the
// chip; +eg2_param_sleeps=1|2|3 selects normal, deep or both).
class chip_sw_eg2_revocation_sleep_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_sleep_vseq)
  `uvm_object_new
endclass
