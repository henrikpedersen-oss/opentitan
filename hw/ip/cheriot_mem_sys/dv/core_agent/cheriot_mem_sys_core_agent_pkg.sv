// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

package cheriot_mem_sys_core_agent_pkg;
  import uvm_pkg::*;
  import dv_utils_pkg::*;
  import dv_base_agent_pkg::*;

  `include "uvm_macros.svh"
  `include "dv_macros.svh"

  `include "cheriot_mem_sys_core_item.sv"
  `include "cheriot_mem_sys_core_agent_cfg.sv"

  typedef dv_base_sequencer #(.ITEM_T(cheriot_mem_sys_core_item),
                              .CFG_T (cheriot_mem_sys_core_agent_cfg))
      cheriot_mem_sys_core_sequencer;

  `include "cheriot_mem_sys_core_driver.sv"
  `include "cheriot_mem_sys_core_monitor.sv"
  `include "cheriot_mem_sys_core_agent.sv"
  `include "cheriot_mem_sys_core_access_seq.sv"

endpackage
