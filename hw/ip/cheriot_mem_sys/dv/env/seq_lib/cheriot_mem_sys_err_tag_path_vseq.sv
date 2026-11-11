// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A device error on the tag path aborts the read-modify-write, answers d_error to the core and
// raises fatal_fault: on the fill read of a store (no write-back, the tag unchanged), on a
// capability load's lookup (tag 0), and on the write-back (theory_of_operation.md, "RMW Filter"
// and "Error Handling").
class cheriot_mem_sys_err_tag_path_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_tag_path_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]                g, ta;
    int unsigned              injections;
    cheriot_mem_sys_core_item it;
    cheriot_on();
    for (int unsigned c = 0; c < 3; c++) begin
      g  = MainSramBase + 32'h400 + 8 * c;
      ta = tag_word_addr(g);
      setup_tagged(g, c != 2);
      case (c)
        0: begin  // the fill read of a store
          m_meta_seq.inject(InjErr, ta, ta + 4, InjReads);
          it = core_item(tlul_pkg::PutFullData, g, 2, 32'h1111_2222);
          core_access(it);
        end
        1: begin  // a capability load's lookup
          m_meta_seq.inject(InjErr, ta, ta + 4, InjReads);
          it = core_item(tlul_pkg::Get, g, 2, 0, 1);
          core_access(it);
        end
        default: begin  // the write-back
          m_meta_seq.inject(InjErr, ta, ta + 4, InjWrites);
          it = core_item(tlul_pkg::PutFullData, g, 2, 32'h3333_4444, 1);
          core_access(it);
        end
      endcase
      wait_quiet();
      do_reset();
    end
    // A plain load, or an unaligned hinted load, does no tag lookup and cannot inherit a
    // sweep's meta fault. Hold its data response outstanding while the sweep looks up the tag.
    for (int unsigned no_lookup = 0; no_lookup < 2; no_lookup++) begin
    g = MainSramBase + 32'h800;
    setup_tagged(g, 1'b1);
    wait_quiet();
    m_cored_h_seq.min_rsp_delay = 50;
    m_cored_h_seq.max_rsp_delay = 50;
    it = core_item(tlul_pkg::Get, g + 4 * no_lookup, 2, 0, no_lookup != 0);
    core_issue(it);
    injections = m_meta_seq.n_inj_applied;
    m_meta_seq.inject(InjErr, tag_word_addr(g), tag_word_addr(g) + 4, InjReads);
    fork
      tbre_sweep(g, 1);
      begin
        wait (m_meta_seq.n_inj_applied > injections);
        `DV_CHECK_EQ(it.done, 1'b0, "test must overlap the sweep fault with the plain load")
      end
    join
    wait (it.done);
    `DV_CHECK_EQ(it.err, 1'b0, "unhinted load must not inherit the sweep's meta fault")
    m_cored_h_seq.min_rsp_delay = 0;
    m_cored_h_seq.max_rsp_delay = 4;
    wait_quiet();
    do_reset();
    end

    // A hinted aligned load and a sweep can both own a shared tag-word fault. Delay the
    // external memory responses so both requests overlap. The scoreboard must retain either
    // documented outcome for both requesters; this sequence does not pick the DUT's owner.
    g = MainSramBase + 32'h900;
    setup_tagged(g, 1'b1);
    wait_quiet();
    m_meta_seq.min_rsp_delay = 50;
    m_meta_seq.max_rsp_delay = 50;
    m_cored_h_seq.min_rsp_delay = 50;
    m_cored_h_seq.max_rsp_delay = 50;
    m_meta_seq.inject(InjErr, tag_word_addr(g), tag_word_addr(g) + 4, InjReads);
    tbre_sweep(g, 1, 1'b0);
    it = core_item(tlul_pkg::Get, g, 2, 0, 1'b1);
    core_issue(it);
    tbre_wait_idle();
    wait (it.done);
    m_meta_seq.min_rsp_delay = 0;
    m_meta_seq.max_rsp_delay = 4;
    m_cored_h_seq.min_rsp_delay = 0;
    m_cored_h_seq.max_rsp_delay = 4;
    wait_quiet();
    do_reset();
  endtask

endclass
