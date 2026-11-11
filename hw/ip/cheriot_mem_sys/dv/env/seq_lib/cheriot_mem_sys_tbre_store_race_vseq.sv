// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Rewrite a revoked capability with a live one 0..23 cycles after each externally observed
// TBRE word request or response. Check the final live tag and the complete meta SRAM through
// the independent model. No internal snoop or routing signal chooses the expected outcome.
// Requirements: REQ_TBRE_24–27, REQ_TBRE_39; REQ_TAG_09.
class cheriot_mem_sys_tbre_store_race_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_store_race_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] g, cb, w0, w1, f0, f1;
    cheriot_on();
    randomize_timing(1);
    for (int response = 0; response < 2; response++) begin
      for (int word = 0; word < 2; word++) begin
        for (int delay = 0; delay < 24; delay++) begin
          bit saw_read = 0;
          bit started = 0;
          bit tag;
          g = MainSramBase + 8 * (64 + 48 * response + 24 * word + delay);
          heap_base(1, cb);
          make_cap(cb, 0, PermsMemRw, w0, w1);
          core_store_cap(g, w0, w1, 1);
          make_cap(NvmBase + 8 * delay, 0, PermsMemRw, f0, f1);
          wait_quiet();
          // Arm the external-port observer before START: the first read may precede its ack.
          fork
            begin
              bit [top_pkg::TL_AIW-1:0] source;
              do @(cfg.m_tbre_h_agent_cfg.vif.mon_cb);
              while (!(cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_valid &&
                        cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_address == g + 4 * word));
              source = cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_source;
              if (response) begin
                do @(cfg.m_tbre_h_agent_cfg.vif.mon_cb);
                while (!(cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.d_valid &&
                          cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.d_source == source &&
                          cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.d_ready));
              end
              saw_read = 1;
              wait_cycles(delay);
              core_store_cap(g, f0, f1, 1);
            end
            begin tbre_sweep(g, 1, 0); started = 1; end
            begin
              for (int timeout = 0; timeout < 2000 && !saw_read; timeout++) wait_cycles(1);
              if (!saw_read) `uvm_fatal(`gfn, "store race: no external TBRE read observed")
            end
          join

          tbre_wait_idle();
          wait_quiet();
          cfg.race_complete = 1;
          core_load_cap(g, tag);
          `DV_CHECK_EQ(tag, 1, "a live racing store lost its tag")
        end
      end
    end
  endtask
endclass
