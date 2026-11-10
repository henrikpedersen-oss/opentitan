// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class pwrmgr_env_cfg extends cip_base_env_cfg #(
  .RAL_T(pwrmgr_reg_block)
);

  // disable fault csr read check from scoreboard
  bit disable_csr_rd_chk = 0;

  // Invalid state test. Used to disable interrupt check.
  bit invalid_st_test = 0;

  // Set by check_and_clear_interrupt(.check_expected(0)) while it reads INTR_STATE: the sequence
  // makes no prediction for that read, so the scoreboard does not compare it (EG2 decision
  // 2026-11-08: the documents do not define the wakeup interrupt after a reset ends low power, a
  // fall through or an abort; the C-model scoreboard checks it within the documented bounds).
  bit intr_not_predicted = 0;

  // Also check pwrmgr against the power manager reference model (pwrmgr_cmodel_sb): +pwrmgr_cmodel=1.
  bit en_cmodel = 1'b0;

  // ext component cfgs
  esc_agent_cfg m_esc_agent_cfg;

  `uvm_object_utils_begin(pwrmgr_env_cfg)
  `uvm_object_utils_end

  `uvm_object_new

  // ext interfaces
  virtual clk_rst_if esc_clk_rst_vif;
  virtual clk_rst_if lc_clk_rst_vif;
  virtual clk_rst_if slow_clk_rst_vif;
  virtual pwrmgr_if pwrmgr_vif;
  virtual pwrmgr_clock_enables_sva_if pwrmgr_clock_enables_sva_vif;
  virtual pwrmgr_rstmgr_sva_if #(.PowerDomains(pwrmgr_pkg::PowerDomains)) pwrmgr_rstmgr_sva_vif;

  // The run_phase object, to deal with objections.
  uvm_phase run_phase;

  virtual function void initialize(bit inherit_ral_models = 1'b0);
    list_of_alerts = pwrmgr_env_pkg::LIST_OF_ALERTS;
    super.initialize(inherit_ral_models);
    num_interrupts = ral.intr_state.get_n_used_bits();
    `ASSERT_I(NumInstrMatch_A, num_interrupts == NUM_INTERRUPTS)
    `uvm_info(`gfn, $sformatf("num_interrupts = %0d", num_interrupts), UVM_MEDIUM)

    // pwrmgr_tl_intg_err test uses default alert name "fata_fault"
    // and it requires following field to be '1'
    tl_intg_alert_fields[ral.fault_status.reg_intg_err] = 1;
    m_tl_agent_cfg.max_outstanding_req = 1;
    m_esc_agent_cfg = esc_agent_cfg::type_id::create("m_esc_agent_cfg");
    `DV_CHECK_RANDOMIZE_FATAL(m_esc_agent_cfg)
    // Disable escalation ping coverage.
    m_esc_agent_cfg.en_ping_cov = 0;
    void'($value$plusargs("pwrmgr_cmodel=%b", en_cmodel));
  endfunction

endclass
