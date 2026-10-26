// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_ctrl_ops (sw/device/tests/eg2_rram/eg2_rram_ctrl_ops.c; EG2 spec REQ_RRAM_13:
// operation (read, write, Rewrite); length 1 word to several pages, log-uniform; alignment class).
class chip_sw_eg2_rram_ctrl_ops_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_ctrl_ops_vseq)
  `uvm_object_new

  rand int unsigned n_ops;
  rand int unsigned w_write, w_read, w_rewrite, w_unaligned, w_bad_op;
  rand int unsigned len_class, fifo_full, scramble;

  constraint n_ops_c { n_ops inside {[8:48]}; }
  // The default weights (30, 30, 15, 15, 10), +-50 % per seed; none unreachable.
  constraint w_op_c {
    w_write inside {[15:45]};
    w_read inside {[15:45]};
    w_rewrite inside {[8:22]};
    w_unaligned inside {[8:22]};
    w_bad_op inside {[5:15]};
  }
  constraint misc_c {
    len_class dist {[0:2] :/ 40, 3 := 60};
    fifo_full dist {1 := 70, 0 := 30};
    scramble inside {[0:2]};
  }

  virtual function void write_params();
    set_param("n_ops", n_ops);
    set_param("w_write", w_write);
    set_param("w_read", w_read);
    set_param("w_rewrite", w_rewrite);
    set_param("w_unaligned", w_unaligned);
    set_param("w_bad_op", w_bad_op);
    set_param("len_class", len_class);
    set_param("fifo_full", fifo_full);
    set_param("scramble", scramble);
  endfunction
endclass : chip_sw_eg2_rram_ctrl_ops_vseq
