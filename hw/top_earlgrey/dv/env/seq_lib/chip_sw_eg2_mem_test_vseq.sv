// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The knobs of chip_sw_eg2_mem_test (sw/device/tests/eg2_regmem/eg2_mem_test.c; EG2 verification
// spec "Registers and memories" and "Registers and memories in CHERIoT mode"), drawn under the
// spec's distributions. The software draws each memory's algorithm, width, range and data from
// its seed within them.
class chip_sw_eg2_mem_test_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_mem_test_vseq)

  `uvm_object_new

  // EG2_RM_MEM_COUNT of the generated eg2_regmem_tables.h: every memory the core reaches.
  localparam int unsigned NumMems = 9;

  rand int unsigned n_mems;
  rand int unsigned w_mem_main, w_mem_sec, w_mem_ret, w_mem_rram, w_mem_otbn, w_mem_buf,
                    w_mem_meta, w_mem_rom;
  rand int unsigned w_alg_march_c, w_alg_mats, w_alg_checker, w_alg_walk_addr, w_alg_addr;
  rand int unsigned w_width_word, w_width_half, w_width_byte, w_width_cap;
  rand int unsigned range_class;
  rand int unsigned window_bytes;
  rand bit          direction;
  rand bit          tag_march;
  rand int unsigned int_over_cap;

  constraint n_mems_c {
    solve range_class before n_mems;
    soft n_mems == NumMems;
    n_mems inside {[1:NumMems]};
    // A whole memory takes up to a million accesses: at most two of them in one chip run.
    (range_class == 0) -> n_mems inside {[1:2]};
  }

  // The default weights, +-50 % per seed.
  constraint w_mem_c {
    w_mem_main inside {[13:37]};
    w_mem_sec inside {[8:22]};
    w_mem_ret inside {[5:15]};
    w_mem_rram inside {[8:22]};
    w_mem_otbn inside {[5:15]};
    w_mem_buf inside {[5:15]};
    w_mem_meta inside {[3:7]};
    w_mem_rom inside {[5:15]};
  }

  constraint w_alg_c {
    w_alg_march_c inside {[15:45]};
    w_alg_mats inside {[10:30]};
    w_alg_checker inside {[10:30]};
    w_alg_walk_addr inside {[15:45]};
    w_alg_addr inside {[10:30]};
  }

  // 60 : 20 : 20, and in CHERIoT mode the capability width beside them.
  constraint w_width_c {
    soft w_width_word == 60;
    soft w_width_half == 20;
    soft w_width_byte == 20;
    soft w_width_cap == 20;
    w_width_word <= 100;
    w_width_half <= 100;
    w_width_byte <= 100;
    w_width_cap <= 100;
  }

  // The whole testable part, its first 1 KiB, its last 1 KiB, a random window.
  constraint range_c {
    range_class dist {0 := 20, 1 := 15, 2 := 15, 3 := 50};
    window_bytes dist {[64:1024] :/ 70, [1025:16384] :/ 30};
  }

  constraint cheriot_c {
    tag_march dist {1'b0 := 30, 1'b1 := 70};
    int_over_cap inside {[0:30]};
  }

  virtual function void write_params();
    set_param("n_mems", n_mems);
    set_param("w_mem_main", w_mem_main);
    set_param("w_mem_sec", w_mem_sec);
    set_param("w_mem_ret", w_mem_ret);
    set_param("w_mem_rram", w_mem_rram);
    set_param("w_mem_otbn", w_mem_otbn);
    set_param("w_mem_buf", w_mem_buf);
    set_param("w_mem_meta", w_mem_meta);
    set_param("w_mem_rom", w_mem_rom);
    set_param("w_alg_march_c", w_alg_march_c);
    set_param("w_alg_mats", w_alg_mats);
    set_param("w_alg_checker", w_alg_checker);
    set_param("w_alg_walk_addr", w_alg_walk_addr);
    set_param("w_alg_addr", w_alg_addr);
    set_param("w_width_word", w_width_word);
    set_param("w_width_half", w_width_half);
    set_param("w_width_byte", w_width_byte);
    set_param("w_width_cap", w_width_cap);
    set_param("range_class", range_class);
    set_param("window_bytes", window_bytes);
    set_param("direction", direction);
    set_param("tag_march", tag_march);
    set_param("int_over_cap", int_over_cap);
    // model_gaps stays 0 on the chip: every check is made.
  endfunction

endclass : chip_sw_eg2_mem_test_vseq
