// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_fetch_during_write (sw/device/tests/eg2_rram/eg2_rram_fetch_during_write.c; EG2
// spec REQ_RRAM_04: write length 1 word to a page, log-uniform, with the spec's 1024-word case;
// fetch distance same, adjacent, far uniform; the iCache).
class chip_sw_eg2_rram_fetch_during_write_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_fetch_during_write_vseq)
  `uvm_object_new

  rand int unsigned len_log;  // the length class: 4 << len_log words
  rand int unsigned n_words, distance, probe_every, otp_write, icache;

  constraint n_words_c {
    solve len_log before n_words;
    len_log dist {[0:7] :/ 60, 8 := 40};  // 4..512 log-uniform; 1024 (the RFC's case) 40 %
    if (len_log == 8) {
      n_words == 1024;
    } else {
      n_words >= (4 << len_log);
      n_words < (8 << len_log);
      n_words % 4 == 0;
    }
  }
  constraint misc_c {
    distance inside {[0:2]};
    probe_every dist {1 := 2, [2:8] :/ 6, [9:64] :/ 2};
    otp_write dist {1 := 70, 0 := 30};
    icache inside {[0:2]};
  }

  virtual function void write_params();
    set_param("n_words", n_words);
    set_param("distance", distance);
    set_param("probe_every", probe_every);
    set_param("otp_write", otp_write);
    set_param("icache", icache);
  endfunction
endclass : chip_sw_eg2_rram_fetch_during_write_vseq
