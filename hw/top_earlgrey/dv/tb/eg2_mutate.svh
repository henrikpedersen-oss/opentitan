// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// EG2 mutations (included in module tb). `+eg2_mutate=<name>` plants one fault from
// hw/top_earlgrey/dv/eg2_mutations.hjson for the whole run, so the regression can be shown to
// notice it. Each mutation:
//   - forces one signal inside the consumer it breaks, never a port: ports collapse into one net
//     in the simulator, and forcing one can change what the driver's side sees as well;
//   - reaches both lockstep cores when it breaks the core's view (the lockstep check would
//     otherwise catch it first and hide whether the intended check works);
//   - counts the clock cycles in which it changed the design's behaviour. The log line
//     `EG2_MUTATION <name> active <n>` at the end is what the grader reads: n = 0 is INACTIVE.
// An unknown name stops the run, so a typo cannot pass as a missed mutation.

string       eg2_mut_name;
bit          eg2_mut_on;
int unsigned eg2_mut_active;

`define EG2_SWITCH  `CPU_HIER.u_cheriot_switch
`define EG2_TF      `PD_MAIN_HIER.u_cheriot_mem_sys.u_cheriot_tag_filter
`define EG2_XPERI   `PD_MAIN_HIER.u_xbar_peri
`define EG2_S1N     `EG2_XPERI.u_s1n_25

// rv_core_ibex_cheriot_switch's sparse FSM encoding.
localparam logic [5:0] Eg2SwUnlocked  = 6'b010110;
localparam logic [5:0] Eg2SwLockedEna = 6'b100101;
localparam logic [5:0] Eg2SwLockedDis = 6'b111011;

initial begin : eg2_mutate
  if ($value$plusargs("eg2_mutate=%s", eg2_mut_name)) begin
    eg2_mut_on = 1'b1;
    $display("EG2_MUTATION %s applied", eg2_mut_name);
    case (eg2_mut_name)
      // The core, its shadow and the TRVK stay in RISC-V mode while cheriot_mem_sys sees CHERIoT.
      "cheriot_ena_core_stuck_false":
        force `CPU_HIER.cheriot_enable_ibex = ibex_pkg::IbexMuBiOff;
      // A locked switch takes a second lock: CHERIOT_ENA stays writable after CHERIOT_LOCK.
      "cheriot_lock_ignored":
        // Forced at the sparse flop's plain-bits input: switch_state_d is an enum.
        force `EG2_SWITCH.u_state_regs.u_state_flop.d_i =
            (`EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis} &&
             `EG2_SWITCH.lock_access_i) ?
                (prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.ena_i) ? Eg2SwLockedEna
                                                                           : Eg2SwLockedDis) :
            (`EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis}) ?
                `EG2_SWITCH.switch_state_q :
            (`EG2_SWITCH.switch_state_q == Eg2SwUnlocked && `EG2_SWITCH.lock_access_i) ?
                ((prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.lock_i) &&
                  prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.ena_i)) ? Eg2SwLockedEna :
                 (prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.lock_i) &&
                  prim_mubi_pkg::mubi4_test_false_strict(`EG2_SWITCH.ena_i)) ? Eg2SwLockedDis :
                 `EG2_SWITCH.switch_state_q) :
            `EG2_SWITCH.switch_state_q;
      // Every load returns tag 0 to both cores.
      "load_tag_dropped":
        force `CPU_HIER.main_core_data_tag_d2h = 1'b0;
      // A capability store writes tag 0: only the TBRE's tag requests keep theirs.
      "store_tag_dropped":
        force `EG2_TF.tag_m_o = `EG2_TF.cap_tag_req.a_valid;
      // UART1's device select routed to UART2 (device indices of xbar_peri: uart0 = 0, ...).
      "xbar_peri_uart1_to_uart2":
        force `EG2_S1N.dev_select_t = (`EG2_S1N.dev_select_i == 5'd1) ? 5'd2
                                                                      : `EG2_S1N.dev_select_i;
      // An unmapped address answered without an error.
      "xbar_peri_unmapped_no_error":
        force `EG2_S1N.gen_err_resp.err_resp.tl_h_o.d_error = 1'b0;
      default:
        $fatal(1, "EG2_MUTATION unknown mutation '%s' (hw/top_earlgrey/dv/eg2_mutations.hjson)",
               eg2_mut_name);
    endcase
  end
end

// Activation: the cycles in which the mutant changed what the design does.
always @(posedge `CPU_HIER.clk_i) begin
  if (eg2_mut_on) begin
    case (eg2_mut_name)
      "cheriot_ena_core_stuck_false":
        if (`CPU_HIER.cheriot_ena_o == prim_mubi_pkg::MuBi4True) eg2_mut_active++;
      "cheriot_lock_ignored":
        if (`EG2_SWITCH.lock_access_i &&
            `EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis}) eg2_mut_active++;
      "load_tag_dropped":
        if (`CPU_HIER.cored_tag_d2h_i) eg2_mut_active++;
      "store_tag_dropped":
        if (`EG2_TF.tl_d_i.a_valid && `EG2_TF.tag_d_i && `EG2_TF.tl_d_is_write &&
            !`EG2_TF.cap_tag_req.a_valid)
          eg2_mut_active++;
      default: ;
    endcase
  end
end

// The crossbar runs on its own clock.
always @(posedge `EG2_XPERI.clk_peri_i) begin
  if (eg2_mut_on) begin
    case (eg2_mut_name)
      "xbar_peri_uart1_to_uart2":
        if (`EG2_S1N.tl_h_i.a_valid && `EG2_S1N.dev_select_i == 5'd1) eg2_mut_active++;
      "xbar_peri_unmapped_no_error":
        if (`EG2_S1N.gen_err_resp.err_resp.tl_h_o.d_valid) eg2_mut_active++;
      default: ;
    endcase
  end
end

final begin
  if (eg2_mut_on) $display("EG2_MUTATION %s active %0d", eg2_mut_name, eg2_mut_active);
end

`undef EG2_SWITCH
`undef EG2_TF
`undef EG2_XPERI
`undef EG2_S1N
