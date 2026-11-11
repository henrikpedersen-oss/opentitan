// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// INTR_TEST requires byte zero. Refuse writes with that byte disabled, leaving
// INTR_STATE and its pin unchanged; accept byte-zero writes and their set effect.
// Related source: OpenTitan Register Tool, "Error responses"
// (util/reggen/README.md): writes smaller than the register's field-defined
// width return an error. https://opentitan.org/book/util/reggen/index.html#error-responses
// That text does not explicitly cover word-sized PutPartialData with byte zero
// disabled. Refusal with no state/pin change is DV assumption DV-CSR-BE in the
// verification spec; revise this check if the documented contract differs.
// Targets both INTR_TEST byte-permission expression terms in the register top.
// Also checks a zero INTR_TEST write with enable/state clear (no interrupt).
// Requirements: REQ_TBRE_28–31; REQ_CMS_12 (block part).
class cheriot_mem_sys_csr_byte_enable_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_csr_byte_enable_vseq)
  `uvm_object_new

  virtual task body();
    bit [3:0] refused_masks[4] = '{4'h2, 4'h4, 4'h8, 4'he};
    bit [31:0] state;
    tl_seq_item rsp;
    csr_wr(.ptr(ral.intr_enable), .value(0));
    csr_wr(.ptr(ral.intr_state), .value(1));
    csr_wr(.ptr(ral.intr_test), .value(0));
    csr_rd(.ptr(ral.intr_state), .value(state));
    `DV_CHECK_EQ(state[0], 0, "zero INTR_TEST write must leave the interrupt clear")
    `DV_CHECK_EQ(cfg.intr_vif.pins[0], 0)
    csr_wr(.ptr(ral.intr_enable), .value(1));
    foreach (refused_masks[i]) begin
      csr_raw_access(tlul_pkg::PutPartialData, ral.intr_test.get_offset(), 2,
                     32'hffff_ffff, refused_masks[i], 0, rsp);
      `DV_CHECK_EQ(rsp.d_error, 1, "INTR_TEST write without required byte zero must fail")
      csr_rd(.ptr(ral.intr_state), .value(state));
      `DV_CHECK_EQ(state[0], 0)
      `DV_CHECK_EQ(cfg.intr_vif.pins[0], 0)
    end
    csr_raw_access(tlul_pkg::PutPartialData, ral.intr_test.get_offset(), 2, 1, 4'h1, 0, rsp);
    `DV_CHECK_EQ(rsp.d_error, 0)
    csr_rd(.ptr(ral.intr_state), .value(state));
    `DV_CHECK_EQ(state[0], 1)
    wait_intr_pin(1, 4, "permitted INTR_TEST byte write");
    csr_wr(.ptr(ral.intr_state), .value(1));
    wait_intr_pin(0, 4, "interrupt acknowledgement");
  endtask
endclass
