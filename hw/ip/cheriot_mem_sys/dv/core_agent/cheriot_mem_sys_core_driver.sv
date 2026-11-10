// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Drives the core data port as the core does: one request presented at a time, with its tag,
// held until it is taken; up to cfg.max_outstanding requests in flight. item_done() is called at
// the request's handshake, and the response (data, error, response tag) is put back when it comes.
class cheriot_mem_sys_core_driver extends dv_base_driver #(
    .ITEM_T(cheriot_mem_sys_core_item),
    .CFG_T (cheriot_mem_sys_core_agent_cfg)
  );
  `uvm_component_utils(cheriot_mem_sys_core_driver)

  // Requests taken by the subsystem and not answered yet.
  protected cheriot_mem_sys_core_item pending[$];

  `uvm_component_new

  virtual task get_and_drive();
    wait (cfg.vif.rst_n === 1'b1);
    @(cfg.vif.host_cb);
    fork
      a_channel_thread();
      d_channel_thread();
    join_none
  endtask

  virtual task on_enter_reset();
    cfg.vif.h2d.a_valid = 1'b0;
    cfg.vif.h2d.d_ready = 1'b0;
    cfg.vif.tag_h2d     = 1'b0;
    // Requests in flight get no response: hand them back so that no sequence waits for ever.
    foreach (pending[i]) begin
      pending[i].aborted = 1'b1;
      pending[i].done    = 1'b1;
      seq_item_port.put_response(pending[i]);
    end
    pending.delete();
  endtask

  // A request is driven from a clock edge. An item that was not waiting arrives at any time, so
  // the driver first waits for the next edge.
  protected task a_channel_thread();
    forever begin
      seq_item_port.try_next_item(req);
      if (req == null) begin
        seq_item_port.get_next_item(req);
        @(cfg.vif.host_cb);
      end
      drive_request(req);
      seq_item_port.item_done();
      if (req.aborted) begin
        req.done = 1'b1;
        seq_item_port.put_response(req);
      end
    end
  endtask

  // Present one request and hold it until it is taken (or a reset drops it).
  protected task drive_request(cheriot_mem_sys_core_item it);
    tlul_pkg::tl_h2d_t h2d;
    while (cfg.in_reset || pending.size() >= cfg.max_outstanding || !free_source(it.source)) begin
      @(cfg.vif.host_cb);
    end
    repeat ($urandom_range(cfg.a_valid_delay_max)) @(cfg.vif.host_cb);

    // d_ready belongs to d_channel_thread: the A channel fields are driven one by one.
    h2d                  = '0;
    h2d.a_opcode         = it.opcode;
    h2d.a_size           = it.size;
    h2d.a_source         = it.source;
    h2d.a_address        = it.addr;
    h2d.a_mask           = it.mask;
    h2d.a_data           = it.data;
    h2d.a_user           = tlul_pkg::TL_A_USER_DEFAULT;
    h2d.a_user.cmd_intg  = tlul_pkg::get_cmd_intg(h2d);
    h2d.a_user.data_intg = tlul_pkg::get_data_intg(it.data);
    cfg.vif.host_cb.h2d.a_opcode  <= h2d.a_opcode;
    cfg.vif.host_cb.h2d.a_param   <= '0;
    cfg.vif.host_cb.h2d.a_size    <= h2d.a_size;
    cfg.vif.host_cb.h2d.a_source  <= h2d.a_source;
    cfg.vif.host_cb.h2d.a_address <= h2d.a_address;
    cfg.vif.host_cb.h2d.a_mask    <= h2d.a_mask;
    cfg.vif.host_cb.h2d.a_data    <= h2d.a_data;
    cfg.vif.host_cb.h2d.a_user    <= h2d.a_user;
    cfg.vif.host_cb.h2d.a_valid   <= 1'b1;
    cfg.vif.host_cb.tag_h2d       <= it.tag;

    do begin
      @(cfg.vif.host_cb);
    end while (!cfg.vif.host_cb.d2h.a_ready && !cfg.in_reset);

    if (cfg.in_reset) begin
      it.aborted = 1'b1;
    end else begin
      pending.push_back(it);
    end
    cfg.vif.host_cb.h2d.a_valid <= 1'b0;
    cfg.vif.host_cb.tag_h2d     <= 1'b0;
  endtask

  // A source ID no request in flight uses.
  protected function bit free_source(output bit [top_pkg::TL_AIW-1:0] source);
    for (int unsigned s = 0; s < cfg.num_sources; s++) begin
      bit used = 1'b0;
      foreach (pending[i]) used |= (pending[i].source == s);
      if (!used) begin
        source = s;
        return 1'b1;
      end
    end
    return 1'b0;
  endfunction

  // d_ready is low for a random number of cycles, then high until a response is taken. A
  // response is taken at a clock edge with d_valid and d_ready both high; it goes back to the
  // request with the same source.
  protected task d_channel_thread();
    int unsigned stall = $urandom_range(cfg.d_ready_delay_max);
    bit          ready = 1'b0;
    forever begin
      if (!cfg.in_reset) begin
        ready = (stall == 0);
        if (stall > 0) stall--;
      end else begin
        ready = 1'b0;
      end
      cfg.vif.host_cb.h2d.d_ready <= ready;
      @(cfg.vif.host_cb);
      if (ready && cfg.vif.host_cb.d2h.d_valid && !cfg.in_reset) begin
        take_response();
        stall = $urandom_range(cfg.d_ready_delay_max);
      end
    end
  endtask

  protected function void take_response();
    tlul_pkg::tl_d2h_t d2h = cfg.vif.host_cb.d2h;
    foreach (pending[i]) begin
      if (pending[i].source == d2h.d_source) begin
        cheriot_mem_sys_core_item it = pending[i];
        pending.delete(i);
        it.rdata = d2h.d_data;
        it.err   = d2h.d_error;
        it.rtag  = cfg.vif.host_cb.tag_d2h;
        it.done  = 1'b1;
        seq_item_port.put_response(it);
        return;
      end
    end
    `uvm_error(`gfn, $sformatf("response with source %0d, which no request in flight has",
                               d2h.d_source))
  endfunction

endclass
