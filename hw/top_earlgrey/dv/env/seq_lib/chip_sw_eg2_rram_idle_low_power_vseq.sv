// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_idle_low_power (sw/device/tests/eg2_rram/eg2_rram_idle_low_power.c; EG2 spec
// REQ_RRAM_26, REQ_SEC_21: the sleep entry point during a write (start, middle, end), the sleep
// type). The test's second boot (after the low-power exit) needs nothing from the bench: the
// software tells the boots apart by the reset reason the test ROM records.
class chip_sw_eg2_rram_idle_low_power_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_idle_low_power_vseq)
  `uvm_object_new

  rand int unsigned n_words, entry, sleep_type, wake_ticks;

  constraint knobs_c {
    n_words dist {[4:64] :/ 20, [65:512] :/ 40, [513:1024] :/ 40};
    n_words % 4 == 0;
    entry inside {[0:2]};
    sleep_type inside {[0:1]};
    wake_ticks inside {[10:40]};
  }

  virtual function void write_params();
    set_param("n_words", n_words);
    set_param("entry", entry);
    set_param("sleep_type", sleep_type);
    set_param("wake_ticks", wake_ticks);
  endfunction
endclass : chip_sw_eg2_rram_idle_low_power_vseq
