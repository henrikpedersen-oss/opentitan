// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Every non-capability write clears the tag of the granule it touches and of no other: a byte at
// each offset, a halfword at each offset, a word, a partial word write with any mask
// (theory_of_operation.md, "Tag Filter": writes always look up; cheriot_mem_sys.hjson
// CHERIOT.TAG_CLEAR_ON_DATA_WRITE). A store with tag 1 on one word only is trusted: the granule
// keeps tag 1, or gets it.
// Requirements: REQ_TAG_02–04; REQ_CMS_16 (block part).
class cheriot_mem_sys_tag_clear_subword_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tag_clear_subword_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] g, w0, w1;
    bit        t, e;
    cheriot_on();
    for (int unsigned k = 0; k < 24; k++) begin
      g = MainSramBase + 32'h200 + 32 * k;
      make_cap(MainSramBase + 8 * k, 0, PermsMemRw, w0, w1);
      // The granule and both neighbours tagged
      core_store_cap(g - 8, w0, w1, 1'b1);
      core_store_cap(g,     w0, w1, 1'b1);
      core_store_cap(g + 8, w0, w1, 1'b1);
      wait_quiet();
      if (k < 8) begin
        core_store(g + k, 0, 32'h5a, e);
      end else if (k < 12) begin
        core_store(g + 2 * (k - 8), 1, 32'h5a5a, e);
      end else if (k < 14) begin
        core_store(g + 4 * (k - 12), 2, 32'h5a5a_5a5a, e);
      end else if (k < 20) begin
        core_access(core_item(tlul_pkg::PutPartialData, g + 4 * (k % 2), 2, $urandom, 0,
                              4'($urandom_range(14, 1))));
      end else begin
        core_access(core_item(tlul_pkg::PutFullData, g + 4 * (k % 2), 2, $urandom, 1));
      end
      wait_quiet();
      core_load_cap(g - 8, t);
      core_load_cap(g + 8, t);
      core_load_cap(g, t);
    end
    wait_quiet();
  endtask

endclass
