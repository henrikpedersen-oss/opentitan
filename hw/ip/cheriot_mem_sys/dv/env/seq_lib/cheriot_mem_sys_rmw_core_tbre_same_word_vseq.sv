// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The core's and the revocation engine's tag filters share one RMW filter. It loses no update when
// both write bits of the same meta word at the same time, and it takes a request in the cycle a
// read is answered, so the two requesters' lookups follow each other at one per cycle
// (theory_of_operation.md, "RMW Filter").
// Requirements: REQ_RMW_03–11, REQ_RMW_18; REQ_ARB_01–11; REQ_TAG_04.
class cheriot_mem_sys_rmw_core_tbre_same_word_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_rmw_core_tbre_same_word_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] blk, cb, w0, w1;
    bit        sweep_done;
    cheriot_on();
    for (int unsigned rep = 0; rep < 8; rep++) begin
      randomize_timing(1'(rep % 2));
      blk = MainSramBase + 256 * (1 + $urandom_range(SramCaps / 32 - 2));
      // The lower half of the word's granules: tagged, revoked
      for (int unsigned i = 0; i < 16; i++) begin
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        core_store_cap(blk + 8 * i, w0, w1, 1'b1);
      end
      wait_quiet();
      // The engine sweeps the lower half while the core stores into the upper half (bases in the
      // NVM: never revoked)
      sweep_done = 0;
      fork
        begin
          tbre_sweep(blk, 16);
          sweep_done = 1;
        end
        while (!sweep_done) begin
          make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
          core_store_cap(blk + 128 + 8 * $urandom_range(15), w0, w1, $urandom_range(3) != 0);
          throttle(2);
        end
      join
      wait_quiet();
    end
  endtask

endclass
