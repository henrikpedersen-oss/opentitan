// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_env_cfg extends cip_base_env_cfg #(.RAL_T(cheriot_mem_sys_regs_reg_block));

  // The agents besides the CIP ones (the `regs` port and the alert):
  // - the core on the core data port;
  // - TL-UL hosts on the core's revocation bitmap port and the software bitmap window;
  // - TL-UL devices on the host ports: the crossbar as the core and the revocation engine see it,
  //   and the meta SRAM.
  rand cheriot_mem_sys_core_agent_cfg m_core_agent_cfg;
  rand tl_agent_cfg                   m_corerevbm_agent_cfg;
  rand tl_agent_cfg                   m_revbm_agent_cfg;
  rand tl_agent_cfg                   m_cored_h_agent_cfg;
  rand tl_agent_cfg                   m_tbre_h_agent_cfg;
  rand tl_agent_cfg                   m_meta_agent_cfg;

  // The memories behind the host ports: the crossbar's (SRAM, NVM, untagged memory; the core and
  // the revocation engine see the same one) and the meta SRAM.
  mem_model#(.AddrWidth(32)) data_mem;
  mem_model#(.AddrWidth(32)) meta_mem;

  // The C reference model checks the subsystem (cheriot_mem_sys_scoreboard). The feature tests
  // turn it on; +cheriot_mem_sys_cmodel=1 turns it on for any test.
  bit          en_cmodel;
  // +cheriot_mem_sys_cmodel_corrupt=<n> hands the n-th compared read to the model with one bit
  // flipped: the _inject_fail test, which must fail.
  int unsigned cmodel_corrupt;

  // The device on cored_tl_h drives known (random) idle response fields, not X: the WTRC's d_ready
  // towards the crossbar depends on the response data while no response is valid (cheriot_wtrc.sv
  // tag_wr_req compares d_data without d_valid), which fails the DUT's CoredTlHDReadyKnown_A
  // (finding, status/rtl_tickets.md "cheriot_mem_sys: the WTRC's d_ready depends on an idle
  // response"). +cheriot_mem_sys_x_idle_rsp=1 drives X there again.
  bit x_idle_rsp_cored_h;
  constraint cored_h_idle_rsp_c { !x_idle_rsp_cored_h -> m_cored_h_agent_cfg.invalidate_d_x == 1'b0; }

  `uvm_object_utils_begin(cheriot_mem_sys_env_cfg)
    `uvm_field_object(m_core_agent_cfg,      UVM_DEFAULT)
    `uvm_field_object(m_corerevbm_agent_cfg, UVM_DEFAULT)
    `uvm_field_object(m_revbm_agent_cfg,     UVM_DEFAULT)
    `uvm_field_object(m_cored_h_agent_cfg,   UVM_DEFAULT)
    `uvm_field_object(m_tbre_h_agent_cfg,    UVM_DEFAULT)
    `uvm_field_object(m_meta_agent_cfg,      UVM_DEFAULT)
    `uvm_field_int   (en_cmodel,             UVM_DEFAULT)
  `uvm_object_utils_end

  `uvm_object_new

  virtual function void initialize(bit inherit_ral_models = 1'b0);
    list_of_alerts = cheriot_mem_sys_env_pkg::LIST_OF_ALERTS;
    super.initialize(inherit_ral_models);
    num_interrupts = ral.intr_state.get_n_used_bits();

    m_core_agent_cfg = cheriot_mem_sys_core_agent_cfg::type_id::create("m_core_agent_cfg");
    m_core_agent_cfg.if_mode = dv_utils_pkg::Host;

    m_corerevbm_agent_cfg = new_tl_agent_cfg("m_corerevbm_agent_cfg", dv_utils_pkg::Host);
    m_revbm_agent_cfg     = new_tl_agent_cfg("m_revbm_agent_cfg",     dv_utils_pkg::Host);
    m_cored_h_agent_cfg   = new_tl_agent_cfg("m_cored_h_agent_cfg",   dv_utils_pkg::Device);
    m_tbre_h_agent_cfg    = new_tl_agent_cfg("m_tbre_h_agent_cfg",    dv_utils_pkg::Device);
    m_meta_agent_cfg      = new_tl_agent_cfg("m_meta_agent_cfg",      dv_utils_pkg::Device);

    data_mem = mem_model#(.AddrWidth(32))::type_id::create("data_mem");
    meta_mem = mem_model#(.AddrWidth(32))::type_id::create("meta_mem");

    void'($value$plusargs("cheriot_mem_sys_cmodel=%b", en_cmodel));
    void'($value$plusargs("cheriot_mem_sys_cmodel_corrupt=%d", cmodel_corrupt));
    void'($value$plusargs("cheriot_mem_sys_x_idle_rsp=%b", x_idle_rsp_cored_h));
  endfunction

  // A host holds a request until it is taken, one in flight; a device takes up to 16.
  protected function tl_agent_cfg new_tl_agent_cfg(string name, dv_utils_pkg::if_mode_e mode);
    tl_agent_cfg c = tl_agent_cfg::type_id::create(name);
    c.if_mode = mode;
    c.is_active = 1'b1;
    c.max_outstanding_req = (mode == dv_utils_pkg::Host) ? 1 : 16;
    c.allow_a_valid_drop_wo_a_ready = 1'b0;
    // The subsystem's sockets take the top bits of the source ID (tlul_socket_m1 idInRange): the
    // bitmap window passes the 4:1 meta SRAM socket (2 bits). 5-bit sources fit every path.
    if (mode == dv_utils_pkg::Host) c.valid_a_source_width = 5;
    return c;
  endfunction

endclass
