// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The tbre_done interrupt is raised when the engine stops being active, also after a sweep with
// an error; INTR_STATE is a level until written 1; INTR_ENABLE masks intr_tbre_done_o; INTR_TEST
// sets INTR_STATE; an ignored start raises none (theory_of_operation.md, "Revocation Engine";
// registers.md, INTR_STATE, INTR_ENABLE and INTR_TEST; interfaces.md, "Interrupts";
// programmers_guide.md, "Revoking Capabilities" and "Tracking Completed Sweeps").
// Requirements: REQ_TBRE_28–31; REQ_CMS_12 (block part).
class cheriot_mem_sys_tbre_intr_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_intr_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   d, a, b, cb, w0, w1;
    int unsigned n;
    cheriot_on();
    // The meta SRAM slowed down, so that the interrupt can come before the last tag is written
    cfg.m_meta_agent_cfg.a_ready_delay_max = 2;
    m_meta_seq.min_rsp_delay = 4;
    m_meta_seq.max_rsp_delay = 12;
    fill_caps(MainSramBase, 64);
    fill_caps(NvmBase, 32);
    revoked_cap_at(MainSramBase + 8 * 63);

    // Enabled: every tag is final when it is raised
    csr_wr(.ptr(ral.intr_enable), .value(32'h1));
    tbre_sweep(MainSramBase, 64, 1'b0);
    wait_intr_pin(1, 200000, "sweep with the interrupt enabled");
    csr_rd(.ptr(ral.tbre_status), .value(d));
    wait_quiet();
    csr_rd(.ptr(ral.intr_state), .value(d));
    // A level: it stays until acknowledged
    wait_cycles(200);
    csr_wr(.ptr(ral.intr_state), .value(32'h1));
    wait_intr_pin(0, 4, "INTR_STATE written 1");

    // Masked: the state is set, the pin stays low until enabled
    csr_wr(.ptr(ral.intr_enable), .value(32'h0));
    tbre_sweep(NvmBase, 32);
    csr_rd(.ptr(ral.intr_state), .value(d));
    csr_wr(.ptr(ral.intr_enable), .value(32'h1));
    wait_intr_pin(1, 4, "INTR_ENABLE written 1 with INTR_STATE set");
    csr_wr(.ptr(ral.intr_state), .value(32'h1));

    // INTR_TEST sets it; writing 0 does nothing
    csr_wr(.ptr(ral.intr_test), .value(32'h1));
    wait_intr_pin(1, 4, "INTR_TEST written 1");
    csr_rd(.ptr(ral.intr_state), .value(d));
    csr_wr(.ptr(ral.intr_state), .value(32'h1));
    csr_wr(.ptr(ral.intr_test), .value(32'h0));

    // Ignored starts raise none
    tbre_sweep(MainSramBase, 0);
    tbre_sweep(MainSramTop, 4);
    wait_cycles(100);
    csr_rd(.ptr(ral.intr_state), .value(d));
    csr_wr(.ptr(ral.tbre_status), .value(32'h1 << StatusStartErr));

    // Acknowledged while the sweep runs: raised at its end
    tbre_sweep(MainSramBase, SramCaps, 1'b0);
    csr_wr(.ptr(ral.intr_state), .value(32'h1));
    tbre_wait_intr();

    // Sweeps of both regions, waited for by the interrupt
    for (int unsigned i = 0; i < 6; i++) begin
      b = rbit() ? MainSramBase + 8 * $urandom_range(32) : NvmBase + 8 * $urandom_range(16);
      n = 1 + $urandom_range(15);
      revoked_cap_at(b + 8 * (n - 1));
      tbre_sweep(b, n, 1'b0);
      tbre_wait_intr();
      wait_quiet();
    end

    // A sweep that ends with an error raises it too
    b = MainSramBase + 32'h800;
    for (int unsigned i = 0; i < 4; i++) begin
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, w0, w1);
      core_store_cap(b + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    a = b + 8 * 2 + 4;
    m_tbre_h_seq.inject(InjErr, a, a + 4, InjReads);
    tbre_sweep(b, 4, 1'b0);
    tbre_wait_intr();
    wait_cycles(200);
    wait_quiet();
    do_reset();
    wait_cycles(4);
  endtask

endclass
