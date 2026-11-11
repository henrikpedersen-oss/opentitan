// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Combine bitmap revocation, d_error and bad data integrity on ONE external
// bitmap response. Isolated one-capability sweeps leave attribution unambiguous.
// Requirements: REQ_TBRE_32–36; REQ_ALT_04.
class cheriot_mem_sys_revocation_causes_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_revocation_causes_vseq)
  `uvm_object_new

  virtual task body();
    bit [2:0] causes[4] = '{3'b011, 3'b101, 3'b110, 3'b111};
    bit [31:0] w0, w1;
    bit tag;
    foreach (causes[i]) begin
      int before_injections;
      bit [31:0] g = MainSramBase + 32'h800;
      cheriot_on();
      revbm_set_base(MainSramBase, causes[i][2]);
      make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
      core_store_cap(g, w0, w1, 1);
      wait_quiet();
      before_injections = m_meta_seq.n_inj_applied;
      m_meta_seq.inject_combined({causes[i][0], 1'b0, causes[i][1]},
                                MetaRevbmBase, MetaNvmTagBase, InjReads, 1, 1);
      tbre_sweep(g, 1);
      wait_quiet();
      `DV_CHECK_EQ(m_meta_seq.n_inj_applied, before_injections + 1)
      core_load_cap(g, tag);
      `DV_CHECK_EQ(tag, 0, "each combined cause revokes this isolated capability")
      do_reset();
    end
  endtask
endclass
