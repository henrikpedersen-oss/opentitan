// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_core_agent_cfg extends dv_base_agent_cfg;

  virtual cheriot_mem_sys_core_if vif;

  // Requests in flight at most; Ibex's LSU has two.
  int unsigned max_outstanding = 2;
  // Source IDs the driver uses. The subsystem's sockets take the top bits of the source ID, so
  // keep it small.
  int unsigned num_sources = 4;

  // Cycles before a request is presented, and cycles d_ready is held low before each response.
  int unsigned a_valid_delay_max = 0;
  int unsigned d_ready_delay_max = 0;
  // Directed external backpressure, released by a sequence or a handoff item.
  bit hold_d_ready = 0;

  `uvm_object_utils_begin(cheriot_mem_sys_core_agent_cfg)
    `uvm_field_int(max_outstanding,   UVM_DEFAULT)
    `uvm_field_int(num_sources,       UVM_DEFAULT)
    `uvm_field_int(a_valid_delay_max, UVM_DEFAULT)
    `uvm_field_int(d_ready_delay_max, UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new

endclass
