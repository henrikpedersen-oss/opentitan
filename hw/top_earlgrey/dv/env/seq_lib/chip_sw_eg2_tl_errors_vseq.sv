// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_eg2_tl_errors[_cheriot] (EG2 spec REQ_TL_03, "TileLink in RISC-V mode"; software
// sw/device/tests/eg2_tl/eg2_tl_errors.c): the knobs, drawn under the spec's classes. The
// software makes every error case and checks it; the bench draws how many, which mix, in which
// order, at which widths and, in CHERIoT mode, through which authority.
class chip_sw_eg2_tl_errors_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_tl_errors_vseq)
  `uvm_object_new

  rand int unsigned n_cases;
  rand int unsigned w_case[8];  // gap, in_window, subword_write, misaligned, subword_read,
                                // rom_write, rram_write, byte_reg
  rand int unsigned w_width_byte, w_width_half, w_width_word;
  rand bit          order;
  rand int unsigned authority;
  rand bit          boost;
  rand int unsigned boosted;

  constraint n_cases_c {
    n_cases dist {[8:32] :/ 30, [33:96] :/ 60, [97:256] :/ 10};
  }

  // Every case reachable; one run in five gives one case half of the total (the weight rule).
  constraint w_case_c {
    solve boost, boosted before w_case;
    boost dist {1'b1 := 1, 1'b0 := 4};
    boosted < 8;
    foreach (w_case[i]) w_case[i] inside {[1:100]};
    if (boost) {
      w_case[boosted] * 2 == w_case.sum();
    }
  }

  constraint w_width_c {
    w_width_byte inside {[5:100]};
    w_width_half inside {[5:100]};
    w_width_word inside {[5:100]};
  }

  constraint order_authority_c {
    order dist {1'b0 := 30, 1'b1 := 70};
    authority dist {0 := 25, 1 := 25, 2 := 25, 3 := 25};
  }

  virtual function void write_params();
    set_param("n_cases", n_cases);
    set_param("w_case_gap", w_case[0]);
    set_param("w_case_in_window", w_case[1]);
    set_param("w_case_subword_write", w_case[2]);
    set_param("w_case_misaligned", w_case[3]);
    set_param("w_case_subword_read", w_case[4]);
    set_param("w_case_rom_write", w_case[5]);
    set_param("w_case_rram_write", w_case[6]);
    set_param("w_case_byte_reg", w_case[7]);
    set_param("w_width_byte", w_width_byte);
    set_param("w_width_half", w_width_half);
    set_param("w_width_word", w_width_word);
    set_param("order", order);
    set_param("authority", authority);
    // model_gaps stays 0 on the chip: every check is made.
  endfunction

endclass : chip_sw_eg2_tl_errors_vseq
