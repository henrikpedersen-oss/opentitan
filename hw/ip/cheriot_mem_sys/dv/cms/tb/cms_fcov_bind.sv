// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Binds the CHERIoT memory subsystem covergroups of the RTOS test SoC
// (../cheriot-rtos-test-suite/dv/cov/cheriot_mem_subsys_fcov.sv) and the TBRE's TRVK covergroup
// (../uvm/core_ibex/fcov/core_ibex_trvk_fcov_if.sv) into the unmodified RTL of this block-level
// bench, so both flows sample the same covergroups. The bind statements are those of
// ../cheriot-rtos-test-suite/dv/cov/cheriot_mem_subsys_fcov_bind.sv (lines 16-160) minus the two
// whose targets (ibex_trvk, cheriot_rev_ctl_tbre) are not in this bench: keep them in step.
// Sampled with +enable_ibex_fcov=1.
module cms_fcov_bind;

  bind cheriot_tag_filter cheriot_tag_filter_fcov_if u_tag_filter_fcov (
    .clk_i,
    .rst_ni,
    .tag_only_writes(TagOnlyWrites),
    .req_fire       (tl_d_i.a_valid && tl_d_o.a_ready),
    .req_is_read    (tl_d_is_read),
    .req_is_write   (tl_d_is_write),
    .req_is_partial (tl_d_i.a_opcode == tlul_pkg::PutPartialData),
    .req_aligned    (tl_d_is_aligned),
    .req_cap_hint   (tag_d_i),
    .req_in_sram    (tl_d_i.a_address >= MainSramBaseAddr && tl_d_i.a_address < MainSramTopAddr),
    .req_in_nvm     (tl_d_i.a_address >= NvmBaseAddr && tl_d_i.a_address < NvmTopAddr),
    .lookup         (require_lookup),
    .cheriot_on     (prim_mubi_pkg::mubi4_test_true_strict(cheriot_ena_i)),
    .nvm_cap_store  (cap_store),
    .meta_stall     (meta_req_valid && !meta_req_ready),
    .rsp_fire       (tl_d_o.d_valid && tl_d_i.d_ready),
    .rsp_lookup     (meta_rsp.lookup),
    .rsp_aligned    (meta_rsp.aligned),
    .rsp_tag        (tag_d_o),
    .rsp_err        (tl_d_o.d_error),
    .rsp_meta_err   (meta_buf_err)
  );

  bind cheriot_rmw_filter cheriot_rmw_filter_fcov_if u_rmw_filter_fcov (
    .clk_i,
    .rst_ni,
    .state_q      (state_q),
    .fill_rsp     (state_q == Fill && tl_d_i.d_valid),
    .old_tag      (tl_d_i.d_data[bit_sel_q]),
    .new_tag      (tag_q),
    .fill_err     (tl_d_i.d_error),
    .bit_sel      (bit_sel_q),
    .data_intg_err(data_intg_error_o),
    .rsp_intg_err (rsp_intg_error_o),
    .device_err   (device_error_o),
    .req_on_read_rsp(req_done && is_read_q)
  );

  bind cheriot_tbre_mover cheriot_tbre_mover_fcov_if u_tbre_mover_fcov (
    .clk_i,
    .rst_ni,
    .sweep_start     (start),
    .num_words       (32'(num_words_i)),
    .running         (state_q == Running),
    .busy            (busy_o),
    .join_fire       (write_a_possible && write_a_ready),
    .second_word     (waddr_fifo_out[WordOffsetW]),
    .first_tag       (write_rtag_q),
    .rtag            (payload_fifo_out.rtag),
    .rerr            (payload_fifo_out.rerr),
    .stale           (track_stale_q[track_rptr]),
    .invalidate      (write_a_valid),
    .inflight        (4'(inflight_q)),
    .errs            ({read_d_err, tl_err.read_tl_intg, tl_err.write_tl, tl_err.write_tl_intg}),
    .snoop_read_hit  (tl_r_o.a_valid && !tl_r_o.a_address[WordOffsetW] && snoop_read_hit),
    .snoop_track_mark(|(snoop_track_hit & ~track_frozen)),
    .snoop_frozen_hit(|(snoop_track_hit & track_frozen))
  );

  bind cheriot_access_check cheriot_access_check_fcov_if u_access_check_fcov (
    .clk_i,
    .rst_ni,
    .req_fire  (tl_h_i.a_valid && tl_h_o.a_ready),
    .cheriot_on(prim_mubi_pkg::mubi4_test_true_strict(cheriot_ena_i)),
    .in_range  (tl_h_i.a_address >= CheriotBaseAddr && tl_h_i.a_address < CheriotTopAddr),
    .op_ok     (tl_h_i.a_opcode inside {tlul_pkg::PutFullData, tlul_pkg::Get}),
    .full_word (tl_h_i.a_address[1:0] == 2'b00 && tl_h_i.a_size == FullWordSize),
    .forwarded (allow_forward)
  );

  bind cheriot_mem_sys cheriot_top_fcov_if u_cheriot_top_fcov (
    .clk_i,
    .rst_ni,
    .start_write       (reg2hw.tbre_start.qe && reg2hw.tbre_start.q),
    .start_nonzero     (|reg2hw.tbre_num_caps.q),
    .start_in_range    (tbre_in_range),
    .start_in_nvm      (tbre_in_nvm),
    .start_cheriot     (prim_mubi_pkg::mubi4_test_true_strict(cheriot_ena_i)),
    .start_accepted    (tbre_valid_d && !tbre_valid_q),
    .start_err         (tbre_start_err),
    .start_clamped     (tbre_sweep_caps != reg2hw.tbre_num_caps.q),
    // TBRE_REGWEN hides a START write from reg2hw while a sweep is active, so see it on the bus.
    .start_while_busy  (regs_tl_d_i.a_valid && regs_tl_d_o.a_ready && tbre_active &&
                        regs_tl_d_i.a_opcode != tlul_pkg::Get &&
                        regs_tl_d_i.a_address[cheriot_mem_sys_reg_pkg::RegsAw-1:0] ==
                        cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_START_OFFSET),
    .tbre_done         (tbre_done),
    // An error earlier in the sweep, or in its last cycle
    .sweep_err         (tbre_failed_q || tbre_sweep_err),
    .epoch_counted     (tbre_epoch_en),
    .intr              (intr_tbre_done_o),
    .rmw_req           ({tag_mux_in_tl_h2d[1].a_valid, tag_mux_in_tl_h2d[0].a_valid}),
    .meta_req          ({meta_mux_in_tl_h2d[3].a_valid, meta_mux_in_tl_h2d[2].a_valid,
                         meta_mux_in_tl_h2d[1].a_valid, meta_mux_in_tl_h2d[0].a_valid}),
    .core_store_fire   (core_req_done && core_req_write &&
                        ((cored_tl_d_i.a_address >= MainSramBaseAddr &&
                          cored_tl_d_i.a_address <  MainSramTopAddr) ||
                         (cored_tl_d_i.a_address >= NvmBaseAddr &&
                          cored_tl_d_i.a_address <  NvmTopAddr))),
    .tbre_inval_pending(u_cheriot_tbre.u_cheriot_tbre_mover.waddr_fifo_out_valid),
    .same_cap          (cored_tl_d_i.a_address[31:3] ==
                        u_cheriot_tbre.u_cheriot_tbre_mover.waddr_fifo_out[31:3]),
    .clear_stale_mark  (tbre_clear_stale_set),
    .clear_squash      (tbre_clear_at_rmw && tbre_clear_stale_q && rmw_tl_d2h.a_ready),
    .fatal_alert       (|cheriot_fatal_error)
  );

  // Capability stores to the NVM: only in the core's tag filter (NvmCapStores = 1).
  bind cheriot_wtrc cheriot_wtrc_fcov_if u_wtrc_fcov (
    .clk_i,
    .rst_ni,
    .cap_state(cap_state_q),
    .rsp_w0   (rsp_done && head_w0),
    .rsp_w1   (head_w1 && (rsp_done || tag_wr_done)),
    .mismatch (mismatch),
    .w0_ok    (w0_ok_q),
    .tag_wr   (tag_wr_req),
    .seq_err  (seq_err),
    .err      (err_o)
  );

  // The TBRE's own TRVK filter. Same decision logic and internal names as the core's ibex_trvk;
  // only the bitmap-port names differ.
  bind cheriot_trvk_core core_ibex_trvk_fcov_if #(.RevBitmapSizeBytes(RevBitmapSizeBytes))
      u_tbre_trvk_fcov (
    .clk_i,
    .rst_ni,
    .revbm_revoked,
    .revbm_out_of_range,
    .revbm_bit_addr,
    .revbm_req_required,
    .revbm_outstanding_q,
    .revbm_bit_select,
    .revbm_rdata_i     (revbm_rsp_data_i),
    .revbm_rsp_data_intg_error,
    .revbm_req_o       (revbm_req_valid_o),
    .revbm_gnt_i       (revbm_req_ready_i),
    .revbm_rvalid_i    (revbm_rsp_valid_i),
    .revbm_err_i       (revbm_rsp_err_i),
    .is_sealing_cap,
    .ptr_storage_valid_q,
    .misalign_flag_out,
    .misalign_flag_out_valid,
    .upstream_rvalid_o (upstream_rsp_valid_o),
    .upstream_tag_o
  );
endmodule
