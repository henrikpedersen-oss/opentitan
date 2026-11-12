// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Reset during an externally active sweep, retain memory contents, check reset CSRs and quiet
// ports, then load and sweep again. Repeat with racing core stores and reset in ePMP mode.
// The model retains both tag outcomes for work interrupted by reset, without an internal tap.
// Requirements: REQ_CMS_15 (block part), REQ_REG_01, REQ_TBRE_05–12, REQ_TAG_01–04.
class cheriot_mem_sys_reset_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_reset_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] d, w0, w1;
    bit tag;
    cheriot_on();
    for (int rep = 0; rep < 4; rep++) begin
      randomize_timing(0);
      fill_caps(MainSramBase, 128);
      m_tbre_h_seq.min_rsp_delay = 10;
      m_tbre_h_seq.max_rsp_delay = 30;
      tbre_sweep(MainSramBase, 128, 0);
      for (int i = 0; i < 16; i++) begin
        make_cap(NvmBase, 0, PermsMemRw, w0, w1);
        core_store_cap(MainSramBase + 8 * $urandom_range(127), w0, w1, rbit());
      end
      wait_core_quiet();
      csr_rd(.ptr(ral.tbre_status), .value(d));
      `DV_CHECK_EQ(d[StatusBusy], 1, "reset must interrupt an active sweep")
      // Do not use do_reset(): its quiet wait would remove the interrupted-sweep stimulus.
      apply_reset("HARD");
      sweep_running = 0;
      wait_cycles(3);
      csr_rd(.ptr(ral.tbre_regwen), .value(d));
      `DV_CHECK_EQ(d, 1)
      csr_rd(.ptr(ral.tbre_status), .value(d));
      `DV_CHECK_EQ(d, 0)
      csr_rd(.ptr(ral.tbre_epoch), .value(d));
      `DV_CHECK_EQ(d, 0)
      csr_rd(.ptr(ral.intr_state), .value(d));
      `DV_CHECK_EQ(d, 0)
      csr_rd(.ptr(ral.tbre_base_addr), .value(d));
      csr_rd(.ptr(ral.tbre_num_caps), .value(d));
      m_tbre_h_seq.min_rsp_delay = 0;
      m_tbre_h_seq.max_rsp_delay = 4;
      wait_cycles(200);
      load_all_caps(MainSramBase, 64);
      tbre_sweep(MainSramBase, 128);
      wait_quiet();
    end
    set_ena(prim_mubi_pkg::MuBi4False);
    do_reset();
    core_load_cap(MainSramBase, tag);
  endtask
endclass
