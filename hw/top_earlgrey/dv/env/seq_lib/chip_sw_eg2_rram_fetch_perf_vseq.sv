// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_fetch_perf (sw/device/tests/eg2_rram/eg2_rram_fetch_perf.c; EG2 spec REQ_RRAM_06:
// iCache 0, 1; read-buffer pattern (linear, branchy, data interleaved A/C/D) uniform). The cycle
// budgets are the test's defaults (RFC RRAM §4.5.3) unless pinned: +eg2_param_budget_word=<n>.
class chip_sw_eg2_rram_fetch_perf_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_fetch_perf_vseq)
  `uvm_object_new

  rand int unsigned pattern, n_words, icache;

  constraint knobs_c {
    pattern dist {[0:4] :/ 70, 5 := 30};  // 5: every pattern
    n_words dist {[4:8] :/ 40, [9:32] :/ 40, [33:64] :/ 20};
    icache inside {[0:1]};
  }

  virtual function void write_params();
    set_param("pattern", pattern);
    set_param("n_words", n_words);
    set_param("icache", icache);
  endfunction
endclass : chip_sw_eg2_rram_fetch_perf_vseq
