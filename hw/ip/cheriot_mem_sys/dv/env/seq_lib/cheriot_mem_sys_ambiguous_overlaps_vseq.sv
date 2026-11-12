// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Bitmap writes during sealing/non-sealing sweeps, core stores around the
// external lower-word read, and a shared tag-word fault with INTR_ENABLE set.
// No requester is selected from internal arbitration; error, tag and sweep
// alternatives remain set-valued in the independent reference model.
// Requirements: REQ_TBRE_24–27, REQ_TBRE_28–36, REQ_TBRE_39; REQ_TAG_09.
class cheriot_mem_sys_ambiguous_overlaps_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_ambiguous_overlaps_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] g = MainSramBase + 32'h900;
    bit [31:0] w0, w1;
    // A bitmap write and an active sweep overlap an external meta fault. The
    // model retains either bitmap value; a later reset clears the fault latch,
    // not SRAM or the model's uncertainty about that write.
    cheriot_on();
    revbm_write(0, 0);
    make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
    core_store_cap(g, w0, w1, 1);
    wait_quiet();
    m_tbre_h_seq.min_rsp_delay = 50;
    m_tbre_h_seq.max_rsp_delay = 50;
    m_meta_seq.min_rsp_delay = 30;
    m_meta_seq.max_rsp_delay = 30;
    tbre_sweep(g, 1, 0);
    m_meta_seq.inject(InjErr, MetaRevbmBase, MetaRevbmBase + 4, InjAny);
    revbm_write(0, 1);
    tbre_wait_idle();
    do_reset();
    m_meta_seq.min_rsp_delay = 0;
    m_meta_seq.max_rsp_delay = 10;
    for (int sealing = 0; sealing < 2; sealing++) begin
      for (int after_lower = 0; after_lower < 2; after_lower++) begin
        cheriot_on();
        make_kind_cap(MainSramBase, sealing ? CapSealing : CapMemRw, 0, w0, w1);
        core_store_cap(g, w0, w1, 1);
        wait_quiet();
        m_tbre_h_seq.min_rsp_delay = 50;
        m_tbre_h_seq.max_rsp_delay = 50;
        cfg.m_tbre_h_agent_cfg.a_ready_delay_min = after_lower ? 50 : 1000;
        cfg.m_tbre_h_agent_cfg.a_ready_delay_max = after_lower ? 50 : 1000;
        fork
          tbre_sweep(g, 1, 0);
          begin
            // Observer armed before START, since the first read may precede its ack.
            do @(cfg.m_tbre_h_agent_cfg.vif.mon_cb);
            while (!(cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_valid &&
                     cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_address == g &&
                     (!after_lower || cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.a_ready)));
            core_access(core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1));
          end
        join
        tbre_wait_idle();
        wait_quiet();
        do_reset();
      end
    end
    // An aligned core lookup and the sweep can both be candidates for this
    // externally injected meta fault. INTR_ENABLE must not narrow that choice.
    begin
      cheriot_mem_sys_core_item it;
      int before_injections;
      cheriot_on();
      setup_tagged(g, 1);
      csr_wr(.ptr(ral.intr_enable), .value(1));
      wait_quiet();
      m_meta_seq.min_rsp_delay = 50;
      m_meta_seq.max_rsp_delay = 50;
      m_cored_h_seq.min_rsp_delay = 50;
      m_cored_h_seq.max_rsp_delay = 50;
      before_injections = m_meta_seq.n_inj_applied;
      m_meta_seq.inject(InjErr, tag_word_addr(g), tag_word_addr(g) + 4, InjReads);
      tbre_sweep(g, 1, 0);
      it = core_item(tlul_pkg::Get, g, 2, 0, 1);
      core_issue(it);
      tbre_wait_idle();
      wait (it.done);
      `DV_CHECK_EQ(m_meta_seq.n_inj_applied, before_injections + 1)
      wait_quiet();
      do_reset();
    end
  endtask
endclass
