// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Legal targeted toggles from evidence/cms-coverage-1111/coverage/code-holes.csv:
// core source IDs (including WTRC tag-write IDs), data payloads, TBRE current and
// clear addresses, WTRC cap address and tag-word address. Fixed protocol/reserved
// fields and bounded counter high bits are left for UNR, not driven illegally.
// Default EG2 map is required to reach its upper SRAM and NVM address bits.
// Requirements: REQ_TF_01–13; REQ_WTRC_01–13; REQ_TBRE_18–27, REQ_TBRE_39.
class cheriot_mem_sys_toggle_sweeps_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_toggle_sweeps_vseq)
  `uvm_object_new

  protected task access_id(tlul_pkg::tl_a_op_e op, bit [31:0] addr, bit [31:0] data,
                           bit tag, int source);
    cheriot_mem_sys_core_item it = core_item(op, addr, 2, data, tag);
    it.use_fixed_source = 1;
    it.source = top_pkg::TL_AIW'(source);
    core_access(it);
  endtask

  virtual task body();
    int ids[7] = '{0, 1, 2, 4, 8, 16, 31};
    bit [31:0] w0, w1;
    bit [31:0] addresses[$];
    `DV_CHECK_EQ_FATAL(RevbmBytes, 3072, "toggle_sweeps requires the default EG2 map")
    cheriot_on();
    cfg.m_core_agent_cfg.d_ready_delay_max = 5;
    cfg.m_cored_h_agent_cfg.a_ready_delay_max = 5;
    cfg.m_tbre_h_agent_cfg.a_ready_delay_max = 5;
    m_meta_seq.min_rsp_delay = 2;
    m_meta_seq.max_rsp_delay = 5;
    // Walking ones and zeros exercise payload and integrity bits without faults.
    for (int bitno = 0; bitno < 32; bitno++) begin
      for (int invert = 0; invert < 2; invert++) begin
        bit [31:0] data = invert ? ~(32'b1 << bitno) : (32'b1 << bitno);
        access_id(tlul_pkg::PutFullData, MainSramBase, data, 0, ids[bitno % 7]);
        access_id(tlul_pkg::Get, MainSramBase, 0, 0, ids[(bitno + 1) % 7]);
        revbm_write(0, data);
      end
    end
    // One capability at each legal address-bit boundary, plus both region ends.
    addresses.push_back(MainSramBase);
    addresses.push_back(MainSramTop - 8);
    addresses.push_back(NvmBase);
    addresses.push_back(NvmTop - 8);
    for (int bitno = 3; bitno < 21; bitno++) begin
      if ((32'b1 << bitno) < SramBytes) addresses.push_back(MainSramBase + (32'b1 << bitno));
      if ((32'b1 << bitno) < NvmBytes) addresses.push_back(NvmBase + (32'b1 << bitno));
    end
    revbm_set_base(MainSramBase, 1);
    foreach (addresses[i]) begin
      make_kind_cap(MainSramBase, CapMemRw, 0, w0, w1);
      nvm_program(addresses[i], w0, w1);
      access_id(tlul_pkg::PutFullData, addresses[i], w0, 1, ids[i % 7]);
      access_id(tlul_pkg::PutFullData, addresses[i] + 4, w1, 1, ids[(i + 1) % 7]);
      wait_quiet();
      tbre_sweep(addresses[i], 1);
      access_id(tlul_pkg::Get, addresses[i], 0, 1, ids[(i + 2) % 7]);
      access_id(tlul_pkg::Get, addresses[i] + 4, 0, 1, ids[(i + 3) % 7]);
    end
    wait_quiet();
  endtask
endclass
