// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The random-number tests (eg2_top_verification spec "Random Number Generation Test Suite",
// REQ_PBF_24, REQ_PBF_12; software in sw/device/tests/eg2_rng). Compiled in build mode en_eg2_rng
// only (EG2_RNG: chip_eg2_rng_tests.hjson), where tb/eg2_rng_src.svh drives entropy_src's noise
// input from the source dv/eg2_rng/eg2_rng_source.h describes.
//
// The bench side of the tests' contract (eg2_rng.h): a bench step is the DV status word written
// with 16'h5200 | step; the vseq acts in the simulation step it sees it, then the software writes
// "in test" back. The knobs are drawn here and written into the image (kEg2Param_<name>,
// EG2_RM_PARAMS), so the pattern the bench selects and the threshold the software programs come
// from the same draw.

// The DPI of tb/eg2_rng_src.svh (eg2_rng_dpi.c), imported again in the package.
import "DPI-C" function int eg2_rng_select(input string spec);
import "DPI-C" function longint eg2_rng_count();

class chip_sw_eg2_rng_base_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rng_base_vseq)
  `uvm_object_new

  localparam string     EsCore = {"tb.dut.top_earlgrey.earlgrey_pd_main.u_entropy_src.",
                                  "u_entropy_src_core"};
  localparam bit [15:0] BenchCode = 16'h5200;

  // The pattern generator's seed (the lanes a pattern leaves free).
  rand bit [31:0] pattern_seed;
  constraint pattern_seed_c {
    pattern_seed != 0;
  }

  // Waits for the software's bench step `step` (eg2_rng_bench_step).
  protected task wait_bench_step(int step);
    wait (cfg.sw_test_status_vif.sw_test_status == sw_test_status_e'(BenchCode | 16'(step)));
    `uvm_info(`gfn, $sformatf("EG2_RNG bench step %0d", step), UVM_LOW)
  endtask

  // A knob pinned on the command line (+eg2_param_<name>), as chip_sw_eg2_base_vseq writes it.
  protected function bit [31:0] pinned(string name, bit [31:0] value);
    bit [31:0] p;
    if ($value$plusargs({"eg2_param_", name, "=%d"}, p)) return p;
    return value;
  endfunction

  // Selects the noise source; a source the DPI refuses ends the run.
  protected function void select_source(string spec);
    if (eg2_rng_select(spec) != 0) begin
      `uvm_fatal(`gfn, $sformatf("EG2_RNG the bench cannot select %s", spec))
    end
    `uvm_info(`gfn, $sformatf("EG2_RNG source %s selected", spec), UVM_LOW)
  endfunction
endclass

// chip_sw_eg2_rng_health: one health test at threshold - 1, threshold or threshold + 1.
class chip_sw_eg2_rng_health_vseq extends chip_sw_eg2_rng_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rng_health_vseq)
  `uvm_object_new

  // The health tests in their threshold register order (eg2_rng_ht_t). 4 (adaptps) and 8/9 (the
  // external health test) are gaps of the software (no defined statistic; xht tied off).
  rand int unsigned ht;
  rand int unsigned side;       // 0: V = T - 1, 1: V = T, 2: V = T + 1
  rand int unsigned threshold;
  rand bit          fips;
  rand int unsigned window;     // FIPS_WINDOW, symbols
  rand int unsigned w;          // the window the pattern counts in: FIPS_WINDOW, or 96 in boot mode

  constraint ht_c {
    ht inside {0, 1, 2, 3, 5, 6, 7};
    side inside {[0 : 2]};
  }
  // entropy_src.hjson FIPS_WINDOW: below 512 samples "not recommended"; 256 is kept as a corner.
  constraint window_c {
    window dist {256 :/ 10, 512 :/ 40, 1024 :/ 40, 2048 :/ 10};
    w == (fips ? window : 96);
  }
  // Each pattern's statistic V must be one the pattern can make in a window of w symbols
  // (eg2_rng_source.h): a run of at least 1, at most w ones or zero symbols, at least w / 16
  // zero symbols for the bucket test, at most w / 2 switching pairs.
  constraint threshold_c {
    ht inside {0, 1} -> threshold inside {[2 : 40]};
    ht == 2 -> threshold inside {[w / 2 : w - 1]};
    ht == 3 -> threshold inside {[1 : w / 2]};
    ht == 5 -> threshold inside {[w / 16 + 1 : w - 1]};
    ht == 6 -> threshold inside {[w / 4 : w / 2 - 1]};
    ht == 7 -> threshold inside {[1 : w / 4]};
  }

  virtual function void write_params();
    set_param("test", ht);
    set_param("side", side);
    set_param("threshold", threshold);
    set_param("mode", fips);
    set_param("window", window);
    set_param("model_gaps", 0);
  endfunction

  // The pattern whose statistic is V on the knobs the software read (pinned ones included).
  protected function string pattern();
    bit [31:0] t = pinned("test", ht), s = pinned("side", side), th = pinned("threshold", threshold);
    bit [31:0] f = pinned("mode", fips), win = pinned("window", window);
    bit [31:0] ww = f ? win : 96;
    bit [31:0] v = s == 0 ? th - 1 : s == 1 ? th : th + 1;
    string p;
    case (t)
      0: p = $sformatf("pattern:repcnt:%0d", v);
      1: p = $sformatf("pattern:repcnts:%0d", v);
      2, 3: p = $sformatf("pattern:adaptp:%0d:%0d", v, ww);
      5: p = $sformatf("pattern:bucket:%0d:%0d", v, ww);
      6, 7: p = $sformatf("pattern:markov:%0d:%0d", v, ww);
      default: p = "";
    endcase
    if (p != "") p = $sformatf("%s,seed=%0d", p, pattern_seed);
    return p;
  endfunction

  virtual task body();
    string p;
    super.body();
    p = pattern();
    `uvm_info(`gfn, $sformatf("EG2_RNG health: test %0d side %0d threshold %0d %s window %0d: %s",
                              ht, side, threshold, fips ? "FIPS" : "boot", window,
                              p == "" ? "a gap of the software, no pattern" : p), UVM_LOW)
    if (p == "") return;
    wait_bench_step(1);
    select_source(p);
  endtask
endclass

// chip_sw_eg2_rng_stuck: the noise source stuck at 0 or 1, at boot or during operation.
class chip_sw_eg2_rng_stuck_vseq extends chip_sw_eg2_rng_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rng_stuck_vseq)
  `uvm_object_new

  // A bench allowance, not a document's: the symbols in entropy_src's pipeline (the esrng FIFO
  // and the health tests' input stage) when the Repetition Count test reaches its threshold.
  localparam int unsigned PipelineSymbols = 32;

  rand bit          value;
  rand bit          phase;      // 0 boot-time, 1 continuous
  rand int unsigned threshold;

  constraint threshold_c {
    threshold dist {[8 : 31] :/ 40, [32 : 128] :/ 50, [129 : 1024] :/ 10};
  }

  virtual function void write_params();
    set_param("value", value);
    set_param("phase", phase);
    set_param("threshold", threshold);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    bit [31:0] v = pinned("value", value), ph = pinned("phase", phase);
    bit [31:0] th = pinned("threshold", threshold);
    longint unsigned n0, n;
    uvm_hdl_data_t failed;
    super.body();
    wait_bench_step(ph ? 2 : 1);
    select_source($sformatf("pattern:stuck%0d", v));
    n0 = eg2_rng_count();
    // ALERT_THRESHOLD (2) failures of the RCT: the first at `threshold` equal symbols, then one
    // per symbol (ALERT_THRESHOLD's description: each symbol of a continuous test counts once).
    // alert_threshold_fail is a level (the fail count at or above ALERT_THRESHOLD), so polling
    // cannot miss it.
    forever begin
      #5ns;
      if (!uvm_hdl_read({EsCore, ".alert_threshold_fail"}, failed)) begin
        `uvm_fatal(`gfn, {"EG2_RNG no such signal ", EsCore, ".alert_threshold_fail"})
      end
      if (failed[0]) break;
    end
    n = eg2_rng_count() - n0;
    if (n > th + 2 + PipelineSymbols) begin
      `uvm_error(`gfn, $sformatf({"EG2_RNG stuck%0d: the health test failed after %0d stuck ",
                                  "symbols, beyond the threshold %0d + 2 + %0d"}, v, n, th,
                                 PipelineSymbols))
    end else begin
      `uvm_info(`gfn, $sformatf("EG2_RNG stuck%0d: the health test failed after %0d symbols",
                                v, n), UVM_LOW)
    end
  endtask
endclass

// chip_sw_eg2_rng_capture: every tap, `capture_words` each, from the host source (+eg2_rng=host
// in the test's run_opts; the source runs from time 0).
class chip_sw_eg2_rng_capture_vseq extends chip_sw_eg2_rng_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rng_capture_vseq)
  `uvm_object_new

  rand int unsigned capture_words;
  rand bit [3:0]    taps;
  rand bit [2:0]    modes;

  // The spec's default is 4096 words per tap; shorter captures keep the regression's time.
  constraint knobs_c {
    capture_words dist {[64 : 511] :/ 30, [512 : 2047] :/ 40, [2048 : 4096] :/ 30};
    capture_words % 8 == 0;
    taps dist {4'hf :/ 50, [4'h1 : 4'he] :/ 50};
    modes dist {3'h7 :/ 50, [3'h1 : 3'h6] :/ 50};
  }

  virtual function void write_params();
    set_param("capture_words", capture_words);
    set_param("taps", taps);
    set_param("modes", modes);
    set_param("model_gaps", 0);
  endfunction

  virtual task body();
    string src;
    if (!$value$plusargs("eg2_rng=%s", src) || src != "host") begin
      `uvm_error(`gfn, "EG2_RNG capture: the noise source must be +eg2_rng=host")
    end
    super.body();
  endtask
endclass

// chip_sw_eg2_rng_conditioner_kat: firmware insert through the SHA-3 conditioner.
class chip_sw_eg2_rng_conditioner_kat_vseq extends chip_sw_eg2_rng_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rng_conditioner_kat_vseq)
  `uvm_object_new

  rand int unsigned len_class;
  constraint len_class_c {
    len_class dist {[0 : 3] :/ 60, 4 :/ 40};
  }

  virtual function void write_params();
    set_param("len_class", len_class);
    set_param("model_gaps", 0);
  endfunction
endclass
