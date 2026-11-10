// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The core on the memory subsystem's data port (always a host).
class cheriot_mem_sys_core_agent extends dv_base_agent #(
    .CFG_T      (cheriot_mem_sys_core_agent_cfg),
    .DRIVER_T   (cheriot_mem_sys_core_driver),
    .SEQUENCER_T(cheriot_mem_sys_core_sequencer),
    .MONITOR_T  (cheriot_mem_sys_core_monitor)
  );
  `uvm_component_utils(cheriot_mem_sys_core_agent)

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (cfg.vif == null &&
        !uvm_config_db#(virtual cheriot_mem_sys_core_if)::get(this, "", "vif", cfg.vif)) begin
      `uvm_fatal(`gfn, "failed to get the cheriot_mem_sys_core_if handle")
    end
  endfunction

endclass
