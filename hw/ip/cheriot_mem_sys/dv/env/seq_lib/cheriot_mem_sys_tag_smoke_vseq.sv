// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The subsystem comes out of reset with its reset values and quiet in ePMP mode (no meta SRAM
// access, no tag); in CHERIoT mode a capability store sets a tag, a capability load returns it, and
// a data store clears it.
class cheriot_mem_sys_tag_smoke_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tag_smoke_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] w0, w1;
    bit        t, e;
    csr_rd(.ptr(ral.tbre_regwen), .value(d));
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_rd(.ptr(ral.intr_state), .value(d));
    csr_rd(.ptr(ral.intr_enable), .value(d));
    csr_rd(.ptr(ral.tbre_base_addr), .value(d));
    csr_rd(.ptr(ral.tbre_num_caps), .value(d));

    // ePMP mode: no meta SRAM access, no tag
    make_cap(MainSramBase + 32'h100, 0, PermsMemRw, w0, w1);
    core_store_cap(MainSramBase, w0, w1, 1'b1);
    core_load_cap(MainSramBase, t);
    wait_quiet();

    // CHERIoT mode: a tag stored, loaded, and cleared by a data store
    cheriot_on();
    core_store_cap(MainSramBase + 8, w0, w1, 1'b1);
    wait_quiet();
    core_load_cap(MainSramBase + 8, t);
    core_store(MainSramBase + 12, 2, 32'h1234_5678, e);
    core_load_cap(MainSramBase + 8, t);
  endtask

endclass
