// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_env extends cip_base_env #(
    .CFG_T              (cheriot_mem_sys_env_cfg),
    .COV_T              (cheriot_mem_sys_env_cov),
    .VIRTUAL_SEQUENCER_T(cheriot_mem_sys_virtual_sequencer),
    .SCOREBOARD_T       (cheriot_mem_sys_scoreboard)
  );
  `uvm_component_utils(cheriot_mem_sys_env)

  // The memory side (cheriot_mem_sys_env_cfg).
  tl_agent m_cored_agent;      // host on cored_tl_d (with the tag sideband, cfg.side_vif)
  tl_agent m_corerevbm_agent;  // host on corerevbm_tl
  tl_agent m_revbm_agent;      // host on revbm_tl_d
  tl_agent m_cored_h_agent;    // device on cored_tl_h
  tl_agent m_tbre_h_agent;     // device on tbre_tl_h
  tl_agent m_meta_agent;       // device on meta_sram_tl

  cheriot_mem_sys_cmodel_sb m_cmodel_sb;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (cfg.side_vif == null &&
        !uvm_config_db#(virtual cheriot_mem_sys_side_if)::get(this, "", "side_vif", cfg.side_vif)) begin
      `uvm_fatal(`gfn, "failed to get side_vif")
    end
    m_cored_agent     = create_agent("m_cored_agent",     cfg.m_cored_agent_cfg);
    m_corerevbm_agent = create_agent("m_corerevbm_agent", cfg.m_corerevbm_agent_cfg);
    m_revbm_agent     = create_agent("m_revbm_agent",     cfg.m_revbm_agent_cfg);
    m_cored_h_agent   = create_agent("m_cored_h_agent",   cfg.m_cored_h_agent_cfg);
    m_tbre_h_agent    = create_agent("m_tbre_h_agent",    cfg.m_tbre_h_agent_cfg);
    m_meta_agent      = create_agent("m_meta_agent",      cfg.m_meta_agent_cfg);
    if (cfg.en_cmodel) begin
      m_cmodel_sb = cheriot_mem_sys_cmodel_sb::type_id::create("m_cmodel_sb", this);
      m_cmodel_sb.cfg = cfg;
    end
  endfunction

  protected function tl_agent create_agent(string name, tl_agent_cfg agent_cfg);
    tl_agent a = tl_agent::type_id::create(name, this);
    uvm_config_db#(tl_agent_cfg)::set(this, {name, "*"}, "cfg", agent_cfg);
    agent_cfg.en_cov = cfg.en_cov;
    return a;
  endfunction

  function void connect_phase(uvm_phase phase);
    super.connect_phase(phase);
    virtual_sequencer.cored_sqr     = m_cored_agent.sequencer;
    virtual_sequencer.corerevbm_sqr = m_corerevbm_agent.sequencer;
    virtual_sequencer.revbm_sqr     = m_revbm_agent.sequencer;
    virtual_sequencer.cored_h_sqr   = m_cored_h_agent.sequencer;
    virtual_sequencer.tbre_h_sqr    = m_tbre_h_agent.sequencer;
    virtual_sequencer.meta_sqr      = m_meta_agent.sequencer;
    if (cfg.en_cmodel) begin
      m_cored_agent.monitor.d_chan_port.connect(m_cmodel_sb.cored_fifo.analysis_export);
      m_corerevbm_agent.monitor.d_chan_port.connect(m_cmodel_sb.corerevbm_fifo.analysis_export);
      m_revbm_agent.monitor.d_chan_port.connect(m_cmodel_sb.revbm_fifo.analysis_export);
      m_cored_h_agent.monitor.d_chan_port.connect(m_cmodel_sb.cored_h_fifo.analysis_export);
      m_tl_agents[cheriot_mem_sys_regs_reg_block::type_name].monitor.d_chan_port.connect(
          m_cmodel_sb.regs_fifo.analysis_export);
      begin
        // The CIP environment keeps its alert agents local: found by name.
        alert_agent_pkg::alert_agent fatal_agent;
        if (!$cast(fatal_agent, lookup("m_alert_agent_fatal_fault"))) begin
          `uvm_fatal(`gfn, "no alert agent m_alert_agent_fatal_fault")
        end
        fatal_agent.m_alert_port.connect(m_cmodel_sb.alert_fifo.analysis_export);
      end
    end
  endfunction

endclass
