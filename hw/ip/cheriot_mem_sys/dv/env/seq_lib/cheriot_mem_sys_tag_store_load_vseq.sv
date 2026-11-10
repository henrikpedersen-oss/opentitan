// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A capability store sets the tag of its 8-byte granule to the store's tag and a capability load
// returns it, in both tagged regions, at their boundaries and at random granules; untagged memory
// has no tag and never reaches the meta SRAM (theory_of_operation.md, "Tag Filter"). In the NVM a
// tagged store sets the tag only when the NVM holds the capability: half of the stores are
// programmed there first, the others are answered with d_error.
class cheriot_mem_sys_tag_store_load_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tag_store_load_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   base[3], top[3], a, w0, w1;
    bit          t;
    int unsigned meta0;
    cheriot_on();
    randomize_timing(0);
    base = '{MainSramBase, NvmBase, UntaggedBase};
    top  = '{MainSramTop, NvmTop, DataErrBase};
    foreach (base[r]) begin
      meta0 = m_meta_seq.n_reads + m_meta_seq.n_writes;
      for (int unsigned i = 0; i < 80; i++) begin
        a = (i == 0) ? base[r] : (i == 1) ? top[r] - 8 : rand_granule(base[r], top[r]);
        make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
        store_cap(a, w0, w1, rbit(), rbit() || i < 2);
        throttle();
      end
      wait_quiet();
      for (int unsigned i = 0; i < 80; i++) core_load_cap(rand_granule(base[r], top[r]), t);
      core_load_cap(base[r], t);
      core_load_cap(top[r] - 8, t);
      if (base[r] == UntaggedBase && m_meta_seq.n_reads + m_meta_seq.n_writes != meta0) begin
        `uvm_error(`gfn, "accesses to untagged memory reached the meta SRAM")
      end
    end
    // Just outside the tagged regions: no tag
    core_store_cap(MainSramTop, w0, w1, 1'b1);
    core_store_cap(MainSramBase - 8, w0, w1, 1'b1);
    core_load_cap(MainSramTop, t);
    core_load_cap(MainSramBase - 8, t);
    wait_quiet();
  endtask

endclass
