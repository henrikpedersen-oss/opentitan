// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A tag write changes exactly the addressed bit of its 32-bit meta word, under heavy traffic to
// the 32 granules of one word; when the bit already has the value it is a read with no write-back
// (theory_of_operation.md, "RMW Filter").
// Requirements: REQ_RMW_01–11, REQ_RMW_18; REQ_TAG_04.
class cheriot_mem_sys_rmw_same_word_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_rmw_same_word_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   blk, a, w0, w1;
    bit          t;
    cheriot_on();
    for (int unsigned rep = 0; rep < 4; rep++) begin
      randomize_timing(rep < 2);
      // Three blocks in the SRAM, one in the NVM; 256 bytes share one meta word
      blk = (rep < 3) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
      for (int unsigned i = 0; i < 300; i++) begin
        a = blk + 8 * $urandom_range(31);
        case ($urandom_range(5))
          0, 1: begin
            make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
            store_cap(a, w0, w1, rbit(), rbit());
          end
          2:       core_issue(core_item(tlul_pkg::PutFullData, a + 4 * rbit(), 2, $urandom));
          3:       core_issue(core_item(tlul_pkg::PutFullData, a + $urandom_range(7), 0, $urandom));
          default: core_issue_load_cap(a);
        endcase
        throttle(rep < 2 ? 4 : 8);
      end
      wait_quiet();
    end
    // The same tag stored again (no write-back: the scoreboard checks it), then cleared
    a = MainSramBase + 32'h40;
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    core_store_cap(a, w0, w1, 1'b1);
    wait_quiet();
    core_store_cap(a, w0, w1, 1'b1);
    wait_quiet();
    core_issue(core_item(tlul_pkg::PutFullData, a, 2, 32'h0));
    wait_quiet();
    core_load_cap(a, t);
  endtask

endclass
