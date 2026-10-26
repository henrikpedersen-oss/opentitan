// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_control_status (EG2 spec REQ_PBF_20, REQ_CDM_03; testpoint
// chip_rv_dm_control_status): dmcontrol and dmstatus (hw/vendor/pulp_riscv_dbg/doc/
// debug-system.md, RISC-V Debug Specification 0.13.2), both modes:
//  - hartsel 0; dmstatus reports havereset after the power-on reset; ackhavereset clears it;
//  - hartsel written all ones reads back 0 (the testpoint: WARL with one hart; the vendor table
//    lists hartsello/hartselhi as R/W, so a difference is a question for the rv_dm owners);
//  - n_halts times, at the drawn moments: haltreq halts the core (allhalted, not running),
//    haltreq cleared, resumereq resumes it (allrunning, allresumeack, not halted).
// The software checks its loop ran undisturbed across the halts (eg2_debug.h).
//
// EXPECTED FAILURE in CHERIoT mode until ticket F13 (status/rtl_tickets.md): rv_dm has no
// CHERIoT input, so a halt runs the base debug ROM on a core in CHERIoT mode, whose integer
// accesses to the debug memory are capability faults: the core is never reported halted and
// wait_cpu_halted times out. Written to REQ_CDM_03; it must pass once F13 is fixed.
class chip_eg2_rv_dm_control_status_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_control_status_vseq)
  `uvm_object_new

  rand int unsigned n_halts;
  rand int unsigned halt_gap_ns[4];
  constraint n_halts_c {
    n_halts dist {1 := 40, [2:4] :/ 60};
    foreach (halt_gap_ns[i]) halt_gap_ns[i] inside {[1_000:100_000]};
  }

  virtual task body();
    uvm_reg_data_t v;
    super.body();
    wait_ready();
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartsello), .value(0), .blocking(1));
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartselhi), .value(0), .blocking(1));
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allhavereset, v), 1,
                 "dmstatus.allhavereset after power-on")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.anyhavereset, v), 1,
                 "dmstatus.anyhavereset after power-on")
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.ackhavereset), .value(1), .blocking(1));
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allhavereset, v), 0,
                 "dmstatus.allhavereset after ackhavereset")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.anyhavereset, v), 0,
                 "dmstatus.anyhavereset after ackhavereset")

    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartsello), .value('1), .blocking(1), .predict(0));
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartselhi), .value('1), .blocking(1), .predict(0));
    csr_rd(.ptr(cfg.jtag_dmi_ral.dmcontrol), .value(v), .blocking(1));
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmcontrol.hartsello, v), 0, "hartsello (WARL)")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmcontrol.hartselhi, v), 0, "hartselhi (WARL)")
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartsello), .value(0), .blocking(1));
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartselhi), .value(0), .blocking(1));

    for (int i = 0; i < n_halts; i++) begin
      #(halt_gap_ns[i] * 1ns);
      cfg.debugger.set_haltreq(1);
      cfg.debugger.wait_cpu_halted();
      csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
      `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allhalted, v), 1, "allhalted")
      `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allrunning, v), 0, "allrunning")
      cfg.debugger.set_haltreq(0);
      cfg.debugger.set_resumereq(1);
      cfg.debugger.wait_cpu_resumed();
      csr_rd(.ptr(cfg.jtag_dmi_ral.dmstatus), .value(v), .blocking(1));
      `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allresumeack, v), 1, "allresumeack")
      `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allrunning, v), 1, "allrunning")
      `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.dmstatus.allhalted, v), 0, "allhalted")
      cfg.debugger.set_resumereq(0);
    end
    finish(3);
  endtask

endclass : chip_eg2_rv_dm_control_status_vseq
