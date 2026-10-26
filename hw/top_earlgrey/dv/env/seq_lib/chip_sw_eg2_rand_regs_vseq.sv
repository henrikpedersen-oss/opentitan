// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The knobs of chip_sw_eg2_rand_regs (sw/device/tests/eg2_regmem/eg2_rand_regs.c; EG2 verification
// spec "Registers and memories"), drawn under the spec's distributions. The software draws each
// register's operation, data and width from its seed within them.
class chip_sw_eg2_rand_regs_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rand_regs_vseq)

  `uvm_object_new

  // EG2_RM_BLOCK_COUNT of the generated eg2_regmem_tables.h: every register window of the top.
  // The software rejects a larger n_windows, so a top with fewer windows fails at once.
  localparam int unsigned NumWindows = 48;

  typedef enum int unsigned {
    DataZeros, DataOnes, DataWalk, DataMax, DataRand, DataNum
  } data_e;
  // The plain (non-boundary) data class.
  localparam bit [DataNum-1:0] DataPlain = 1 << DataRand;

  rand int unsigned n_windows;
  rand int unsigned max_regs;
  rand int unsigned n_patterns;
  rand int unsigned w_op_reset, w_op_write, w_op_walk, w_op_alias, w_op_regwen, w_op_shadow;
  rand int unsigned w_data[DataNum];
  rand bit          boost;
  rand data_e       boosted;
  rand int unsigned w_width_word, w_width_half, w_width_byte;
  rand int unsigned order;
  rand bit          reset_read_first;
  rand int unsigned alias_reads;

  // A window takes about 22 minutes at the chip's simulation speed: all 48 in one run took ~17 h
  // and was killed at the 480-minute limit (regr-xlm 2026-10-25, 22 windows done). So a run
  // checks 4-12 windows (each run a random subset, shuffled by the seed) and the regression runs
  // several seeds (chip_smoketests.hjson reseed), together about every window. 2026-10-26.
  constraint n_windows_c {
    n_windows inside {[4:12]};
    n_windows <= NumWindows;
  }

  constraint sizes_c {
    soft max_regs == 1024;
    max_regs inside {[1:1024]};
    soft n_patterns == 2;
    n_patterns inside {[1:8]};
    soft alias_reads == 8;
    alias_reads inside {[0:64]};
  }

  // The default weights, +-50 % per seed.
  constraint w_op_c {
    w_op_reset inside {[8:22]};
    w_op_write inside {[18:52]};
    w_op_walk inside {[8:22]};
    w_op_alias inside {[8:22]};
    w_op_regwen inside {[5:15]};
    w_op_shadow inside {[5:15]};
  }

  // The weight rule (EG2 spec, "Constraints in SystemVerilog").
  constraint w_data_c {
    solve boost, boosted before w_data;
    boost dist {1'b1 := 1, 1'b0 := 4};
    boosted < DataNum;
    foreach (w_data[i]) w_data[i] inside {[1:100]};
    if (boost) {
      w_data[boosted] * 2 == w_data.sum();
    } else {
      (w_data.sum() with (DataPlain[item.index] ? 0 : item)) * 10 >= w_data.sum() * 6;
      (w_data.sum() with (DataPlain[item.index] ? item : 0)) * 10 <= w_data.sum() * 3;
      (w_data.sum() with (DataPlain[item.index] ? item : 0)) * 20 >= w_data.sum();
    }
  }

  constraint w_width_c {
    soft w_width_word == 70;
    soft w_width_half == 15;
    soft w_width_byte == 15;
    w_width_word <= 100;
    w_width_half <= 100;
    w_width_byte <= 100;
  }

  constraint order_c {
    order inside {[0:2]};
    reset_read_first dist {1'b0 := 30, 1'b1 := 70};
  }

  virtual function void write_params();
    set_param("n_windows", n_windows);
    set_param("max_regs", max_regs);
    set_param("n_patterns", n_patterns);
    set_param("w_op_reset", w_op_reset);
    set_param("w_op_write", w_op_write);
    set_param("w_op_walk", w_op_walk);
    set_param("w_op_alias", w_op_alias);
    set_param("w_op_regwen", w_op_regwen);
    set_param("w_op_shadow", w_op_shadow);
    set_param("w_data_zeros", w_data[DataZeros]);
    set_param("w_data_ones", w_data[DataOnes]);
    set_param("w_data_walk", w_data[DataWalk]);
    set_param("w_data_max", w_data[DataMax]);
    set_param("w_data_rand", w_data[DataRand]);
    set_param("w_width_word", w_width_word);
    set_param("w_width_half", w_width_half);
    set_param("w_width_byte", w_width_byte);
    set_param("order", order);
    set_param("reset_read_first", reset_read_first);
    set_param("alias_reads", alias_reads);
    // model_gaps stays 0 on the chip: every check is made.
  endfunction

endclass : chip_sw_eg2_rand_regs_vseq
