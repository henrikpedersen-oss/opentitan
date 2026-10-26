// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_exec_en (sw/device/tests/eg2_rram/eg2_rram_exec_en.c; EG2 spec REQ_RRAM_02,
// REQ_TL_05: EXEC each encoding class uniform).
class chip_sw_eg2_rram_exec_en_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_exec_en_vseq)
  `uvm_object_new

  rand int unsigned n_values;
  rand int unsigned w_exec_magic, w_exec_zero, w_exec_flip, w_exec_inv, w_exec_random;
  rand int unsigned target_class;

  constraint n_values_c { n_values inside {[4:32]}; }
  // Each class of EXEC value about as likely as the others (uniform, +-50 % per seed).
  constraint w_exec_c {
    w_exec_magic inside {[10:30]};
    w_exec_zero inside {[10:30]};
    w_exec_flip inside {[10:30]};
    w_exec_inv inside {[10:30]};
    w_exec_random inside {[10:30]};
  }
  constraint target_c { target_class dist {0 := 1, 1 := 1, 2 := 2}; }

  virtual function void write_params();
    set_param("n_values", n_values);
    set_param("w_exec_magic", w_exec_magic);
    set_param("w_exec_zero", w_exec_zero);
    set_param("w_exec_flip", w_exec_flip);
    set_param("w_exec_inv", w_exec_inv);
    set_param("w_exec_random", w_exec_random);
    set_param("target_class", target_class);
  endfunction
endclass : chip_sw_eg2_rram_exec_en_vseq
