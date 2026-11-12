// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// NVM comparison failures, repeated W0/W1, partial W0/W1 and wrong-granule W1,
// with a clear tag or a tag made ambiguous by a bitmap write during a sweep.
// Only the WTRC core-stimulus assertions are disabled around malformed stores.
// The independent model retains every documented tag outcome of the overlap.
// Requirements: REQ_WTRC_01–13; REQ_TBRE_32–36; REQ_CMS_17 (block part).
class cheriot_mem_sys_nvm_corners_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_nvm_corners_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] w0, w1;
    bit e0, e1, tag;
    for (int ambiguous = 0; ambiguous < 2; ambiguous++) begin
      for (int scenario = 0; scenario < 6; scenario++) begin
        bit [31:0] g = NvmBase + 32'h400 + 16 * scenario;
        cheriot_on();
        make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
        revbm_set_base(MainSramBase, 0);
        nvm_program(g, w0, w1);
        nvm_program(g + 8, w0, w1);
        if (ambiguous) begin
          nvm_cap_store_wait(g, w0, w1, e0, e1);
          wait_quiet();
          m_tbre_h_seq.min_rsp_delay = 50;
          m_tbre_h_seq.max_rsp_delay = 50;
          tbre_sweep(g, 1, 0);
          revbm_set_base(MainSramBase, 1);
          tbre_wait_idle();
          m_tbre_h_seq.min_rsp_delay = 0;
          m_tbre_h_seq.max_rsp_delay = 10;
        end
        if (scenario inside {0, 1, 2, 5}) wtrc_assertions(0);
        case (scenario)
          0: begin
            core_access(core_item(tlul_pkg::PutPartialData, g, 2, w0, 1, 4'b0111));
            core_access(core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1));
          end
          1: begin
            core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
            core_access(core_item(tlul_pkg::PutPartialData, g + 4, 2, w1, 1, 4'b0111));
          end
          2: begin
            core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
            core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
            core_access(core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1));
          end
          3: nvm_cap_store_wait(g, w0 ^ 1, w1, e0, e1);
          4: nvm_cap_store_wait(g, w0, w1 ^ 1, e0, e1);
          5: begin
            core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
            core_access(core_item(tlul_pkg::PutFullData, g + 12, 2, w1, 1));
          end
        endcase
        wait_quiet();
        if (scenario inside {0, 1, 2, 5}) wtrc_assertions(1);
        core_load_cap(g, tag);
        core_load_cap(g + 8, tag);
        do_reset();
      end
    end
    repeated_w1_while_response_held();
  endtask

  // A completed tag write offers W1's response, but the core has not accepted
  // it yet. Presenting another W1 is out of sequence. Schedule solely from
  // that external held response; do not inspect the WTRC state machine.
  protected task repeated_w1_while_response_held();
    bit [31:0] g = NvmBase + 32'h800;
    bit [31:0] w0, w1;
    bit tag;
    cheriot_mem_sys_core_item repeated_word;
    cheriot_on();
    revbm_set_base(MainSramBase, 0);
    make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
    nvm_program(g, w0, w1);
    core_access(core_item(tlul_pkg::PutFullData, g, 2, w0, 1));
    cfg.m_core_agent_cfg.hold_d_ready = 1;
    core_issue(core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1));
    do @(cfg.m_core_agent_cfg.vif.mon_cb);
    while (!cfg.m_core_agent_cfg.vif.mon_cb.d2h.d_valid);
    wtrc_assertions(0);
    repeated_word = core_item(tlul_pkg::PutFullData, g + 4, 2, w1, 1);
    core_issue(repeated_word);
    wait (repeated_word.presented);
    wait_cycles(8);
    cfg.m_core_agent_cfg.hold_d_ready = 0;
    wait_quiet();
    wtrc_assertions(1);
    core_load_cap(g, tag);
    do_reset();
  endtask
endclass
