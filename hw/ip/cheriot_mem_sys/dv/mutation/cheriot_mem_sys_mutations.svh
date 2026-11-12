// Copyright lowRISC contributors. Licensed under Apache-2.0.
// Ported from cms/tb/cms_tb.sv at cde6b0a7e0: the same 16 RTL mutations.
// Compiled only for the separate mutation build, never normal coverage runs.
// These internal forces plant faults. Internal activity counters only print
// diagnostics; they never feed the reference model, scoreboard or test verdict.
// The campaign grades external-checker failures separately from RTL assertions.
`ifndef VERILATOR
`define CMS_TF    dut.u_cheriot_tag_filter
`define CMS_WTRC  `CMS_TF.gen_wtrc.u_cheriot_wtrc
`define CMS_MOVER dut.u_cheriot_tbre.u_cheriot_tbre_mover
`define CMS_TRVK  dut.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core
`define CMS_RMW   dut.u_cheriot_rmw_filter
`define CMS_ACT   dut.u_cheriot_access_check_trvk

  typedef enum int unsigned {
    MutNone,
    MutPartialWriteKeepsTag,
    MutDataStoreSetsTag,
    MutCapStoreDropsTag,
    MutTrvkWrongBit,
    MutTbreSkipLast,
    MutTbreNoInval,
    MutTbreEpochStuck,
    MutTbreDoneEarly,
    MutSnoopOff,
    MutMetaIntgUnreported,
    MutDataErrDropped,
    MutAlertDropped,
    MutModeLooseMubi,
    MutTbreReadErrFatal,
    MutWtrcPresentedUnflagged,
    MutAlertTestUnlocked
  } cms_mut_e;

  cms_mut_e    mut = MutNone;
  string       mut_name;
  int unsigned mut_cycles = 0;

  // The core's tag filter (rtl/cheriot_tag_filter.sv) answers the core with the data path's
  // response, setting d_error on a failed lookup (proc_connect_tl_rsp); this one drops the data
  // path's own d_error unless the WTRC built the response. The response integrity is updated by the
  // difference, as the RTL does it.
  function automatic tlul_pkg::tl_d2h_t cms_mut_drop_host_err(tlul_pkg::tl_d2h_t host, logic own,
                                                              logic meta_err, logic a_ready,
                                                              logic d_valid);
    tlul_pkg::tl_d2h_t t;
    t                 = host;
    t.d_error         = (own && host.d_error) || meta_err;
    t.d_user.rsp_intg = host.d_user.rsp_intg ^ tlul_pkg::get_rsp_intg(t) ^
                        tlul_pkg::get_rsp_intg(host);
    t.a_ready         = a_ready;
    t.d_valid         = d_valid;
    return t;
  endfunction

  // Replacement values (the RTL line each one replaces is in cms_mutation_check.py)
  logic              mv_lookup;      // require_lookup
  logic              mv_tag_data;    // tag_m_o, data store sets the tag
  logic              mv_tag_cap;     // tag_m_o, capability store drops it
  logic [4:0]        mv_bit_sel;     // revbm_bit_select
  logic [31:0]       mv_num_words;   // tbre_num_words
  logic              mv_busy;        // mover busy_o
  tlul_pkg::tl_d2h_t mv_core_rsp;    // tag filter tl_d_o
  logic              mv_fwd;         // allow_forward
  logic              mv_mover_err;   // mover err_o
  logic              mv_alert_we;    // ALERT_TEST's gated write enable

  // partial_write_keeps_tag: only a full-word PutFullData write is looked up
  assign mv_lookup = prim_mubi_pkg::mubi4_test_true_strict(core_if.cheriot_ena) && `CMS_TF.addr_tagged &&
                     !`CMS_TF.cap_store &&
                     ((`CMS_TF.tl_d_is_read && `CMS_TF.tl_d_is_aligned && `CMS_TF.tag_d_i) ||
                      (`CMS_TF.tl_d_is_write && `CMS_TF.tl_d_i.a_opcode == tlul_pkg::PutFullData &&
                       `CMS_TF.tl_d_i.a_size == 2'd2 && `CMS_TF.tl_d_i.a_mask == 4'hf));
  // datastore_sets_tag: a full-word data store writes tag 1
  assign mv_tag_data = `CMS_TF.cap_tag_req.a_valid || `CMS_TF.tag_d_i ||
                       (`CMS_TF.tl_d_is_write && `CMS_TF.tl_d_i.a_opcode == tlul_pkg::PutFullData &&
                        `CMS_TF.tl_d_i.a_size == 2'd2);
  // capstore_drops_tag: the upper word of a capability store writes tag 0
  assign mv_tag_cap = `CMS_TF.cap_tag_req.a_valid ||
                      (`CMS_TF.tag_d_i && !(`CMS_TF.tl_d_is_write && `CMS_TF.tl_d_i.a_address[2]));
  // trvk_wrong_bit: the engine's bitmap lookup reads the next granule's bit
  assign mv_bit_sel = `CMS_TRVK.revbm_bit_addr[4:0] + 5'd1;
  // tbre_skip_last: a sweep of more than one capability stops one capability early
  assign mv_num_words = (dut.tbre_sweep_caps > 1) ? (32'(dut.tbre_sweep_caps) - 32'd1) << 1
                                                    : 32'(dut.tbre_sweep_caps) << 1;
  // tbre_done_early: busy only while reads are being issued, not until every clear is answered
  assign mv_busy = (`CMS_MOVER.state_q == 1'b1);
  // data_err_dropped: see cms_mut_drop_host_err
  assign mv_core_rsp = cms_mut_drop_host_err(`CMS_TF.host_rsp, `CMS_WTRC.tag_wr || `CMS_WTRC.head_cap,
                                             `CMS_TF.host_rsp_meta_err, `CMS_TF.tl_d_req_ready,
                                             `CMS_TF.tl_d_rsp_valid);
  // mode_loose_mubi: the core's bitmap window takes any cheriot_ena_i but MuBi4False as enabled
  assign mv_fwd = prim_mubi_pkg::mubi4_test_true_loose(core_if.cheriot_ena) &&
                  `CMS_ACT.tl_h_i.a_address >= MetaRevbmBase &&
                  `CMS_ACT.tl_h_i.a_address < MetaNvmTagBase &&
                  `CMS_ACT.tl_h_i.a_address[1:0] == 2'b00 && `CMS_ACT.tl_h_i.a_size == 2'd2 &&
                  (`CMS_ACT.tl_h_i.a_opcode == tlul_pkg::PutFullData ||
                   `CMS_ACT.tl_h_i.a_opcode == tlul_pkg::Get);
  // tbre_read_err_fatal: an error response to a read of the swept memory is a mover fault (the
  // RTL before the merge of PR #31515), so a read-protected page raises fatal_fault
  assign mv_mover_err = (|`CMS_MOVER.tl_err) || `CMS_MOVER.read_d_err;
  // alert_test_unlocked: ALERT_TEST.regwen is ignored, so alert testing cannot be disabled
  assign mv_alert_we = dut.u_reg_regs.alert_test_we;

  initial begin : mutate
    if ($value$plusargs("cms_mutate=%s", mut_name)) begin
      $display("MUTATION %s applied (+cms_mutate)", mut_name);
      case (mut_name)
        "partial_write_keeps_tag": begin
          mut = MutPartialWriteKeepsTag;
          force `CMS_TF.require_lookup = mv_lookup;
        end
        "datastore_sets_tag": begin
          mut = MutDataStoreSetsTag;
          force `CMS_TF.tag_m_o = mv_tag_data;
        end
        "capstore_drops_tag": begin
          mut = MutCapStoreDropsTag;
          force `CMS_TF.tag_m_o = mv_tag_cap;
        end
        "trvk_wrong_bit": begin
          mut = MutTrvkWrongBit;
          force `CMS_TRVK.revbm_bit_select = mv_bit_sel;
        end
        "tbre_skip_last": begin
          mut = MutTbreSkipLast;
          force dut.tbre_num_words = mv_num_words;
        end
        "tbre_no_inval": begin
          mut = MutTbreNoInval;
          force `CMS_MOVER.write_a_valid = 1'b0;
        end
        "tbre_epoch_stuck": begin
          mut = MutTbreEpochStuck;
          force dut.tbre_epoch_en = 1'b0;
        end
        "tbre_done_early": begin
          mut = MutTbreDoneEarly;
          force `CMS_MOVER.busy_o = mv_busy;
        end
        "snoop_off": begin
          mut = MutSnoopOff;
          force dut.tbre_snoop_valid = '0;
        end
        "meta_intg_unreported": begin
          mut = MutMetaIntgUnreported;
          force `CMS_RMW.rsp_intg_error_o  = 1'b0;
          force `CMS_RMW.data_intg_error_o = 1'b0;
        end
        "data_err_dropped": begin
          mut = MutDataErrDropped;
          force `CMS_TF.tl_d_o = mv_core_rsp;
        end
        "alert_dropped": begin
          mut = MutAlertDropped;
          force dut.u_prim_alert_sender_fatal_fault.alert_req_i = 1'b0;
        end
        "mode_loose_mubi": begin
          mut = MutModeLooseMubi;
          force `CMS_ACT.allow_forward = mv_fwd;
        end
        "tbre_read_err_fatal": begin
          mut = MutTbreReadErrFatal;
          force `CMS_MOVER.err_o = mv_mover_err;
        end
        // A request presented between W1 and its tag write is not flagged, only one out of
        // sequence when it is taken (the RTL before the merge of PR #31515)
        "wtrc_presented_unflagged": begin
          mut = MutWtrcPresentedUnflagged;
          force `CMS_WTRC.req_before_tag_wr = 1'b0;
        end
        "alert_test_unlocked": begin
          mut = MutAlertTestUnlocked;
          force dut.u_reg_regs.alert_test_gated_we = mv_alert_we;
        end
        default: $fatal(1, "[cms_tb] unknown mutation '%s'", mut_name);
      endcase
    end
  end

  // Does the forced value differ from the RTL's, where it matters?
  always @(negedge clk) begin
    bit          differs;
    logic        rtl;
    logic [2:0]  sv;
    logic [31:0] sa [3];
    differs = 0;
    if (mut != MutNone && rst_n) begin
      case (mut)
        MutPartialWriteKeepsTag: begin
          rtl = prim_mubi_pkg::mubi4_test_true_strict(core_if.cheriot_ena) && `CMS_TF.addr_tagged &&
                !`CMS_TF.cap_store &&
                ((`CMS_TF.tl_d_is_read && `CMS_TF.tl_d_is_aligned && `CMS_TF.tag_d_i) ||
                 `CMS_TF.tl_d_is_write);
          differs = `CMS_TF.tl_d_i.a_valid && rtl != `CMS_TF.require_lookup;
        end
        MutDataStoreSetsTag, MutCapStoreDropsTag: begin
          rtl     = `CMS_TF.cap_tag_req.a_valid || `CMS_TF.tag_d_i;
          differs = `CMS_TF.tl_m_o.a_valid && rtl != `CMS_TF.tag_m_o;
        end
        MutTrvkWrongBit: begin
          // Only when the lookup's answer changes
          differs = `CMS_TRVK.revbm_rsp_valid_i &&
                    `CMS_TRVK.revbm_rsp_data_i[`CMS_TRVK.revbm_bit_addr[4:0]] !=
                    `CMS_TRVK.revbm_rsp_data_i[`CMS_TRVK.revbm_bit_select];
        end
        MutTbreSkipLast:
          differs = dut.tbre_valid_q &&
                    dut.tbre_num_words != 32'(dut.tbre_sweep_caps) << 1;
        MutTbreNoInval:
          differs = `CMS_MOVER.write_a_possible && !`CMS_MOVER.payload_fifo_out.rtag &&
                    !`CMS_MOVER.payload_fifo_out.rerr && `CMS_MOVER.write_rtag_q &&
                    `CMS_MOVER.waddr_fifo_out[2] && !`CMS_MOVER.track_stale_q[`CMS_MOVER.track_rptr];
        MutTbreEpochStuck:
          differs = dut.tbre_done && !dut.tbre_failed_q && !dut.tbre_sweep_err;
        MutTbreDoneEarly:
          differs = `CMS_MOVER.busy_o != (`CMS_MOVER.state_q == 1'b1 || `CMS_MOVER.inflight_q != '0);
        MutSnoopOff: begin
          // A core write, during a sweep, to a capability whose lower word's read the engine
          // presents, which it tracks, or whose clear it presents
          sv    = {dut.core_wr_valid_q, dut.cored_tl_d_i.a_valid && dut.core_req_write};
          sa[0] = dut.cored_tl_d_i.a_address;
          sa[1] = dut.core_wr_addr_q[0];
          sa[2] = dut.core_wr_addr_q[1];
          for (int unsigned s = 0; s < 3; s++) begin
            if (dut.tbre_active && sv[s] &&
                ((`CMS_MOVER.tl_r_o.a_valid && sa[s][31:3] == `CMS_MOVER.tl_r_o.a_address[31:3]) ||
                 sa[s][31:3] == `CMS_MOVER.track_granule_q[0] ||
                 sa[s][31:3] == `CMS_MOVER.track_granule_q[1] ||
                 (dut.tbre_clear_valid && sa[s][31:3] == dut.tbre_clear_addr[31:3])))
              differs = 1;
          end
        end
        MutMetaIntgUnreported:
          differs = `CMS_RMW.tl_d_i.d_valid && `CMS_RMW.tl_d_o.d_ready &&
                    (|`CMS_RMW.rsp_intg_error || |`CMS_RMW.rsp_data_intg_error);
        MutDataErrDropped: begin
          rtl     = `CMS_TF.host_rsp.d_error || `CMS_TF.host_rsp_meta_err;
          differs = `CMS_TF.tl_d_o.d_valid && rtl != `CMS_TF.tl_d_o.d_error;
        end
        MutAlertDropped:
          differs = |dut.cheriot_fatal_error;
        MutModeLooseMubi: begin
          rtl = prim_mubi_pkg::mubi4_test_true_strict(core_if.cheriot_ena) &&
                `CMS_ACT.tl_h_i.a_address >= MetaRevbmBase &&
                `CMS_ACT.tl_h_i.a_address < MetaNvmTagBase &&
                `CMS_ACT.tl_h_i.a_address[1:0] == 2'b00 && `CMS_ACT.tl_h_i.a_size == 2'd2 &&
                (`CMS_ACT.tl_h_i.a_opcode == tlul_pkg::PutFullData ||
                 `CMS_ACT.tl_h_i.a_opcode == tlul_pkg::Get);
          differs = `CMS_ACT.tl_h_i.a_valid && rtl != `CMS_ACT.allow_forward;
        end
        MutTbreReadErrFatal:
          differs = `CMS_MOVER.read_d_err && !(|`CMS_MOVER.tl_err);
        // cap_state_q 2'd2 is CapW1: W1 taken, neither answered nor turned into its tag write
        MutWtrcPresentedUnflagged:
          differs = `CMS_WTRC.tl_d_i.a_valid && `CMS_WTRC.cap_state_q == 2'd2 &&
                    !`CMS_WTRC.w1_done;
        MutAlertTestUnlocked:
          differs = dut.u_reg_regs.alert_test_we && !dut.u_reg_regs.alert_test_regwen_qs;
        default: ;
      endcase
      if (differs) begin
        mut_cycles++;
        if (mut_cycles == 1)
          $display("MUTATION %s active @%0d: the forced value differs from the RTL's", mut_name,
                   $time);
      end
    end
  end

  final if (mut != MutNone)
    $display("MUTATION %s differed from the RTL on %0d cycle(s)", mut_name, mut_cycles);

`undef CMS_ACT
`undef CMS_RMW
`undef CMS_TRVK
`undef CMS_MOVER
`undef CMS_WTRC
`undef CMS_TF
`endif
