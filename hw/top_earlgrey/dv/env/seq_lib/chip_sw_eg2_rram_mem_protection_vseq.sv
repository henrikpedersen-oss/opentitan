// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_mem_protection (sw/device/tests/eg2_rram/eg2_rram_mem_protection.c; EG2 spec
// REQ_RRAM_14: region configuration drawn legal (base, size, permissions), 0-8 regions; the
// emulated info windows and their overlap). The test draws each region's base, size and
// attributes from its seed within the 16-page scratch window; the bench draws how many.
class chip_sw_eg2_rram_mem_protection_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_mem_protection_vseq)
  `uvm_object_new

  rand int unsigned n_regions, n_probes, dflt_attrs, info, lock, emul;

  constraint knobs_c {
    n_regions dist {0 := 10, [1:4] :/ 50, [5:8] :/ 40};
    n_probes inside {[16:64]};
    dflt_attrs dist {[0:15] :/ 30, 16 := 70};  // 16: drawn by the software
    info dist {1 := 70, 0 := 30};
    lock dist {1 := 50, 0 := 50};
    emul dist {0 := 20, 1 := 30, 2 := 30, 3 := 20};
  }

  virtual function void write_params();
    set_param("n_regions", n_regions);
    set_param("n_probes", n_probes);
    set_param("dflt_attrs", dflt_attrs);
    set_param("info", info);
    set_param("lock", lock);
    set_param("emul", emul);
  endfunction
endclass : chip_sw_eg2_rram_mem_protection_vseq
