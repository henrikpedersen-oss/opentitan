// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A fault on the revocation engine's bitmap lookup (an error response, a response or a data
// integrity fault) fails the sweep with sweep_err, raises fatal_fault, and revokes the capability
// whose lookup failed, live as it is (theory_of_operation.md, "Revocation Engine" and "Error
// Handling").
// Requirements: REQ_TBRE_32–36; REQ_ALT_04.
class cheriot_mem_sys_err_tbre_revbm_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_tbre_revbm_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] b, cb, w0, w1;
    inj_kind_e kinds[3] = '{InjErr, InjRspIntg, InjDataIntg};
    cheriot_on();
    foreach (kinds[c]) begin
      b = MainSramBase + 32'h900;
      // Live capabilities: only a failed lookup revokes one
      for (int unsigned i = 0; i < 4; i++) begin
        heap_base(0, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        core_store_cap(b + 8 * i, w0, w1, 1'b1);
      end
      wait_quiet();
      // The engine's lookup of the first capability is the next bitmap read
      m_meta_seq.inject(kinds[c], MetaRevbmBase, MetaNvmTagBase, InjReads);
      tbre_sweep(b, 4, 1'b0);
      tbre_wait_idle();
      clear_sweep_err();
      wait_quiet();
      do_reset();
    end
  endtask

endclass
