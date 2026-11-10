// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// EG2 FPV properties of alert_handler (edagen formal.files), bound into the design. Each from the
// documents (theory_of_operation.md "Escalation Mechanisms", "Programmable Escalation Protocol";
// alert_handler.hjson CLASSx_CLR_REGWEN, CLASSx_ACCUM_CNT, the local alert IDs); the design signals
// they read are only the class FSM state as CLASSx_STATE shows it, the accumulator as
// CLASSx_ACCUM_CNT shows it, the class clear and the register values.
//
//   EscPhaseForward_A  an escalating class only moves forward: from PhaseN to PhaseN or PhaseN+1
//                      (Phase3 to Terminal), unless the class is cleared or the FSM has failed
//                      ("Programmable Escalation Protocol": up to 4 delays, "each representing a
//                      distinct escalation phase (0 - 3)"; "SW can stop a triggered escalation
//                      protocol by clearing"; the waveforms run Phase0..Phase3 then Terminal)
//   EscTerminalHeld_A  Terminal is held until the class is cleared (the same sections; the
//                      documents name no other way out of Terminal)
//   AccuSaturates_A    the accumulator stays at its maximum until cleared ("The counter is a
//                      saturation counter, meaning that it will not wrap around once it hits the
//                      maximum representable count")
//   LockedNoClear_A    a class whose CLR_REGWEN is 0 cannot be cleared (CLASSx_CLR_REGWEN: "Clear
//                      enable for escalation protocol"; set to false by hardware "if the escalation
//                      protocol has been triggered and the bit CLASSA_CTRL_SHADOWED.LOCK is true")
//   PingFailLocal_C    a ping failure of an alert reaches the local alert cause (cover)
module alert_handler_eg2_fpv_props
  import alert_handler_pkg::*;
(
  input logic                     clk_i,
  input logic                     rst_ni,
  input cstate_e [3:0]            st,
  input logic    [3:0]            clr,
  input logic    [3:0][AccuCntDw-1:0] accu,
  input logic    [3:0]            clr_regwen,
  input logic                     ping_fail_alert,
  input logic                     loc_en0,
  input logic                     loc_cause0
);
  function automatic cstate_e next_phase(cstate_e s);
    unique case (s)
      Phase0: return Phase1;
      Phase1: return Phase2;
      Phase2: return Phase3;
      default: return Terminal;
    endcase
  endfunction

  for (genvar k = 0; k < 4; k++) begin : gen_class
    EscPhaseForward_A: assert property (@(posedge clk_i) disable iff (!rst_ni)
        (st[k] inside {Phase0, Phase1, Phase2, Phase3}) && !clr[k] |=>
        (st[k] == $past(st[k])) || (st[k] == next_phase($past(st[k]))) || (st[k] == FsmError));
    EscTerminalHeld_A: assert property (@(posedge clk_i) disable iff (!rst_ni)
        st[k] == Terminal && !clr[k] |=> st[k] inside {Terminal, FsmError});
    AccuSaturates_A: assert property (@(posedge clk_i) disable iff (!rst_ni)
        accu[k] == '1 && !clr[k] |=> accu[k] == '1);
    LockedNoClear_A: assert property (@(posedge clk_i) disable iff (!rst_ni)
        !clr_regwen[k] |-> !clr[k]);
    EscPhase_C: cover property (@(posedge clk_i) disable iff (!rst_ni) st[k] == Phase3);
    Terminal_C: cover property (@(posedge clk_i) disable iff (!rst_ni) st[k] == Terminal);
    AccuMax_C: cover property (@(posedge clk_i) disable iff (!rst_ni) accu[k] == '1);
  end
  PingFailLocal_C: cover property (@(posedge clk_i) disable iff (!rst_ni)
      ping_fail_alert && loc_en0 ##[1:3] loc_cause0);
endmodule

bind alert_handler alert_handler_eg2_fpv_props u_eg2_fpv_props (
  .clk_i,
  .rst_ni,
  .st({gen_classes[3].u_esc_timer.esc_state_o, gen_classes[2].u_esc_timer.esc_state_o,
       gen_classes[1].u_esc_timer.esc_state_o, gen_classes[0].u_esc_timer.esc_state_o}),
  .clr(reg2hw_wrap.class_clr),
  .accu({gen_classes[3].u_accu.accu_cnt_o, gen_classes[2].u_accu.accu_cnt_o,
         gen_classes[1].u_accu.accu_cnt_o, gen_classes[0].u_accu.accu_cnt_o}),
  .clr_regwen({u_reg_wrap.u_reg.classd_clr_regwen_qs, u_reg_wrap.u_reg.classc_clr_regwen_qs,
               u_reg_wrap.u_reg.classb_clr_regwen_qs, u_reg_wrap.u_reg.classa_clr_regwen_qs}),
  .ping_fail_alert(loc_alert_trig[0]),
  .loc_en0(reg2hw_wrap.loc_alert_en[0]),
  .loc_cause0(u_reg_wrap.u_reg.loc_alert_cause_0_qs)
);
