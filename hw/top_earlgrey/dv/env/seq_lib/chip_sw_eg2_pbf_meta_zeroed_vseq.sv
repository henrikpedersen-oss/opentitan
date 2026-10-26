// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_meta_zeroed_sb (eg2_top_verification spec REQ_PBF_14, REQ_CMS_15; software
// sw/device/tests/eg2_pbf/eg2_pbf_meta_zeroed_sb.c): the meta SRAM all zero before the first
// capability store. The bench draws the knobs and confirms the bitmap window was read.
class chip_sw_eg2_pbf_meta_zeroed_sb_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_meta_zeroed_sb_vseq)
  `uvm_object_new

  rand bit [1:0]  read_order;     // 0 ascending, 1 descending, 2 a permutation
  rand bit [31:0] first_granule;  // in the free main SRAM area (12288 granules)
  rand bit [31:0] nvm_pages;

  constraint knobs_c {
    read_order inside {[0 : 2]};
    first_granule < 12288;
    nvm_pages inside {[0 : 16]};
  }

  virtual function void write_params();
    set_param("read_order", read_order);
    set_param("first_granule", first_granule);
    set_param("nvm_pages", nvm_pages);
  endfunction

  virtual task body();
    // Every word of the revbm window is read (in RISC-V mode each read answers an error).
    add_activity("revbm window accesses", {PdMain, ".u_cheriot_mem_sys.revbm_tl_d_i"}, 2);
    super.body();
  endtask
endclass
