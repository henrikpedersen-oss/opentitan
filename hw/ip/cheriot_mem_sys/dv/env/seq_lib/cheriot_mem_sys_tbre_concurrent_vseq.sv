// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Core traffic and bitmap writes during a sweep are served correctly and do not disturb it
// (theory_of_operation.md, "Revocation Engine" and "Arbitration").
// Requirements: REQ_TBRE_24–27, REQ_TBRE_39; REQ_TAG_09.
class cheriot_mem_sys_tbre_concurrent_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_concurrent_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   b, a, w0, w1, cb;
    int unsigned n;
    bit          sweep_done;
    cheriot_on();
    for (int unsigned rep = 0; rep < 4; rep++) begin
      randomize_timing(1'(rep % 2));
      n = SramCaps / 4;
      b = MainSramBase + 8 * $urandom_range(SramCaps - n);
      fill_caps(b, n);
      sweep_done = 0;
      fork
        begin
          tbre_sweep(b, n);
          sweep_done = 1;
        end
        while (!sweep_done) begin
          a = rand_granule(MainSramBase, MainSramTop);
          case ($urandom_range(7))
            0, 1: core_issue_load_cap(a);
            2: begin
              // A live capability (base in the NVM): it must keep its tag
              make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
              core_store_cap(a, w0, w1, 1'b1);
            end
            3: core_issue(core_item(tlul_pkg::PutFullData, a + $urandom_range(7), 0, $urandom));
            4: core_issue(core_item(tlul_pkg::Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2, 0,
                                    1));
            5: heap_base(rbit(), cb);  // a bitmap write
            default: core_issue(core_item(tlul_pkg::Get, UntaggedBase + 4 * $urandom_range(255)));
          endcase
          throttle(4);
        end
      join
      wait_quiet();
    end
  endtask

endclass
