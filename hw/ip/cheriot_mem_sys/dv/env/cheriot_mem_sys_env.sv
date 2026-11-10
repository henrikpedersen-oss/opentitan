// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The agents drive and watch every port. Their monitors feed the reference model (with
// cfg.en_cmodel) and the scoreboard; the reference model sends the scoreboard what it expects.
// The sequences reach only the sequencers and the memory backdoor.
class cheriot_mem_sys_env extends cip_base_env #(
    .CFG_T              (cheriot_mem_sys_env_cfg),
    .COV_T              (cheriot_mem_sys_env_cov),
    .VIRTUAL_SEQUENCER_T(cheriot_mem_sys_virtual_sequencer),
    .SCOREBOARD_T       (cheriot_mem_sys_scoreboard)
  );
  `uvm_component_utils(cheriot_mem_sys_env)

  cheriot_mem_sys_core_agent   m_core_agent;       // host on cored_tl_d, with the tag sideband
  tl_agent                     m_corerevbm_agent;  // host on corerevbm_tl
  tl_agent                     m_revbm_agent;      // host on revbm_tl_d
  tl_agent                     m_cored_h_agent;    // device on cored_tl_h
  tl_agent                     m_tbre_h_agent;     // device on tbre_tl_h
  tl_agent                     m_meta_agent;       // device on meta_sram_tl
  cheriot_mem_sys_mem_backdoor m_mem_backdoor;     // the backdoor of the crossbar's memory
  cheriot_mem_sys_ref_model    m_ref_model;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    m_core_agent = cheriot_mem_sys_core_agent::type_id::create("m_core_agent", this);
    uvm_config_db#(cheriot_mem_sys_core_agent_cfg)::set(this, "m_core_agent*", "cfg",
                                                        cfg.m_core_agent_cfg);
    cfg.m_core_agent_cfg.en_cov = cfg.en_cov;

    m_corerevbm_agent = create_tl_agent("m_corerevbm_agent", cfg.m_corerevbm_agent_cfg);
    m_revbm_agent     = create_tl_agent("m_revbm_agent",     cfg.m_revbm_agent_cfg);
    m_cored_h_agent   = create_tl_agent("m_cored_h_agent",   cfg.m_cored_h_agent_cfg);
    m_tbre_h_agent    = create_tl_agent("m_tbre_h_agent",    cfg.m_tbre_h_agent_cfg);
    m_meta_agent      = create_tl_agent("m_meta_agent",      cfg.m_meta_agent_cfg);

    m_mem_backdoor = cheriot_mem_sys_mem_backdoor::type_id::create("m_mem_backdoor", this);
    m_mem_backdoor.mem = cfg.data_mem;

    if (cfg.en_cmodel) begin
      m_ref_model = cheriot_mem_sys_ref_model::type_id::create("m_ref_model", this);
      m_ref_model.cfg = cfg;
    end
  endfunction

  protected function tl_agent create_tl_agent(string name, tl_agent_cfg agent_cfg);
    uvm_config_db#(tl_agent_cfg)::set(this, {name, "*"}, "cfg", agent_cfg);
    agent_cfg.en_cov = cfg.en_cov;
    return tl_agent::type_id::create(name, this);
  endfunction

  function void connect_phase(uvm_phase phase);
    tl_monitor regs_monitor = m_tl_agents[cheriot_mem_sys_regs_reg_block::type_name].monitor;
    super.connect_phase(phase);
    virtual_sequencer.core_sqr      = m_core_agent.sequencer;
    virtual_sequencer.corerevbm_sqr = m_corerevbm_agent.sequencer;
    virtual_sequencer.revbm_sqr     = m_revbm_agent.sequencer;
    virtual_sequencer.cored_h_sqr   = m_cored_h_agent.sequencer;
    virtual_sequencer.tbre_h_sqr    = m_tbre_h_agent.sequencer;
    virtual_sequencer.meta_sqr      = m_meta_agent.sequencer;
    virtual_sequencer.mem_backdoor  = m_mem_backdoor;

    // The scoreboard: what the monitors observe
    m_core_agent.monitor.analysis_port.connect(scoreboard.core_fifo.analysis_export);
    m_core_agent.monitor.req_port.connect(scoreboard.core_req_imp);
    m_core_agent.monitor.mode_port.connect(scoreboard.mode_imp);
    m_revbm_agent.monitor.d_chan_port.connect(scoreboard.revbm_fifo.analysis_export);
    m_corerevbm_agent.monitor.d_chan_port.connect(scoreboard.corerevbm_fifo.analysis_export);
    regs_monitor.d_chan_port.connect(scoreboard.regs_fifo.analysis_export);
    m_tbre_h_agent.monitor.a_chan_port.connect(scoreboard.tbre_h_req_imp);
    m_meta_agent.monitor.a_chan_port.connect(scoreboard.meta_req_imp);
    m_meta_agent.monitor.d_chan_port.connect(scoreboard.meta_rsp_imp);
    m_revbm_agent.monitor.a_chan_port.connect(scoreboard.activity_imp);
    m_corerevbm_agent.monitor.a_chan_port.connect(scoreboard.activity_imp);
    m_cored_h_agent.monitor.a_chan_port.connect(scoreboard.activity_imp);
    m_cored_h_agent.monitor.d_chan_port.connect(scoreboard.activity_imp);
    m_tbre_h_agent.monitor.d_chan_port.connect(scoreboard.activity_imp);
    regs_monitor.a_chan_port.connect(scoreboard.activity_imp);

    if (m_ref_model == null) return;
    scoreboard.ref_model = m_ref_model;

    // The reference model: what the monitors observe, and the memory backdoor
    m_core_agent.monitor.req_port.connect(m_ref_model.core_req_imp);
    m_core_agent.monitor.analysis_port.connect(m_ref_model.core_rsp_imp);
    m_core_agent.monitor.analysis_port.connect(m_ref_model.core_fifo.analysis_export);
    m_core_agent.monitor.mode_port.connect(m_ref_model.mode_imp);
    m_revbm_agent.monitor.a_chan_port.connect(m_ref_model.window_req_imp);
    m_corerevbm_agent.monitor.a_chan_port.connect(m_ref_model.window_req_imp);
    m_revbm_agent.monitor.d_chan_port.connect(m_ref_model.revbm_fifo.analysis_export);
    m_corerevbm_agent.monitor.d_chan_port.connect(m_ref_model.corerevbm_fifo.analysis_export);
    regs_monitor.d_chan_port.connect(m_ref_model.regs_fifo.analysis_export);
    m_cored_h_agent.monitor.d_chan_port.connect(m_ref_model.cored_h_rsp_imp);
    m_tbre_h_agent.monitor.d_chan_port.connect(m_ref_model.tbre_h_rsp_imp);
    m_meta_agent.monitor.d_chan_port.connect(m_ref_model.meta_rsp_imp);
    m_mem_backdoor.analysis_port.connect(m_ref_model.backdoor_imp);

    // The scoreboard: what the reference model expects
    m_ref_model.core_exp_port.connect(scoreboard.core_exp_fifo.analysis_export);
    m_ref_model.revbm_exp_port.connect(scoreboard.revbm_exp_fifo.analysis_export);
    m_ref_model.corerevbm_exp_port.connect(scoreboard.corerevbm_exp_fifo.analysis_export);
    m_ref_model.csr_exp_port.connect(scoreboard.csr_exp_fifo.analysis_export);
    m_ref_model.alert_exp_port.connect(scoreboard.alert_exp_fifo.analysis_export);
  endfunction

endclass
