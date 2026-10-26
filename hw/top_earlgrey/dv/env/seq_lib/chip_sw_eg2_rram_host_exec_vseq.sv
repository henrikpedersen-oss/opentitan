// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_host_exec (sw/device/tests/eg2_rram/eg2_rram_host_exec.c; EG2 spec REQ_RRAM_01,
// "RRAM controller in RISC-V mode": access fetch, load uniform; the address classes first and last
// word of a page, a straddling word, random; the iCache).
class chip_sw_eg2_rram_host_exec_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_host_exec_vseq)
  `uvm_object_new

  rand int unsigned n_ops;
  rand int unsigned w_fetch, w_load;
  rand int unsigned w_addr_first, w_addr_last, w_addr_straddle, w_addr_random;
  rand int unsigned scramble, icache;

  constraint n_ops_c { n_ops dist {[4:16] :/ 60, [17:64] :/ 40}; }
  // Fetch and load uniform, +-20 % per seed.
  constraint w_op_c {
    w_fetch inside {[40:60]};
    w_load inside {[40:60]};
  }
  // The weight rule: the boundary classes (first, last, straddle) at least 60 % of the total, the
  // random class at least 5 % and at most 30 %; none unreachable.
  constraint w_addr_c {
    w_addr_first inside {[1:100]};
    w_addr_last inside {[1:100]};
    w_addr_straddle inside {[1:100]};
    w_addr_random inside {[1:100]};
    (w_addr_first + w_addr_last + w_addr_straddle) * 10 >=
        (w_addr_first + w_addr_last + w_addr_straddle + w_addr_random) * 6;
    w_addr_random * 10 <= (w_addr_first + w_addr_last + w_addr_straddle + w_addr_random) * 3;
    w_addr_random * 20 >= (w_addr_first + w_addr_last + w_addr_straddle + w_addr_random);
  }
  constraint misc_c {
    scramble inside {[0:2]};
    icache dist {0 := 1, 1 := 1, 2 := 1, 3 := 2};
  }

  virtual function void write_params();
    set_param("n_ops", n_ops);
    set_param("w_fetch", w_fetch);
    set_param("w_load", w_load);
    set_param("w_addr_first", w_addr_first);
    set_param("w_addr_last", w_addr_last);
    set_param("w_addr_straddle", w_addr_straddle);
    set_param("w_addr_random", w_addr_random);
    set_param("scramble", scramble);
    set_param("icache", icache);
    // model_gaps stays 0 on the chip.
  endfunction
endclass : chip_sw_eg2_rram_host_exec_vseq
