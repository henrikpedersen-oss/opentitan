// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A response integrity or data integrity fault on a meta SRAM response to the RMW filter raises
// fatal_fault, on a tag lookup and on a tag fill (theory_of_operation.md, "RMW Filter" and "Error
// Handling").
class cheriot_mem_sys_err_meta_intg_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_meta_intg_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]  g, ta, d;
    bit         t, e;
    tl_seq_item rsp;
    cheriot_on();
    for (int unsigned c = 0; c < 4; c++) begin
      g  = MainSramBase + 32'h600 + 8 * c;
      ta = tag_word_addr(g);
      setup_tagged(g, 1'b1);
      m_meta_seq.inject((c % 2) ? InjDataIntg : InjRspIntg, ta, ta + 4, InjReads);
      if (c < 2) core_load(g, 2, 1'b1, d, t, e);                                  // a lookup
      else       core_access(core_item(tlul_pkg::PutFullData, g + 4, 2, 32'h0));  // a fill
      wait_quiet();
      do_reset();
    end
    // An integrity fault on a response to the software bitmap window: the subsystem passes it to
    // the requester; whether it also raises fatal_fault, the documents do not say (the reference
    // model allows it).
    m_meta_seq.inject(InjRspIntg, MetaRevbmBase, MetaNvmTagBase, InjReads);
    bitmap_access(0, tlul_pkg::Get, MetaRevbmBase, 2, 0, 0, rsp);
    do_reset();
  endtask

endclass
