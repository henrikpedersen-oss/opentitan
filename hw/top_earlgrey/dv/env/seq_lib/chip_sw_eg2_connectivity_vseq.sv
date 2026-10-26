// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The knobs of chip_sw_eg2_connectivity (sw/device/tests/eg2_regmem/eg2_connectivity.c; EG2
// verification spec, chip_sw_eg2_connectivity): which blocks (one bit per block of the generated
// eg2_regmem_tables.h, in its order; every block by default) and in which order.
class chip_sw_eg2_connectivity_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_connectivity_vseq)

  `uvm_object_new

  // EG2_RM_BLOCK_COUNT of the generated eg2_regmem_tables.h.
  localparam int unsigned NumBlocks = 48;

  rand bit [63:0]   blocks;
  rand int unsigned order;
  rand int unsigned alert_polls;

  constraint blocks_c {
    soft blocks == '1;  // every block
    blocks[NumBlocks-1:0] != '0;
  }

  constraint order_c {
    order inside {[0:2]};  // the top's order, reverse, random
  }

  constraint alert_polls_c {
    soft alert_polls == 10000;
    alert_polls inside {[100:1000000]};
  }

  virtual function void write_params();
    set_param("blocks_lo", blocks[31:0]);
    set_param("blocks_hi", blocks[63:32]);
    set_param("order", order);
    set_param("alert_polls", alert_polls);
    // model_gaps stays 0 on the chip: every check is made.
  endfunction

endclass : chip_sw_eg2_connectivity_vseq
