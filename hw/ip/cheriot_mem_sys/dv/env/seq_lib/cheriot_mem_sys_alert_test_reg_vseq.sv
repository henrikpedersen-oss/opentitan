// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A write of ALERT_TEST.fatal_fault raises exactly one fatal_fault handshake and does not latch,
// while ALERT_TEST.regwen is set; writing 0 to regwen (rw0c, reset 1) disables alert testing
// until reset, the write that clears it still raising its own alert (registers.md, ALERT_TEST).
// The scoreboard checks each expected alert and flags any other.
class cheriot_mem_sys_alert_test_reg_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_alert_test_reg_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] d;
    cheriot_on();
    wait_cycles(20);
    csr_rd(.ptr(ral.alert_test), .value(d));
    alert_test_write(32'h8000_0001);  // fatal_fault, regwen kept
    alert_test_write(32'h8000_0001);  // again
    alert_test_write(32'h8000_0000);  // fatal_fault 0
    csr_rd(.ptr(ral.alert_test), .value(d));
    alert_test_write(32'h0000_0000);  // regwen cleared
    csr_rd(.ptr(ral.alert_test), .value(d));
    alert_test_write(32'h8000_0001);  // locked
    csr_rd(.ptr(ral.alert_test), .value(d));
    // Reset sets regwen again; a write of 0x1 (fatal_fault 1, regwen 0) raises its alert and locks
    do_reset();
    csr_rd(.ptr(ral.alert_test), .value(d));
    alert_test_write(32'h0000_0001);
    alert_test_write(32'h8000_0001);  // locked by the previous write
    csr_rd(.ptr(ral.alert_test), .value(d));
  endtask

endclass
