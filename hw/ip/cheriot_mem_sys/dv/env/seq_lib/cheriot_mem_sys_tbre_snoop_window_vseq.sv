// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The revocation engine watches the core's writes (theory_of_operation.md, "Revocation Engine"):
// every write the core presents, and every accepted one until it is answered, is compared at
// capability granularity with the capabilities the engine has in flight, from the presentation of
// the read of a capability's lower word. A capability written from then on is not cleared,
// including a write to its upper word only and a write of a capability that is itself revoked; a
// write answered before is resolved by the engine like any capability.
// Requirements: REQ_TBRE_24–27, REQ_TBRE_39; REQ_TAG_09.
class cheriot_mem_sys_tbre_snoop_window_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_snoop_window_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   b, g, cb, w0, w1, f0, f1;
    bit          t, nvm;
    int unsigned k, n, kept_nvm, failed_nvm;
    cheriot_on();
    for (int unsigned rep = 0; rep < 64; rep++) begin
      randomize_timing(rep % 3 == 0);
      nvm = (rep % 4 == 3);
      n   = 2 + $urandom_range(6);
      b   = nvm ? NvmBase + 8 * $urandom_range(NvmCaps - n)
                : MainSramBase + 8 * $urandom_range(SramCaps - n);
      // n revoked, tagged capabilities
      for (int unsigned i = 0; i < n; i++) begin
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        store_cap(b + 8 * i, w0, w1, 1'b1);
        throttle(2);
      end
      wait_quiet();
      // During the sweep, a write to capability k (with a revoked capability, or part of one)
      k = $urandom_range(n - 1);
      g = b + 8 * k;
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, f0, f1);
      tbre_sweep(b, n, 1'b0);
      wait_cycles($urandom_range(8 * k + 12));
      if (nvm) begin
        core_store_cap(g, f0, f1, 1'b1);  // differs from the NVM: d_error, nothing written
        failed_nvm++;
      end else begin
        case ($urandom_range(3))
          0:       core_store_cap(g, f0, f1, 1'b1);
          1:       core_issue(core_item(tlul_pkg::PutFullData, g + 4, 2, f1, 1));  // upper word
          2:       core_issue(core_item(tlul_pkg::PutFullData, g, 2, f0, 1));      // lower word
          default: core_issue(core_item(tlul_pkg::PutFullData, g + 4 * rbit(), 2, $urandom));
        endcase
      end
      tbre_wait_idle();
      wait_quiet();
      if (nvm) begin
        core_load_cap(g, t);
        if (t) kept_nvm++;
      end
    end
    // Reported, not judged here (the C model judges it, from the document): a capability store to
    // the NVM that fails writes nothing, yet as a watched write it stops the clear.
    `uvm_info(`gfn, $sformatf({"snoop window: %0d failed capability store(s) to the NVM, %0d ",
                               "revoked capability(ies) there still tagged after the sweep"},
                              failed_nvm, kept_nvm), UVM_LOW)
  endtask

endclass
