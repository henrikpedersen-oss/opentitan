// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Errors on the data path (cored_tl_h) reach the core as d_error, raise no alert, and do not stop
// the tag update: a failed data write still updates the tag (theory_of_operation.md, "Tag
// Filter").
class cheriot_mem_sys_err_data_path_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_data_path_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] g, d;
    bit        t, e;
    cheriot_on();
    for (int unsigned c = 0; c < 4; c++) begin
      g = MainSramBase + 32'ha00 + 8 * c;
      setup_tagged(g, 1'(c % 2));
      // An error on the crossbar side only: the C model expects d_error from the crossbar's answer
      // and the tag updated exactly as the store's tag says.
      m_cored_h_seq.inject(InjErr, g, g + 8, InjAny);
      // Cases 0 and 1: a store (tag 1 over tag 0, tag 0 over tag 1); 2 and 3: a capability load
      if (c < 2) core_access(core_item(tlul_pkg::PutFullData, g, 2, $urandom, !1'(c % 2)));
      else       core_load(g, 2, 1'b1, d, t, e);
      wait_quiet();
    end
    // The crossbar's own error range
    core_load(DataErrBase, 2, 1'b0, d, t, e);
    core_store(DataErrBase + 4, 2, 32'h0, e);
    core_load_cap(MainSramBase + 32'ha00, t);
    core_load_cap(MainSramBase + 32'ha08, t);
    wait_cycles(200);
    wait_quiet();
  endtask

endclass
