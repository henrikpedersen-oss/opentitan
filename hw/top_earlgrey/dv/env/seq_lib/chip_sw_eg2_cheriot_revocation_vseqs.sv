// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The vseqs of the revocation corner cases (eg2_top_verification, spec "Revocation", "Corner
// cases"; images in //sw/device/tests/eg2_cheriot, their shared steps in eg2_revoke.h). Each holds
// its test's knobs as rand fields and writes them into the image (write_params); `model_gaps` is
// never written: the chip makes every check.
//
// chip_sw_eg2_revocation_bench_vseq is the bench's half the corner cases share, as bench steps
// (EG2 BENCH <n> <op> <a> <b>, chip_sw_eg2_rand_vseq):
//
//   rsp_delay <on> 0        Response delays on the SRAM link: the D channels of the main SRAM's,
//                           the sec SRAM's and the meta SRAM's (tags and the revocation bitmap)
//                           device ports are held for 1-rsp_delay_max cycles at a time, starting
//                           in a cycle with rsp_delay_pct % chance. A hold starts only in a cycle
//                           without a response on the port, so the host never sees d_valid fall
//                           (TL-UL): d_valid is held at 0 towards the host and d_ready at 0
//                           towards the device, which keeps its response until the hold ends.
//                           Legal stimulus, no fault: nothing is announced to the injection
//                           registry.
//   count_revbm <lo> <hi>   The bitmap reads counted from now on: those of the bitmap words that
//                           cover the heap granules [lo, hi) (default: all).
//   count_begin <lo> <hi>   Starts counting A-channel handshakes: requests to [lo, hi) on the
//   count_end 0 0           core's data port (rv_core_ibex to cheriot_mem_sys) and on
//                           cheriot_mem_sys's port to the crossbar, and the core's bitmap reads
//                           (corerevbm). The answer is {bitmap[29:20], xbar[19:10], core[9:0]},
//                           each saturated; the software compares them with the requests it made
//                           ("every request reaches the bus once").
//   sba_noise <on> 0        SBA reads of sec SRAM in the background, contention on the crossbar
//                           (the vseq sets use_jtag).
//   tbre_watch_begin <lo> <hi>, tbre_watch_end <which> 0
//                           Counts the TBRE's reads (cheriot_mem_sys's tbre port) inside [lo, hi)
//                           (which 0) and outside it (which 1).
//   snoop_watch <on> 0      Counts the core's writes to a capability the TBRE read in the last
//                           SnoopWindow cycles (a write to the capability the TBRE is resolving);
//                           the answer to `snoop_watch 0 0` is the count.
class chip_sw_eg2_revocation_bench_vseq extends chip_sw_eg2_cheriot_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_bench_vseq)
  `uvm_object_new

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  // The ports the response delays hold: main SRAM, sec SRAM, meta SRAM.
  localparam string DelayPort[3] = '{"sram_ctrl_main_ram_tl", "sram_ctrl_sec_ram_tl",
                                     "cheriot_mem_sys_meta_sram_tl"};
  localparam string CoreData = "rv_core_ibex_cored_tl_h";
  localparam string XbarData = "main_tl_cheriot_mem_sys__cored";
  localparam string CoreRevbm = "rv_core_ibex_corerevbm_tl";
  localparam string TbrePort = "main_tl_cheriot_mem_sys__tbre";
  localparam bit [31:0] HeapBase = 32'h1000_0000;
  localparam bit [31:0] SecSramBase = 32'h1002_0000;
  localparam int unsigned SecSramWords = 'h10000 / 4;
  localparam int unsigned SnoopWindow = 16;

  // Response delays (bench-only knobs: +eg2_param_<name> pins them, as for the software's).
  rand bit [2:0]    rsp_delay_ports;
  rand int unsigned rsp_delay_pct;
  rand int unsigned rsp_delay_max;
  constraint rsp_delay_c {
    rsp_delay_ports != 0;
    rsp_delay_pct inside {[5:50]};
    rsp_delay_max dist {[1:4] :/ 40, [5:32] :/ 40, [33:200] :/ 20};
  }

  protected bit delay_on, count_on, sba_on, sba_busy, watch_on, snoop_on;
  protected int unsigned held_cycles[3], holds[3];
  protected int unsigned cnt_core, cnt_xbar, cnt_revbm;
  protected bit [31:0] cnt_lo, cnt_hi, watch_lo, watch_hi;
  protected bit        revbm_filter;
  protected bit [31:0] revbm_wlo, revbm_whi;
  protected int unsigned watch_in, watch_out, snoop_hits, sba_reads;

  virtual function void write_params();
    super.write_params();
    void'($value$plusargs("eg2_param_rsp_delay_ports=%d", rsp_delay_ports));
    void'($value$plusargs("eg2_param_rsp_delay_pct=%d", rsp_delay_pct));
    void'($value$plusargs("eg2_param_rsp_delay_max=%d", rsp_delay_max));
    `uvm_info(`gfn, $sformatf("EG2 bench: response delays on ports 0b%03b, %0d %% per cycle, up to %0d cycles",
                              rsp_delay_ports, rsp_delay_pct, rsp_delay_max), UVM_LOW)
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), $sformatf("cannot read %s", path))
    return v;
  endfunction

  protected function tlul_pkg::tl_h2d_t rd_h2d(string port);
    uvm_hdl_data_t v = rd({PdMain, ".", port, "_req"});
    return tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
  endfunction

  protected function tlul_pkg::tl_d2h_t rd_d2h(string port);
    uvm_hdl_data_t v = rd({PdMain, ".", port, "_rsp"});
    return tlul_pkg::tl_d2h_t'(v[$bits(tlul_pkg::tl_d2h_t)-1:0]);
  endfunction

  // A-channel handshake on `port` in the current cycle (sampled after the clock edge has settled).
  protected function bit a_fire(string port, output bit [31:0] addr, output bit write);
    tlul_pkg::tl_h2d_t h = rd_h2d(port);
    tlul_pkg::tl_d2h_t d = rd_d2h(port);
    addr = h.a_address;
    write = h.a_opcode != tlul_pkg::Get;
    return h.a_valid && d.a_ready;
  endfunction

  // One cycle of the main clock, then past the edge's updates.
  protected task cycle();
    cpu_clk();
    #1ps;
  endtask

  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    result = 0;
    case (op)
      "rsp_delay": begin
        if (a != 0 && !delay_on) begin
          delay_on = 1;
          foreach (DelayPort[i]) begin
            automatic int unsigned p = i;
            if (rsp_delay_ports[p]) fork rsp_delay(p); join_none
          end
        end else if (a == 0 && delay_on) begin
          delay_on = 0;
          // Each hold ends within rsp_delay_max cycles.
          repeat (rsp_delay_max + 2) cycle();
          foreach (DelayPort[i]) begin
            `uvm_info(`gfn, $sformatf("EG2 bench: %s responses held %0d times, %0d cycles in all",
                                      DelayPort[i], holds[i], held_cycles[i]), UVM_LOW)
          end
        end
      end
      "count_revbm": begin
        // Granules [a, b) of the heap: bitmap words (g - HeapBase) / 256, as the port's 12-bit
        // byte address sees them (modulo 1024 words; a range inside one 256 KiB alias).
        revbm_wlo = ((a - HeapBase) >> 8) & 'h3ff;
        revbm_whi = (((b - 1) - HeapBase) >> 8) & 'h3ff;
        revbm_filter = 1;
      end
      "count_begin": begin
        cnt_lo = a;
        cnt_hi = b;
        cnt_core = 0;
        cnt_xbar = 0;
        cnt_revbm = 0;
        count_on = 1;
        fork count_loop(); join_none
      end
      "count_end": begin
        count_on = 0;
        cycle();
        `uvm_info(`gfn, $sformatf("EG2 bench: requests to [0x%08x, 0x%08x): core %0d, crossbar %0d; bitmap reads %0d",
                                  cnt_lo, cnt_hi, cnt_core, cnt_xbar, cnt_revbm), UVM_LOW)
        result = sat10(cnt_core) | (sat10(cnt_xbar) << 10) | (sat10(cnt_revbm) << 20);
      end
      "sba_noise": begin
        if (a != 0 && !sba_on) begin
          sba_on = 1;
          fork sba_noise(); join_none
        end else if (a == 0) begin
          sba_on = 0;
          wait (!sba_busy);
          `uvm_info(`gfn, $sformatf("EG2 bench: %0d SBA reads of sec SRAM", sba_reads), UVM_LOW)
        end
      end
      "tbre_watch_begin": begin
        watch_lo = a;
        watch_hi = b;
        watch_in = 0;
        watch_out = 0;
        watch_on = 1;
        fork tbre_watch(); join_none
      end
      "tbre_watch_end": begin
        if (watch_on) begin
          watch_on = 0;
          cycle();
          `uvm_info(`gfn, $sformatf("EG2 bench: TBRE reads in [0x%08x, 0x%08x): %0d, outside: %0d",
                                    watch_lo, watch_hi, watch_in, watch_out), UVM_LOW)
        end
        result = a == 0 ? watch_in : watch_out;
      end
      "snoop_watch": begin
        if (a != 0) begin
          snoop_hits = 0;
          snoop_on = 1;
          fork snoop_watch(); join_none
        end else begin
          snoop_on = 0;
          cycle();
          `uvm_info(`gfn, $sformatf("EG2 bench: %0d core writes to a capability the TBRE had in flight",
                                    snoop_hits), UVM_LOW)
          result = snoop_hits;
        end
      end
      default: super.bench_op(op, a, b, result);
    endcase
  endtask

  protected function bit [31:0] sat10(int unsigned n);
    return n > 'h3ff ? 'h3ff : n;
  endfunction

  protected task rsp_delay(int unsigned p);
    string req = {PdMain, ".", DelayPort[p], "_req"};
    string rsp = {PdMain, ".", DelayPort[p], "_rsp"};
    while (delay_on) begin
      int unsigned len;
      cycle();
      if ($urandom_range(0, 99) >= rsp_delay_pct) continue;
      // Only between responses: the host never sees a presented response withdrawn.
      if (rd({rsp, ".d_valid"}) != 0) continue;
      len = $urandom_range(1, rsp_delay_max);
      if (!(uvm_hdl_force({rsp, ".d_valid"}, 0))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_force({rsp, \".d_valid\"}, 0)) %s", rsp))
      if (!(uvm_hdl_force({req, ".d_ready"}, 0))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_force({req, \".d_ready\"}, 0)) %s", req))
      repeat (len) cpu_clk();
      #1ps;
      if (!(uvm_hdl_release({req, ".d_ready"}))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_release({req, \".d_ready\"})) %s", req))
      if (!(uvm_hdl_release({rsp, ".d_valid"}))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_release({rsp, \".d_valid\"})) %s", rsp))
      holds[p]++;
      held_cycles[p] += len;
    end
  endtask

  protected task count_loop();
    bit [31:0] addr;
    bit write;
    while (count_on) begin
      cycle();
      if (a_fire(CoreData, addr, write) && addr >= cnt_lo && addr < cnt_hi) cnt_core++;
      if (a_fire(XbarData, addr, write) && addr >= cnt_lo && addr < cnt_hi) cnt_xbar++;
      // The bitmap port's address: the word's byte offset in the 3 KiB bitmap (the meta SRAM and
      // the bitmap base are 4 KiB aligned).
      if (a_fire(CoreRevbm, addr, write) &&
          (!revbm_filter || ((addr & 'hfff) >> 2) inside {[revbm_wlo:revbm_whi]})) begin
        cnt_revbm++;
      end
    end
  endtask

  protected task sba_noise();
    while (sba_on) begin
      bit [31:0] v, err;
      sba_busy = 1;
      sba_read32(SecSramBase + 4 * $urandom_range(0, SecSramWords - 1), v, err);
      sba_reads++;
      sba_busy = 0;
      cycle();
    end
  endtask

  protected task tbre_watch();
    bit [31:0] addr;
    bit write;
    while (watch_on) begin
      cycle();
      if (a_fire(TbrePort, addr, write)) begin
        if (addr >= watch_lo && addr < watch_hi) watch_in++;
        else watch_out++;
      end
    end
  endtask

  protected task snoop_watch();
    bit [31:0] addr, cap[$];
    int unsigned age[$];
    bit write;
    while (snoop_on) begin
      cycle();
      foreach (age[i]) age[i]++;
      while (age.size() && age[0] > SnoopWindow) begin
        void'(age.pop_front());
        void'(cap.pop_front());
      end
      if (a_fire(TbrePort, addr, write)) begin
        cap.push_back(addr & ~32'h7);
        age.push_back(0);
      end
      if (a_fire(CoreData, addr, write) && write) begin
        foreach (cap[i]) begin
          if (cap[i] == (addr & ~32'h7)) begin
            snoop_hits++;
            break;
          end
        end
      end
    end
  endtask
endclass

// chip_sw_cheriot_trvk_backpressure (REQ_CIB_06, REQ_CMS_11): the response delays, the SBA traffic
// (use_jtag, +use_jtag_dmi=1) and the counts are bench steps; the TBRE sweep and the timer are the
// software's.
class chip_sw_eg2_trvk_backpressure_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_trvk_backpressure_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_rounds, marked_pct, delays, contention, irq, irq_delay;
  constraint knobs_c {
    n_rounds inside {[8:28]};
    marked_pct dist {[1:20] :/ 25, [21:79] :/ 50, [80:100] :/ 25};
    // Response delays in most runs; contention: none, the TBRE, SBA, both (mostly both).
    delays dist {1 := 80, 0 := 20};
    contention dist {0 := 10, 1 := 20, 2 := 20, 3 := 50};
    irq dist {1 := 80, 0 := 20};
    irq_delay inside {[20:400]};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_rounds", n_rounds);
    set_param("marked_pct", marked_pct);
    set_param("delays", delays);
    set_param("contention", contention);
    set_param("irq", irq);
    set_param("irq_delay", irq_delay);
  endfunction
endclass

// chip_sw_cheriot_trvk_truth_table (REQ_CMS_05, REQ_CMS_11): software only.
class chip_sw_eg2_trvk_truth_table_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_trvk_truth_table_vseq)
  `uvm_object_new
  rand int unsigned n_random, rram_cases;
  constraint knobs_c {
    n_random inside {[16:128]};
    rram_cases dist {1 := 80, 0 := 20};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_random", n_random);
    set_param("rram_cases", rram_cases);
  endfunction
endclass

// chip_sw_cheriot_tbre_core_write_snoop (REQ_CMS_12): the snoop watch and the response delays are
// bench steps.
class chip_sw_eg2_tbre_core_write_snoop_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_core_write_snoop_vseq)
  `uvm_object_new
  rand int unsigned n_slots, n_sweeps, marked_pct, write_pct, delays;
  constraint knobs_c {
    n_slots dist {[64:255] :/ 30, [256:1024] :/ 70};
    n_sweeps inside {[4:8]};
    marked_pct inside {[10:90]};
    write_pct inside {[30:100]};
    delays dist {1 := 75, 0 := 25};
  }
  virtual function void write_params();
    super.write_params();
    set_param("n_slots", n_slots);
    set_param("n_sweeps", n_sweeps);
    set_param("marked_pct", marked_pct);
    set_param("write_pct", write_pct);
    // The write kinds: fresh tagged 40, untagged 20, lower word 20, upper word 20.
    set_param("w_csc_tagged", 40);
    set_param("w_csc_untagged", 20);
    set_param("w_sw_lo", 20);
    set_param("w_sw_hi", 20);
    set_param("delays", delays);
  endfunction
endclass

// chip_sw_cheriot_tbre_regs[_cheriot] (REQ_CMS_12): the TBRE's read watch is a bench step.
class chip_sw_eg2_tbre_regs_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_tbre_regs_vseq)
  `uvm_object_new
  rand int unsigned rram_top, past_top_extra;
  constraint knobs_c {
    rram_top dist {1 := 50, 0 := 50};
    past_top_extra inside {[1:64]};
  }
  virtual function void write_params();
    super.write_params();
    set_param("rram_top", rram_top);
    set_param("past_top_extra", past_top_extra);
  endfunction
endclass

// chip_sw_cheriot_revbm_write_order (REQ_CMS_11): the SBA writes are bench steps (sba_wr32;
// use_jtag, +use_jtag_dmi=1).
class chip_sw_eg2_revbm_write_order_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revbm_write_order_vseq)
  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;
  endfunction
  rand int unsigned n_random;
  constraint knobs_c {n_random inside {[4:32]};}
  virtual function void write_params();
    super.write_params();
    set_param("n_random", n_random);
  endfunction
endclass

// chip_sw_cheriot_revocation_bounds (REQ_CMS_05, REQ_CMS_11, REQ_CIB_04): the request counts are
// bench steps; no knobs.
class chip_sw_eg2_revocation_bounds_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revocation_bounds_vseq)
  `uvm_object_new
endclass
