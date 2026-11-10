// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_env_cfg extends cip_base_env_cfg #(.RAL_T(cheriot_mem_sys_regs_reg_block));

  // The memory side (besides the `regs` port of the RAL): TL-UL hosts on the subsystem's device
  // ports (the core data port, the core's revocation bitmap port, the bitmap window) and TL-UL
  // devices answering its host ports (towards the main crossbar for the core and the revocation
  // engine, towards the meta SRAM).
  rand tl_agent_cfg m_cored_agent_cfg;
  rand tl_agent_cfg m_corerevbm_agent_cfg;
  rand tl_agent_cfg m_revbm_agent_cfg;
  rand tl_agent_cfg m_cored_h_agent_cfg;
  rand tl_agent_cfg m_tbre_h_agent_cfg;
  rand tl_agent_cfg m_meta_agent_cfg;

  // The tag sideband of the core data port, and cheriot_ena_i.
  virtual cheriot_mem_sys_side_if side_vif;

  // The memories behind the host ports: the main crossbar's (SRAM, NVM, untagged memory; the core
  // and the revocation engine see the same one) and the meta SRAM's.
  mem_model_pkg::mem_model#(.AddrWidth(32)) data_mem;
  mem_model_pkg::mem_model#(.AddrWidth(32)) meta_mem;

  // Check the subsystem against its C reference model (cheriot_mem_sys_cmodel_sb):
  // +cheriot_mem_sys_cmodel=1. +cheriot_mem_sys_cmodel_corrupt=<n> hands the n-th compared read to
  // the model with one bit flipped (the _inject_fail test: the check must fail).
  bit          en_cmodel = 1'b0;
  chandle      cmodel_h;  // the model, once cheriot_mem_sys_cmodel_sb has built it
  // fatal_fault alerts the alert agent saw (not its pings), counted by cheriot_mem_sys_cmodel_sb.
  int unsigned fatal_alert_cnt;
  // The next n core transactions carry an error the test injected on the meta path: the model is
  // told (meta_err), as the cms testbench's scoreboard was (sb.exp_core_err).
  int unsigned cm_exp_core_err;
  int unsigned cmodel_corrupt = 0;

  // The device on cored_tl_h drives known (random) idle response fields, not X: the WTRC's d_ready
  // towards the crossbar depends on the response data while no response is valid (cheriot_wtrc.sv
  // tag_wr_req compares d_data without d_valid), which fails the DUT's CoredTlHDReadyKnown_A
  // (finding, status/rtl_tickets.md "cheriot_mem_sys: the WTRC's d_ready depends on an idle
  // response"). +cheriot_mem_sys_x_idle_rsp=1 drives X there again.
  bit x_idle_rsp_cored_h = 1'b0;
  constraint cored_h_idle_rsp_c { !x_idle_rsp_cored_h -> m_cored_h_agent_cfg.invalidate_d_x == 1'b0; }

  `uvm_object_utils_begin(cheriot_mem_sys_env_cfg)
    `uvm_field_object(m_cored_agent_cfg,     UVM_DEFAULT)
    `uvm_field_object(m_corerevbm_agent_cfg, UVM_DEFAULT)
    `uvm_field_object(m_revbm_agent_cfg,     UVM_DEFAULT)
    `uvm_field_object(m_cored_h_agent_cfg,   UVM_DEFAULT)
    `uvm_field_object(m_tbre_h_agent_cfg,    UVM_DEFAULT)
    `uvm_field_object(m_meta_agent_cfg,      UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new

  virtual function void initialize(bit inherit_ral_models = 1'b0);
    list_of_alerts = cheriot_mem_sys_env_pkg::LIST_OF_ALERTS;
    super.initialize(inherit_ral_models);
    num_interrupts = ral.intr_state.get_n_used_bits();

    m_cored_agent_cfg     = new_agent_cfg("m_cored_agent_cfg",     dv_utils_pkg::Host, 2);
    m_corerevbm_agent_cfg = new_agent_cfg("m_corerevbm_agent_cfg", dv_utils_pkg::Host, 1);
    m_revbm_agent_cfg     = new_agent_cfg("m_revbm_agent_cfg",     dv_utils_pkg::Host, 1);
    m_cored_h_agent_cfg   = new_agent_cfg("m_cored_h_agent_cfg",   dv_utils_pkg::Device, 16);
    m_tbre_h_agent_cfg    = new_agent_cfg("m_tbre_h_agent_cfg",    dv_utils_pkg::Device, 16);
    m_meta_agent_cfg      = new_agent_cfg("m_meta_agent_cfg",      dv_utils_pkg::Device, 16);

    data_mem = mem_model_pkg::mem_model#(.AddrWidth(32))::type_id::create("data_mem");
    meta_mem = mem_model_pkg::mem_model#(.AddrWidth(32))::type_id::create("meta_mem");

    void'($value$plusargs("cheriot_mem_sys_cmodel=%b", en_cmodel));
    void'($value$plusargs("cheriot_mem_sys_cmodel_corrupt=%d", cmodel_corrupt));
    void'($value$plusargs("cheriot_mem_sys_x_idle_rsp=%b", x_idle_rsp_cored_h));
  endfunction

  // A host on the core side holds a request until it is taken (the core does), with up to
  // `outstanding` in flight.
  protected function tl_agent_cfg new_agent_cfg(string name, dv_utils_pkg::if_mode_e mode,
                                                int unsigned outstanding);
    tl_agent_cfg c = tl_agent_cfg::type_id::create(name);
    c.if_mode = mode;
    c.is_active = 1'b1;
    c.max_outstanding_req = outstanding;
    c.allow_a_valid_drop_wo_a_ready = 1'b0;
    // The subsystem's TL-UL sockets take the top bits of the source ID (tlul_socket_m1 idInRange):
    // the core's tag requests pass the 2:1 tag socket (1 bit) and then the 4:1 meta SRAM socket
    // (2 bits), and the bitmap window goes straight to the latter. 5-bit sources fit every path.
    if (mode == dv_utils_pkg::Host) c.valid_a_source_width = 5;
    return c;
  endfunction

endclass
