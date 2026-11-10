// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Only a strict MuBi4True on cheriot_ena_i enables the subsystem's CHERIoT behaviour. Every other
// value, MuBi4False and the 14 invalid encodings, makes the whole meta SRAM inaccessible from
// every port, issues no tag lookup and ignores TBRE_START (theory_of_operation.md, "Access
// Checkers" and "Revocation Engine").
class cheriot_mem_sys_mode_gating_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_mode_gating_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   w0, w1;
    bit          t, e;
    tl_seq_item  rsp;
    cheriot_on();
    for (int unsigned i = 0; i < 8; i++) begin
      make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
      core_store_cap(MainSramBase + 8 * i, w0, w1, 1'(i % 2));
    end
    revbm_write(0, 32'hdead_beef);
    wait_quiet();
    for (int unsigned v = 0; v < 16; v++) begin
      if (4'(v) == 4'(prim_mubi_pkg::MuBi4True)) continue;
      set_ena(prim_mubi_pkg::mubi4_t'(v));
      for (int unsigned i = 0; i < 8; i++) core_load_cap(MainSramBase + 8 * i, t);
      core_store_cap(MainSramBase + 8 * 8, w0, w1, 1'b1);  // sets no tag
      core_store(MainSramBase + 8, 2, 32'h1, e);           // clears no tag
      bitmap_access(0, tlul_pkg::Get, MetaRevbmBase, 2, 0, 0, rsp);
      bitmap_access(0, tlul_pkg::PutFullData, MetaRevbmBase + 4, 2, 32'h1, 0, rsp);
      bitmap_access(1, tlul_pkg::Get, MetaRevbmBase, 2, 0, 0, rsp);
      tbre_sweep(MainSramBase, 16);                        // ignored
      wait_quiet();
      wait_cycles(50);
    end
    cheriot_on();
    bitmap_access(0, tlul_pkg::Get, MetaRevbmBase, 2, 0, 0, rsp);
    for (int unsigned i = 0; i < 9; i++) core_load_cap(MainSramBase + 8 * i, t);
    wait_quiet();
  endtask

endclass
