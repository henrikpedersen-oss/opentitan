// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_write_then_fetch (sw/device/tests/eg2_rram/eg2_rram_write_then_fetch.c; EG2 spec
// REQ_RRAM_05: write length 1 word to a page, log-uniform; fetch distance from the written page
// same, adjacent, far uniform; the iCache).
class chip_sw_eg2_rram_write_then_fetch_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_write_then_fetch_vseq)
  `uvm_object_new

  rand int unsigned n_rounds, write_class, distance, icache, scramble;

  constraint knobs_c {
    n_rounds inside {[2:16]};
    write_class dist {[0:2] :/ 40, 3 := 60};  // 3: log-uniform per round
    distance dist {[0:2] :/ 60, 3 := 40};     // 3: drawn per round
    icache dist {[0:2] :/ 50, 3 := 50};
    scramble inside {[0:2]};
  }

  virtual function void write_params();
    set_param("n_rounds", n_rounds);
    set_param("write_class", write_class);
    set_param("distance", distance);
    set_param("icache", icache);
    set_param("scramble", scramble);
  endfunction
endclass : chip_sw_eg2_rram_write_then_fetch_vseq
