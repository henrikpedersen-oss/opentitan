// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Fail the external meta SRAM WRITE that invalidates one revoked capability.
// The core is quiet, so attribution is unambiguous. Check injection activity;
// the independent model checks sweep_err, epoch, alert and final meta contents.
// This is a device error, not integrity on the mover's locally generated reply.
// Requirements: REQ_TBRE_32–36; REQ_ALT_04; REQ_TAG_07–08.
class cheriot_mem_sys_invalidation_error_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_invalidation_error_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] g = MainSramBase + 32'h800;
    bit [31:0] w0, w1, status, epoch;
    int before_injections;
    cheriot_on();
    revbm_set_base(MainSramBase, 1);
    make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
    core_store_cap(g, w0, w1, 1);
    wait_quiet();
    before_injections = m_meta_seq.n_inj_applied;
    m_meta_seq.inject(InjErr, tag_word_addr(g), tag_word_addr(g) + 4, InjWrites);
    tbre_sweep(g, 1);
    csr_rd(.ptr(ral.tbre_status), .value(status));
    csr_rd(.ptr(ral.tbre_epoch), .value(epoch));
    `DV_CHECK_EQ(m_meta_seq.n_inj_applied, before_injections + 1)
    `DV_CHECK_EQ(status[StatusSweepErr], 1)
    wait_quiet();
    do_reset();
  endtask
endclass
