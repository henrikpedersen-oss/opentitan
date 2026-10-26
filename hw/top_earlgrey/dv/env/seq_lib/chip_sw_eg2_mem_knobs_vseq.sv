// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The knobs of the memory-area tests that need no bench action of their own
// (sw/device/tests/eg2_mem): chip_sw_sram_readback, chip_sw_otbn_isa, chip_sw_otbn_keymgr.

// chip_sw_sram_readback: which test-owned SRAMs, how many words, where.
class chip_sw_eg2_sram_readback_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_readback_vseq)
  `uvm_object_new

  rand int unsigned mems, n_words, window;
  rand bit init_check;
  constraint knobs_c {
    mems dist {3 := 60, 1 := 20, 2 := 20};
    n_words dist {[1:4] :/ 20, [5:64] :/ 60, [65:256] :/ 20};
    window dist {0 := 25, 1 := 25, 2 := 50};
    init_check dist {1 := 80, 0 := 20};
  }

  virtual function void write_params();
    set_param("mems", mems);
    set_param("n_words", n_words);
    set_param("window", window);
    set_param("init_check", init_check);
  endfunction
endclass : chip_sw_eg2_sram_readback_vseq

// chip_sw_otbn_isa: a DMEM secure wipe before each load, or not.
class chip_sw_eg2_otbn_isa_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_otbn_isa_vseq)
  `uvm_object_new

  rand bit wipe_dmem;
  constraint knobs_c {
    wipe_dmem dist {1 := 50, 0 := 50};
  }

  virtual function void write_params();
    set_param("wipe_dmem", wipe_dmem);
  endfunction
endclass : chip_sw_eg2_otbn_isa_vseq

// chip_sw_otbn_keymgr: the key version (0 to the CreatorRootKey's maximum, 0x11) and the salt word
// the "different salt" step changes.
class chip_sw_eg2_otbn_keymgr_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_otbn_keymgr_vseq)
  `uvm_object_new

  rand int unsigned version, salt_flip;
  constraint knobs_c {
    version dist {0 := 30, 'h11 := 30, [1:'h10] :/ 40};
    salt_flip inside {[0:7]};
  }

  virtual function void write_params();
    set_param("version", version);
    set_param("salt_flip", salt_flip);
  endfunction
endclass : chip_sw_eg2_otbn_keymgr_vseq
