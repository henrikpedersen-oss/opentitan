// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Watches the core data port. It publishes:
// - req_port: each request at its handshake, with its tag and the mode at that moment;
// - analysis_port: each completed access, the request with its response and the response's tag
//   (responses come back in order per source ID);
// - mode_port: each change of cheriot_ena_i.
// It also keeps cfg.in_reset.
class cheriot_mem_sys_core_monitor extends dv_base_monitor #(
    .ITEM_T(cheriot_mem_sys_core_item),
    .CFG_T (cheriot_mem_sys_core_agent_cfg)
  );
  `uvm_component_utils(cheriot_mem_sys_core_monitor)

  uvm_analysis_port #(cheriot_mem_sys_core_item) req_port;
  uvm_analysis_port #(prim_mubi_pkg::mubi4_t)    mode_port;

  protected cheriot_mem_sys_core_item in_flight[int unsigned][$];

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    req_port  = new("req_port", this);
    mode_port = new("mode_port", this);
  endfunction

  virtual task run_phase(uvm_phase phase);
    fork
      track_reset();
      track_mode();
      super.run_phase(phase);
    join
  endtask

  protected task track_reset();
    forever begin
      cfg.in_reset = (cfg.vif.rst_n !== 1'b1);
      if (cfg.in_reset) in_flight.delete();
      @(cfg.vif.rst_n);
    end
  endtask

  protected task track_mode();
    forever begin
      mode_port.write(cfg.vif.cheriot_ena);
      @(cfg.vif.cheriot_ena);
    end
  endtask

  virtual protected task collect_trans();
    forever begin
      @(cfg.vif.mon_cb);
      if (cfg.in_reset) continue;
      if (cfg.vif.mon_cb.h2d.a_valid && cfg.vif.mon_cb.d2h.a_ready) collect_request();
      if (cfg.vif.mon_cb.d2h.d_valid && cfg.vif.mon_cb.h2d.d_ready) collect_response();
    end
  endtask

  protected function void collect_request();
    tlul_pkg::tl_h2d_t        h2d = cfg.vif.mon_cb.h2d;
    cheriot_mem_sys_core_item it  = cheriot_mem_sys_core_item::type_id::create("it");
    it.opcode = tlul_pkg::tl_a_op_e'(h2d.a_opcode);
    it.addr   = h2d.a_address;
    it.size   = h2d.a_size;
    it.mask   = h2d.a_mask;
    it.data   = h2d.a_data;
    it.source = h2d.a_source;
    it.tag    = cfg.vif.mon_cb.tag_h2d;
    it.ena    = cfg.vif.mon_cb.cheriot_ena;
    in_flight[it.source].push_back(it);
    begin
      cheriot_mem_sys_core_item req;
      `downcast(req, it.clone())
      req_port.write(req);
    end
  endfunction

  protected function void collect_response();
    tlul_pkg::tl_d2h_t        d2h = cfg.vif.mon_cb.d2h;
    cheriot_mem_sys_core_item it;
    if (!in_flight.exists(d2h.d_source) || in_flight[d2h.d_source].size() == 0) begin
      `uvm_error(`gfn, $sformatf("response with source %0d, which no request in flight has",
                                 d2h.d_source))
      return;
    end
    it       = in_flight[d2h.d_source].pop_front();
    it.rdata = d2h.d_data;
    it.err   = d2h.d_error;
    it.rtag  = cfg.vif.mon_cb.tag_d2h;
    it.done  = 1'b1;
    `uvm_info(`gfn, it.convert2string(), UVM_HIGH)
    analysis_port.write(it);
  endfunction

endclass
