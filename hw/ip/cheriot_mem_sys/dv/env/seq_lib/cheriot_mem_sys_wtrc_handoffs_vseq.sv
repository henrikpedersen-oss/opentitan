// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Fill the tag request queue, then present an unaccepted request. Present the
// next NVM W0 on the cycle the previous W1 response is consumed, both after a
// failed comparison and after a successful tag write. Scheduling uses only
// external A/D handshakes; the reference model checks responses and meta SRAM.
// Requirements: REQ_WTRC_01–13; REQ_TF_02, REQ_TF_12–13.
class cheriot_mem_sys_wtrc_handoffs_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_wtrc_handoffs_vseq)
  `uvm_object_new

  virtual task body();
    cheriot_mem_sys_core_item a, b, c;
    bit [31:0] w0, w1;
    cheriot_on();
    cfg.m_core_agent_cfg.a_valid_delay_max = 0;
    cfg.m_core_agent_cfg.d_ready_delay_max = 0;
    cfg.m_cored_h_agent_cfg.a_ready_delay_min = 0;
    cfg.m_cored_h_agent_cfg.a_ready_delay_max = 0;
    // The driver may present the third request, but still permits only two
    // accepted requests until a response frees a slot.
    cfg.m_core_agent_cfg.hold_d_ready = 1;
    a = core_item(tlul_pkg::Get, MainSramBase, 2, 0, 1);
    b = core_item(tlul_pkg::Get, MainSramBase + 8, 2, 0, 1);
    c = core_item(tlul_pkg::Get, MainSramBase + 16, 2, 0, 1);
    c.present_when_full = 1;
    core_issue(a);
    core_issue(b);
    core_issue(c);
    wait (c.presented);
    wait_cycles(8);
    `DV_CHECK_EQ(c.accepted, 0, "third lookup must be stalled while both slots are full")
    cfg.m_core_agent_cfg.hold_d_ready = 0;
    wait_core_quiet();

    for (int match = 0; match < 2; match++) begin
      bit [31:0] g = NvmBase + 32'h100 + 32 * match;
      make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
      nvm_program(g, w0, w1);
      nvm_program(g + 8, w0, w1);
      core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
      cfg.m_core_agent_cfg.hold_d_ready = 1;
      a = core_item(tlul_pkg::PutFullData, g + 4, 2, w1 ^ (match ? 0 : 1), 1);
      core_issue(a);
      // Wait for W1's response to be offered, held by external D backpressure.
      do @(cfg.m_core_agent_cfg.vif.mon_cb);
      while (!cfg.m_core_agent_cfg.vif.mon_cb.d2h.d_valid);
      b = core_item(tlul_pkg::PutFullData, g + 8, 2, w0, 1);
      b.release_d_ready = 1;
      core_issue(b);
      wait (b.presented);
      @(cfg.m_core_agent_cfg.vif.mon_cb);
      `DV_CHECK_EQ(cfg.m_core_agent_cfg.vif.mon_cb.h2d.a_valid, 1)
      `DV_CHECK_EQ(cfg.m_core_agent_cfg.vif.mon_cb.d2h.a_ready, 1)
      `DV_CHECK_EQ(cfg.m_core_agent_cfg.vif.mon_cb.d2h.d_valid, 1)
      `DV_CHECK_EQ(cfg.m_core_agent_cfg.vif.mon_cb.h2d.d_ready, 1)
      `uvm_info(`gfn, $sformatf("CMS_HANDOFF: next W0 with W1 response, match=%0d", match), UVM_LOW)
      core_access(core_item(tlul_pkg::PutFullData, g + 12, 2, w1, 1));
      wait_core_quiet();
    end
    wait_quiet();
  endtask
endclass
