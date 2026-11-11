// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Single-cycle write-only fault injection at each of the seven legacy TRVK sites. A fixed known
// revoked capability and bitmap are established through ports. Pulse timing scans offsets from
// the external TBRE request; no internal value selects the pulse or determines effectiveness.
// A control sweep must clear the tag. A fault is effective only when a subsequent external
// capability load retains it; then fatal_fault must have been observed. Only the injected
// granule's tag is allowed either value in the independent model for that trial.
// Requirements: REQ_CMS_02 (known gap: LOGIC.SHADOW absent), REQ_CMS_08.
class cheriot_mem_sys_trvk_fault_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_trvk_fault_vseq)
  `uvm_object_new

  virtual task body();
    string site = "verdict", signal_name, path;
    bit [31:0] value, w0, w1;
    bit tag;
    int effective = 0, detected = 0;
    bit [31:0] addr = MainSramBase + 'h800;
    void'($value$plusargs("cms_tf_site=%s", site));
    case (site)
      "verdict":      begin signal_name = "revbm_revoked"; value = 0; end
      "tag_out":      begin signal_name = "upstream_tag_o"; value = 1; end
      "bit_select":   begin signal_name = "revbm_bit_select"; value = 1; end
      "req_required": begin signal_name = "revbm_req_required"; value = 0; end
      "out_of_range": begin signal_name = "revbm_out_of_range"; value = 1; end
      "sealing":      begin signal_name = "is_sealing_cap"; value = 1; end
      "align":        begin signal_name = "misalign_flag_out"; value = 0; end
      default: `uvm_fatal(`gfn, {"unknown TRVK injection site: ", site})
    endcase
    path = {"tb.dut.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core.", signal_name};
    cheriot_on();
    randomize_timing(1);
    // Keep the external latency fixed across offset trials. Re-randomising it for each
    // one-cycle pulse can miss the decision every time and give an inconclusive campaign.
    cfg.m_tbre_h_agent_cfg.a_ready_delay_min = 0;
    cfg.m_tbre_h_agent_cfg.a_ready_delay_max = 0;
    cfg.m_meta_agent_cfg.a_ready_delay_min = 0;
    cfg.m_meta_agent_cfg.a_ready_delay_max = 0;
    m_tbre_h_seq.min_rsp_delay = 0;
    m_tbre_h_seq.max_rsp_delay = 0;
    m_meta_seq.min_rsp_delay = 0;
    m_meta_seq.max_rsp_delay = 0;
    // Bitmap bit 0 revoked, bit 1 clear: bit-select injection has a known opposite value.
    revbm_write(0, 1);
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    core_store_cap(addr, w0, w1, 1);
    wait_quiet();
    tbre_sweep(addr, 1);
    core_load_cap(addr, tag);
    if (corrupt_check("trvk")) tag = !tag;
    `DV_CHECK_EQ(tag, 0, "TRVK control sweep must clear the known revoked tag")
    for (int delay = 0; delay < 48; delay++) begin
      int alerts_before;
      bit observed_request = 0;
      do_reset();
      core_store_cap(addr, w0, w1, 1);
      wait_quiet();
      cfg.trvk_fault_addr = addr;
      cfg.trvk_fault_arm = 1;
      alerts_before = cfg.observed_fatal_alerts;
      fork
        begin tbre_sweep(addr, 1); end
        begin
          // Both the trigger and the verdict are external. The path is only a fault actuator.
          do @(cfg.m_tbre_h_agent_cfg.vif.mon_cb);
          while (!(cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_valid &&
                    cfg.m_tbre_h_agent_cfg.vif.mon_cb.d2h.a_ready &&
                    cfg.m_tbre_h_agent_cfg.vif.mon_cb.h2d.a_address == addr));
          observed_request = 1;
          repeat (delay) @(cfg.clk_rst_vif.cbn);
          @(negedge cfg.clk_rst_vif.clk);
          `DV_CHECK_FATAL(uvm_hdl_force(path, uvm_hdl_data_t'(value)), "TRVK force failed")
          @(negedge cfg.clk_rst_vif.clk);
          `DV_CHECK_FATAL(uvm_hdl_release(path), "TRVK release failed")
        end
        begin
          for (int timeout = 0; timeout < 2000 && !observed_request; timeout++) wait_cycles(1);
          if (!observed_request) `uvm_fatal(`gfn, "TRVK injection saw no external sweep request")
        end
      join
      wait_quiet();
      cfg.trvk_fault_arm = 0;
      core_load_cap(addr, tag);
      wait_cycles(600);
      if (tag) begin
        effective++;
        if (cfg.observed_fatal_alerts != alerts_before) detected++;
        `uvm_info("CMS_TRVK_FAULT", $sformatf(
          "%s offset=%0d: external load retained revoked tag, alert=%0b", site, delay,
          cfg.observed_fatal_alerts != alerts_before), UVM_LOW)
      end
    end
    `uvm_info("CMS_TRVK_FAULT", $sformatf("%s: %0d externally effective trials, %0d detected",
                                          site, effective, detected), UVM_LOW)
    `DV_CHECK_GT(effective, 0, "no externally effective TRVK fault: campaign inconclusive")
    if (detected != effective)
      `uvm_error("CMS_KNOWN_GAP_REQ_CMS_02", "externally effective TRVK faults were not alerted")
  endtask
endclass
