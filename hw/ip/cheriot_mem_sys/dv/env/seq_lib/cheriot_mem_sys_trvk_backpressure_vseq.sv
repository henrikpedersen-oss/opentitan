// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Sweep under bounded, independent data/meta latency and request/response stalls while core and
// bitmap traffic continue. Check external TBRE outstanding reads (at most two), forward progress,
// final tags and data; then repeat with prompt bitmap responses and measure the core path.
// The internal TRVK verdict cycle and owner-specific bitmap count cannot be measured at this
// IP's external ports; they remain explicit coverage gaps, never inferred from internal taps.
// Requirements: REQ_CMS_07–08; REQ_TBRE_13–16, REQ_TBRE_37–40; REQ_TAG_09.
class cheriot_mem_sys_trvk_backpressure_vseq extends cheriot_mem_sys_latency_vseq;
  `uvm_object_utils(cheriot_mem_sys_trvk_backpressure_vseq)
  `uvm_object_new

  virtual task body();
    int unsigned sweeps = 6;
    void'($value$plusargs("cms_bp_sweeps=%d", sweeps));
    cheriot_on();
    for (int sweep = 0; sweep <= sweeps; sweep++) begin
      bit stop = 0;
      int outstanding = 0, maximum = 0;
      int unsigned cycles = 0, last_progress = 0, responses = 0;
      bit [31:0] base = MainSramBase + 'h800;
      fill_caps(base, 64);
      m_tbre_h_seq.min_rsp_delay = 0;
      m_tbre_h_seq.max_rsp_delay = sweep == sweeps ? 7 : $urandom_range(160, 16);
      m_meta_seq.min_rsp_delay = 0;
      m_meta_seq.max_rsp_delay = sweep == sweeps ? 0 : $urandom_range(160, 16);
      m_cored_h_seq.max_rsp_delay = sweep == sweeps ? 0 : 64;
      cfg.m_tbre_h_agent_cfg.a_ready_delay_max = sweep == sweeps ? 0 : 80;
      cfg.m_meta_agent_cfg.a_ready_delay_max = sweep == sweeps ? 0 : 80;
      cfg.m_core_agent_cfg.d_ready_delay_max = sweep == sweeps ? 0 : 80;
      fork
        begin
          tbre_sweep(base, 64);
          stop = 1;
        end
        begin
          while (!stop) begin
            bit tag;
            tl_seq_item rsp;
            bit [31:0] addr = base + 8 * $urandom_range(63);
            bit [31:0] w0, w1, heap;
            bit err;
            case ($urandom_range(5))
              0, 1: core_load_cap(addr, tag);
              2: begin
                make_cap(NvmBase, 0, PermsMemRw, w0, w1);
                core_store_cap(addr, w0, w1, 1);
              end
              3: core_store(addr, 0, $urandom, err);
              4: heap_base(rbit(), heap);
              5: bitmap_access(1, tlul_pkg::Get, MetaBase + 4 * $urandom_range(RevbmBytes/4-1),
                                 2, 0, 0, rsp);
            endcase
            wait_cycles($urandom_range(20));
          end
        end
        begin
          while (!stop) begin
            @(cfg.m_tbre_h_agent_cfg.vif.mon_cb);
            cycles++;
            if (cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_valid &&
                cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.a_ready) outstanding++;
            if (cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.d_valid &&
                cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.d_ready) begin
              outstanding--; responses++; last_progress = cycles;
              if (corrupt_check("backpressure"))
                `DV_CHECK_LE(outstanding + 3, 2, "CMS_CORRUPT_BACKPRESSURE")
            end
            if (outstanding > maximum) maximum = outstanding;
            `DV_CHECK_LE(outstanding, 2, "too many external TBRE reads in flight")
            `DV_CHECK_GE(outstanding, 0, "unexpected external TBRE response")
            if (cycles - last_progress > 10000)
              `uvm_fatal(`gfn, "no external TBRE progress under bounded backpressure")
          end
        end
      join
      `DV_CHECK_EQ(responses, 128, "every swept word must be read")
      `uvm_info(`gfn, $sformatf("backpressure sweep %0d: %0d responses, max outstanding %0d",
                                sweep, responses, maximum), UVM_LOW)
      wait_quiet();
      load_all_caps(base, 64);
    end
    // The same zero/2-cycle core latency contract as the dedicated latency sequence.
    super.body();
  endtask
endclass
