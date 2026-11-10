// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Coverage of the documented behaviour the alert handler model went through (alert_handler_cmodel_sb,
// sampled each cycle from the model): every class state and every documented transition of each
// class (theory_of_operation.md "Escalation Mechanisms": Idle -> Timeout, Idle/Timeout -> Phase0,
// Phase0..3 -> next, Phase3 -> Terminal, any -> Idle on a clear), every escalation line asserted and
// released, every class interrupt, and the crash dump latched.
class alert_handler_cmodel_cov extends uvm_object;
  `uvm_object_utils(alert_handler_cmodel_cov)

  // State names as CLASSx_STATE reads them (alert_handler.hjson): Idle 0, Timeout 1, FsmError 2,
  // Terminal 3, Phase0..3 4..7.
  covergroup class_state_cg with function sample(int unsigned k, int unsigned prev,
                                                 int unsigned cur);
    cp_class: coverpoint k { bins cls[] = {[0:3]}; }
    cp_state: coverpoint cur {
      bins idle = {0}; bins timeout = {1}; bins terminal = {3};
      bins phase[] = {[4:7]};
    }
    cp_trans: coverpoint {prev[2:0], cur[2:0]} {
      bins idle_timeout   = {6'b000_001};
      bins idle_phase0    = {6'b000_100};
      bins timeout_phase0 = {6'b001_100};
      bins timeout_idle   = {6'b001_000};
      bins phase0_phase1  = {6'b100_101};
      bins phase1_phase2  = {6'b101_110};
      bins phase2_phase3  = {6'b110_111};
      bins phase3_term    = {6'b111_011};
      bins phase_cleared[] = {6'b100_000, 6'b101_000, 6'b110_000, 6'b111_000};
      bins term_cleared   = {6'b011_000};
    }
    cr_class_state: cross cp_class, cp_state;
    cr_class_trans: cross cp_class, cp_trans;
  endgroup

  covergroup esc_cg with function sample(int unsigned e, bit level);
    cp_esc: coverpoint e { bins esc[] = {[0:3]}; }
    cp_level: coverpoint level;
    cr_esc_level: cross cp_esc, cp_level;
  endgroup

  covergroup intr_cg with function sample(int unsigned k, bit pin);
    cp_class: coverpoint k { bins cls[] = {[0:3]}; }
    cp_pin: coverpoint pin;
    cr_class_pin: cross cp_class, cp_pin;
  endgroup

  covergroup dump_cg with function sample(bit latched);
    cp_latched: coverpoint latched;
  endgroup

  protected int unsigned prev_state[4];
  protected bit [3:0]    prev_esc;
  protected bit [3:0]    prev_intr;
  protected bit          prev_dump;

  function new(string name = "");
    super.new(name);
    class_state_cg = new();
    esc_cg = new();
    intr_cg = new();
    dump_cg = new();
  endfunction

  function void sample_model(chandle h);
    bit [3:0] intr = eg2_alert_cmodel_pkg::eg2_alert_intr_state(h) &
                     eg2_alert_cmodel_pkg::eg2_alert_intr_enable(h);
    bit dump = eg2_alert_cmodel_pkg::eg2_alert_dump_latched(h) != 0;
    for (int k = 0; k < 4; k++) begin
      int unsigned s = eg2_alert_cmodel_pkg::eg2_alert_class_state(h, k);
      if (s != prev_state[k]) class_state_cg.sample(k, prev_state[k], s);
      prev_state[k] = s;
    end
    for (int e = 0; e < 4; e++) begin
      bit l = eg2_alert_cmodel_pkg::eg2_alert_esc(h, e) != 0;
      if (l != prev_esc[e]) esc_cg.sample(e, l);
      prev_esc[e] = l;
    end
    for (int k = 0; k < 4; k++) begin
      if (intr[k] != prev_intr[k]) intr_cg.sample(k, intr[k]);
    end
    prev_intr = intr;
    if (dump != prev_dump) dump_cg.sample(dump);
    prev_dump = dump;
  endfunction

endclass
