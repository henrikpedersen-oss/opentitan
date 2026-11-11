// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// CHERIoT boot locks SRAM CTRL (REQ_SOC_08). Software checks that attempted INIT and renewal
// preserve data and status. No key/backdoor stimulus is needed for a refused command; the
// RISC-V scrambled-access sequence retains the active key-derivation and scrambling checks.
class chip_sw_eg2_sram_scrambled_access_cheriot_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_scrambled_access_cheriot_vseq)
  `uvm_object_new

  rand int unsigned n_words;
  rand int unsigned rekey;
  rand bit [31:0] bd_seed;
  constraint n_words_c { n_words inside {[16:1024]}; }
  constraint rekey_c { rekey inside {[0:4]}; }

  virtual function void write_params();
    set_param("n_words", n_words);
    set_param("rekey", rekey);
    set_param("bd_seed", bd_seed);
  endfunction
endclass
