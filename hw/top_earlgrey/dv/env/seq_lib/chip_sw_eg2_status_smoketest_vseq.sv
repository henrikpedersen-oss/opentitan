// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The parameters of eg2_status_smoketest (sw/device/tests/eg2_status_smoketest.c): which alert it
// raises, and how many error interrupts; the software picks which interrupts from its seed.
class chip_sw_eg2_status_smoketest_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_status_smoketest_vseq)

  `uvm_object_new

  rand bit [31:0] alert;
  rand bit [31:0] num_intrs;

  // Any alert of the chip, in alert-handler order.
  constraint alert_c {
    alert < NUM_ALERTS;
  }

  constraint num_intrs_c {
    num_intrs inside {[1:4]};
  }

  virtual function void write_params();
    set_param("alert", alert);
    set_param("num_intrs", num_intrs);
  endfunction

endclass : chip_sw_eg2_status_smoketest_vseq
