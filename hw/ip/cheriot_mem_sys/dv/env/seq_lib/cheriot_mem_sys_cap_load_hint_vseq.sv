// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Only a hinted, 64-bit-aligned load in CHERIoT mode to tagged memory looks its tag up; the second
// word of a capability carries the first word's tag, and only read responses take part, so a write
// answered in between is untagged and leaves the captured tag alone; two outstanding accesses work
// under back-pressure (theory_of_operation.md, "Tag Filter").
// Requirements: REQ_TF_02, REQ_TF_08–11; REQ_TAG_01.
class cheriot_mem_sys_cap_load_hint_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_cap_load_hint_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] w0, w1, d;
    bit        t, e;
    cheriot_on();
    for (int unsigned i = 0; i < 64; i++) begin
      make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
      core_store_cap(MainSramBase + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    for (int unsigned pass = 0; pass < 3; pass++) begin
      randomize_timing(pass == 0);
      // Capability loads back to back, both words outstanding, with untagged loads between them
      for (int unsigned i = 0; i < 40; i++) begin
        core_issue_load_cap(MainSramBase + 8 * $urandom_range(63));
        if (rbit()) core_issue(core_item(tlul_pkg::Get, MainSramBase + 8 * $urandom_range(63)));
        if (rbit()) core_issue(core_item(tlul_pkg::Get, MainSramBase + $urandom_range(511), 0));
        if ($urandom_range(3) == 0) begin
          core_issue(core_item(tlul_pkg::Get, MainSramBase + 2 * $urandom_range(255), 1));
        end
        throttle(4);
      end
      wait_quiet();
    end
    // A hinted read of the first word alone, and of untagged memory
    core_load(MainSramBase, 2, 1'b1, d, t, e);
    core_load(UntaggedBase, 2, 1'b1, d, t, e);
    core_load_cap(UntaggedBase + 8, t);
    wait_quiet();
  endtask

endclass
