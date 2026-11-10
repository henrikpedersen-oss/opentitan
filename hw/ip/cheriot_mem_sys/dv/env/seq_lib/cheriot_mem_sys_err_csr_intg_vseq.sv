// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A command integrity fault on the register port is answered with d_error, does not write the
// register, and raises fatal_fault, latched until reset (theory_of_operation.md, "Error
// Handling").
class cheriot_mem_sys_err_csr_intg_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_csr_intg_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]  d;
    tl_seq_item rsp;
    cheriot_on();
    csr_wr(.ptr(ral.tbre_base_addr), .value(MainSramBase + 32'h100));
    csr_raw_access(tlul_pkg::PutFullData, ral.tbre_base_addr.get_offset(), 2,
                   MainSramBase + 32'h800, 0, .bad_cmd_intg(1'b1), .rsp(rsp));
    csr_rd(.ptr(ral.tbre_base_addr), .value(d));  // the C model expects the old value
    // Latched until reset: the scoreboard expects the alert to keep coming meanwhile
    wait_cycles(500);
    do_reset();
    wait_cycles(300);
  endtask

endclass
