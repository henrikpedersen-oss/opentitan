// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Measure added latency from first external core request presentation to response handshake,
// minus first external crossbar request presentation to response-valid. Exercise reads, subword
// operations and two-word capabilities with changed/unchanged tags in CHERIoT, ePMP and invalid
// modes. REQ_CMS_07 allows zero extra cycles except at most two per tag-changing word.
// The existing extra cycle on unchanged capability-store W1 is a known gap, not a pass.
// Requirements: REQ_CMS_06, REQ_CMS_07; REQ_TF_16a–b.
class cheriot_mem_sys_latency_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_latency_vseq)
  `uvm_object_new

  int unsigned latency_mismatches;

  protected task fast_memories();
    cfg.m_core_agent_cfg.a_valid_delay_max = 0;
    cfg.m_core_agent_cfg.d_ready_delay_max = 0;
    cfg.m_cored_h_agent_cfg.a_ready_delay_min = 0;
    cfg.m_cored_h_agent_cfg.a_ready_delay_max = 0;
    cfg.m_meta_agent_cfg.a_ready_delay_min = 0;
    cfg.m_meta_agent_cfg.a_ready_delay_max = 0;
    m_cored_h_seq.min_rsp_delay = 0;
    m_cored_h_seq.max_rsp_delay = 0;
    m_meta_seq.min_rsp_delay = 0;
    m_meta_seq.max_rsp_delay = 0;
  endtask

  protected task measure(cheriot_mem_sys_core_item items[$], bit changes[$], bit cheriot);
    longint unsigned cp[$], cd[$], mp[$], md[$], cq[$], mq[$];
    bit done = 0;
    longint unsigned cycle = 0, core_first, mem_first, mem_valid;
    bit core_present = 0, mem_present = 0, mem_rsp_seen = 0;
    fork
      begin
        foreach (items[i]) core_issue(items[i]);
        wait_quiet();
        done = 1;
      end
      begin
        while (!done) begin
          @(cfg.m_core_agent_cfg.vif.mon_cb);
          cycle++;
          if (cfg.m_core_agent_cfg.vif.mon_cb.d2h.d_valid &&
              cfg.m_core_agent_cfg.vif.mon_cb.h2d.d_ready && cq.size()) begin
            cp.push_back(cq.pop_front()); cd.push_back(cycle);
          end
          if (cfg.m_core_agent_cfg.vif.mon_cb.h2d.a_valid && !core_present) begin
            core_present = 1; core_first = cycle;
          end
          if (cfg.m_core_agent_cfg.vif.mon_cb.h2d.a_valid &&
              cfg.m_core_agent_cfg.vif.mon_cb.d2h.a_ready) begin
            cq.push_back(core_first); core_present = 0;
          end
          if (cfg.m_cored_h_agent_cfg.vif.mon_cb.d2h.d_valid && !mem_rsp_seen && mq.size()) begin
            mem_rsp_seen = 1; mem_valid = cycle;
          end
          if (cfg.m_cored_h_agent_cfg.vif.mon_cb.d2h.d_valid &&
              cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.d_ready && mq.size()) begin
            mp.push_back(mq.pop_front()); md.push_back(mem_valid); mem_rsp_seen = 0;
          end
          if (cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_valid && !mem_present) begin
            mem_present = 1; mem_first = cycle;
            if ((cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_address &
                 ((1 << cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_size) - 1)) != 0)
              `uvm_info("CMS_LATENCY_PORT", $sformatf("unaligned host request addr=%08x size=%0d",
                        cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_address,
                        cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_size), UVM_LOW)
          end
          if (cfg.m_cored_h_agent_cfg.vif.mon_cb.h2d.a_valid &&
              cfg.m_cored_h_agent_cfg.vif.mon_cb.d2h.a_ready) begin
            mq.push_back(mem_first); mem_present = 0;
          end
        end
      end
    join
    `DV_CHECK_EQ(cd.size(), items.size(), "latency core observation count")
    `DV_CHECK_EQ(md.size(), items.size(), "latency crossbar observation count")
    if (cd.size() != items.size() || md.size() != items.size()) return;
    foreach (items[i]) begin
      int added = int'(cd[i] - cp[i]) - int'(md[i] - mp[i]);
      int bound = cheriot && changes[i] ? 2 : 0;
      if (corrupt_check("latency")) begin
        added = bound + 1;
        `uvm_info("CMS_CORRUPT", "latency: measured added cycles deliberately corrupted", UVM_LOW)
        `DV_CHECK_LE(added, bound, "CMS_CORRUPT_LATENCY")
      end else if (added < 0 || added > bound) begin
        latency_mismatches++;
        `uvm_info("CMS_LATENCY_MISMATCH", $sformatf(
          "%s addr=%08x tag_change=%0b: added %0d cycles, documented bound 0..%0d",
          items[i].opcode.name(), items[i].addr, changes[i], added, bound), UVM_LOW)
      end
    end
  endtask

  virtual task body();
    bit [31:0] g, w0, w1;
    prim_mubi_pkg::mubi4_t modes[3] = '{prim_mubi_pkg::MuBi4True,
                                       prim_mubi_pkg::MuBi4False, prim_mubi_pkg::mubi4_t'(0)};
    fast_memories();
    foreach (modes[m]) begin
      for (int cls = 0; cls < 6; cls++) begin
        for (int pre = 0; pre < 2; pre++) begin
          for (int tag = 0; tag < (cls == 5 ? 2 : 1); tag++) begin
            for (int rep = 0; rep < 2; rep++) begin
              cheriot_mem_sys_core_item items[$];
              bit changes[$];
              bit current_tag = 1'(pre);
              g = rand_granule(MainSramBase, MainSramTop);
              cheriot_on();
              make_cap(NvmBase, 0, PermsMemRw, w0, w1);
              core_store_cap(g, w0, w1, 1'(pre));
              wait_quiet();
              set_ena(modes[m]);
              case (cls)
                0: items.push_back(core_item(tlul_pkg::Get, g));
                1: items.push_back(core_item(tlul_pkg::Get,
                       g + (rep ? 2 * $urandom_range(1) : $urandom_range(3)), 2'(rep)));
                2: begin
                  items.push_back(core_item(tlul_pkg::Get, g, 2, 0, 1));
                  items.push_back(core_item(tlul_pkg::Get, g+4, 2, 0, 1));
                end
                3: items.push_back(core_item(tlul_pkg::PutFullData, g, 2, $urandom));
                4: items.push_back(core_item(rep ? tlul_pkg::PutPartialData : tlul_pkg::PutFullData,
                                             g, rep ? 2 : 0, $urandom, 0, rep ? 4'h5 : 4'h1));
                5: begin
                  items.push_back(core_item(tlul_pkg::PutFullData, g, 2, $urandom, 1'(tag)));
                  items.push_back(core_item(tlul_pkg::PutFullData, g+4, 2, $urandom, 1'(tag)));
                end
              endcase
              foreach (items[i]) begin
                changes.push_back(items[i].is_write() && current_tag != items[i].tag);
                if (items[i].is_write()) current_tag = items[i].tag;
              end
              `uvm_info("CMS_LATENCY_CASE", $sformatf("mode=%h class=%0d pre=%0d tag=%0d rep=%0d addr=%08x size=%0d",
                        modes[m], cls, pre, tag, rep, items[0].addr, items[0].size), UVM_LOW)
              measure(items, changes, m == 0);
            end
          end
        end
      end
    end
    // Complete every latency class before reporting the gap. The normal UVM quit count
    // must not stop the matrix at its first known failing case.
    if (latency_mismatches)
      `uvm_error("CMS_KNOWN_GAP_REQ_CMS_07",
                 $sformatf("%0d external latency checks exceeded the documented bound",
                           latency_mismatches))
  endtask
endclass
