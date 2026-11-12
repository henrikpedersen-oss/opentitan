// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
// Bound RTL coverage lives separately in sva/; this logger remains print-only.
//
// Optional diagnostic logger (+cms_meta_trace). Internal taps only print what the DUT routed.
// No analysis port, ownership state, prediction, assertion or check uses these signals.
// The reference model and scoreboard use external ports exclusively.
class cheriot_mem_sys_meta_monitor extends uvm_component;
  `uvm_component_utils(cheriot_mem_sys_meta_monitor)
  `uvm_component_new
  virtual cheriot_mem_sys_meta_if vif;

  task run_phase(uvm_phase phase);
    if (vif == null) return;
    forever begin
      @(vif.mon_cb);
      if (!vif.mon_cb.rst_n) continue;
      for (int i = 0; i < 2; i++) begin
        if (vif.mon_cb.tag_h2d[i].a_valid && vif.mon_cb.tag_d2h[i].a_ready)
          `uvm_info(`gfn, $sformatf("DUT tag route %0d: source %h address %h opcode %h", i,
            vif.mon_cb.tag_h2d[i].a_source, vif.mon_cb.tag_h2d[i].a_address,
            vif.mon_cb.tag_h2d[i].a_opcode), UVM_LOW)
      end
      for (int i = 0; i < 4; i++) begin
        if (vif.mon_cb.branches_h2d[i].a_valid && vif.mon_cb.branches_d2h[i].a_ready)
          `uvm_info(`gfn, $sformatf("DUT meta route %0d: source %h address %h opcode %h", i,
            vif.mon_cb.branches_h2d[i].a_source, vif.mon_cb.branches_h2d[i].a_address,
            vif.mon_cb.branches_h2d[i].a_opcode), UVM_LOW)
      end
    end
  endtask
endclass
