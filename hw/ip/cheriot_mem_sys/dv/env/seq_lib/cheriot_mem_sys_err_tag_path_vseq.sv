// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A device error on the tag path aborts the read-modify-write, answers d_error to the core and
// raises fatal_fault: on the fill read of a store (no write-back, the tag unchanged), on a
// capability load's lookup (tag 0), and on the write-back (theory_of_operation.md, "RMW Filter"
// and "Error Handling").
class cheriot_mem_sys_err_tag_path_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_tag_path_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]                g, ta;
    cheriot_mem_sys_core_item it;
    cheriot_on();
    for (int unsigned c = 0; c < 3; c++) begin
      g  = MainSramBase + 32'h400 + 8 * c;
      ta = tag_word_addr(g);
      setup_tagged(g, c != 2);
      case (c)
        0: begin  // the fill read of a store
          m_meta_seq.inject(InjErr, ta, ta + 4, InjReads);
          it = core_item(tlul_pkg::PutFullData, g, 2, 32'h1111_2222);
          core_access(it);
        end
        1: begin  // a capability load's lookup
          m_meta_seq.inject(InjErr, ta, ta + 4, InjReads);
          it = core_item(tlul_pkg::Get, g, 2, 0, 1);
          core_access(it);
        end
        default: begin  // the write-back
          m_meta_seq.inject(InjErr, ta, ta + 4, InjWrites);
          it = core_item(tlul_pkg::PutFullData, g, 2, 32'h3333_4444, 1);
          core_access(it);
        end
      endcase
      wait_quiet();
      do_reset();
    end
  endtask

endclass
