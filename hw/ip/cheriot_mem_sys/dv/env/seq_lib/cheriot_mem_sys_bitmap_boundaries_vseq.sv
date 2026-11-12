// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Non-power-of-two bitmap window: last real granule, first rounded-only granule,
// last rounded granule, first beyond the rounded window, and below the heap.
// Run on the default EG2 map (3072 bitmap bytes), not cms_map. Independently
// decoded capability bases select the cases; the model checks tag retention.
// Requirements: REQ_TBRE_18–27; REQ_CMS_09–11.
class cheriot_mem_sys_bitmap_boundaries_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_bitmap_boundaries_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] offsets[5] = '{196600, 196608, 262136, 262144, 32'hffff_fff8};
    bit [31:0] w0, w1;
    bit tag;
    `DV_CHECK_EQ_FATAL(RevbmBytes, 3072, "bitmap_boundaries requires the default EG2 map")
    cheriot_on();
    revbm_set_base(MainSramTop - 8, 1);
    foreach (offsets[i]) begin
      bit [31:0] g = MainSramBase + 32'h800 + 8 * i;
      bit [31:0] base = MainSramBase + offsets[i];
      make_kind_cap(base, CapMemRw, 0, w0, w1);
      `DV_CHECK_EQ(cap_base(w0, w1), base)
      core_store_cap(g, w0, w1, 1);
      wait_quiet();
      tbre_sweep(g, 1);
      core_load_cap(g, tag);
      `DV_CHECK_EQ(tag, (i != 0), "only the last real bitmap granule is revoked")
    end
    wait_quiet();
  endtask
endclass
