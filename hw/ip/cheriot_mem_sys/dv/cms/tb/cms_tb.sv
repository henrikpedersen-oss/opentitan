// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Block-level testbench of the OpenTitan CHERIoT memory subsystem (`cheriot_mem_sys`,
// hw/ip/cheriot_mem_sys/rtl/cheriot_mem_sys.sv), unmodified.
//
//   cms_tl_host x4  -> cored_tl_d (+ tag sideband), revbm_tl_d, corerevbm_tl, regs_tl_d
//   cms_tl_mem  x3  <- cored_tl_h and tbre_tl_h (one shared data store), meta_sram_tl
//   prim_alert_receiver on fatal_fault; intr_tbre_done_o checked against the model;
//   tlul_assert on all seven TL-UL ports
//   cms_sb (cms_pkg): shadow tag map, revocation bitmap, data and CSR model
//
// Plusargs: +test=<name> (see cms_tests.svh), +seed=<n> (for the log; xrun -svseed seeds),
// +timeout_cycles=<n>, +cms_sb_corrupt=<kind>[,<n>] (scoreboard fault injection: the run must fail),
// +cms_mutate=<name> (plant one RTL bug, Xcelium only: see "Mutation testing" below),
// +cms_cmodel=1|2 (the C reference model as a second checker: 1 reports, 2 fails the run).
// The last line is "CMS_RESULT test=.. seed=.. status=PASS|FAIL errors=.. <statistics>".

module cms_tb;
  import cms_pkg::*;
  import tlul_pkg::*;

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Clock, reset, mode
  ////////////////////////////////////////////////////////////////////////////////////////////////

  logic clk = 1'b0;
  logic rst_n;
  prim_mubi_pkg::mubi4_t ena;

  always #5 clk = ~clk;
  always @(posedge clk) cms_cycle <= cms_cycle + 1;

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // DUT
  ////////////////////////////////////////////////////////////////////////////////////////////////

  tl_h2d_t core_h2d, revbm_h2d, corerevbm_h2d, csr_h2d;
  tl_d2h_t core_d2h, revbm_d2h, corerevbm_d2h, csr_d2h;
  logic    core_tag_h2d, core_tag_d2h;
  logic    unused_tag_revbm, unused_tag_corerevbm, unused_tag_csr;
  tl_h2d_t dmem_h2d, tbre_h2d, meta_h2d;
  tl_d2h_t dmem_d2h, tbre_d2h, meta_d2h;
  logic    intr_tbre_done;

  prim_alert_pkg::alert_rx_t [cheriot_mem_sys_reg_pkg::NumAlerts-1:0] alert_rx;
  prim_alert_pkg::alert_tx_t [cheriot_mem_sys_reg_pkg::NumAlerts-1:0] alert_tx;

  // The core tag sideband as RFC CHERIoT Memory Subsystem "Transferring the Capability Tag Bit"
  // states it: a MuBi4 in each direction (REQ_CMS_01, cms_tag_sideband_mubi). The pinned RTL has
  // one bit each way (cored_tag_h2d_i / cored_tag_d2h_o, TODO(#31066)), so the default build
  // connects the hosts' bit unchanged; +define+CMS_TAG_MUBI connects MuBi4 ports once the RTL has
  // them. tag_mubi_h2d is what the bench presents: the host's bit as MuBi4True / MuBi4False, or
  // the encoding a test overrides it with (tag_mubi_ovr_en). With CMS_TAG_MUBI the host sees a
  // response tag only for MuBi4True.
  prim_mubi_pkg::mubi4_t tag_mubi_h2d;
  prim_mubi_pkg::mubi4_t tag_mubi_ovr = prim_mubi_pkg::MuBi4False;
  bit                    tag_mubi_ovr_en = 1'b0;
  assign tag_mubi_h2d = tag_mubi_ovr_en ? tag_mubi_ovr : prim_mubi_pkg::mubi4_bool_to_mubi(core_tag_h2d);
`ifdef CMS_TAG_MUBI
  prim_mubi_pkg::mubi4_t tag_mubi_d2h;
  assign core_tag_d2h = prim_mubi_pkg::mubi4_test_true_strict(tag_mubi_d2h);
`define CMS_DUT_TAG_H2D tag_mubi_h2d
`define CMS_DUT_TAG_D2H tag_mubi_d2h
`else
`define CMS_DUT_TAG_H2D core_tag_h2d
`define CMS_DUT_TAG_D2H core_tag_d2h
`endif

  cheriot_mem_sys #(
    .addr_t          (logic [31:0]),
    .MainSramBaseAddr(MainSramBase),
    .MainSramTopAddr (MainSramTop),
    .NvmBaseAddr     (NvmBase),
    .NvmTopAddr      (NvmTop),
    .MetaSramBaseAddr(MetaBase),
    .MemSizeRevbm    (RevbmBytes),
    .AlertAsyncOn    ('0)
  ) u_dut (
    .clk_i           (clk),
    .rst_ni          (rst_n),
    .cheriot_ena_i   (ena),
    .intr_tbre_done_o(intr_tbre_done),
    .alert_rx_i      (alert_rx),
    .alert_tx_o      (alert_tx),
    .regs_tl_d_i     (csr_h2d),
    .regs_tl_d_o     (csr_d2h),
    .cored_tl_d_i    (core_h2d),
    .cored_tag_h2d_i (`CMS_DUT_TAG_H2D),
    .cored_tl_d_o    (core_d2h),
    .cored_tag_d2h_o (`CMS_DUT_TAG_D2H),
    .corerevbm_tl_i  (corerevbm_h2d),
    .corerevbm_tl_o  (corerevbm_d2h),
    .revbm_tl_d_i    (revbm_h2d),
    .revbm_tl_d_o    (revbm_d2h),
    .cored_tl_h_o    (dmem_h2d),
    .cored_tl_h_i    (dmem_d2h),
    .tbre_tl_h_o     (tbre_h2d),
    .tbre_tl_h_i     (tbre_d2h),
    .meta_sram_tl_o  (meta_h2d),
    .meta_sram_tl_i  (meta_d2h)
  );

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Hosts and memories
  ////////////////////////////////////////////////////////////////////////////////////////////////

  cms_tl_host #(.Name("core"), .Port(PortCore), .MaxOutstanding(2)) u_core (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_o(core_h2d), .tl_i(core_d2h),
    .tag_o(core_tag_h2d), .tag_i(core_tag_d2h));
  cms_tl_host #(.Name("revbm"), .Port(PortRevbm), .MaxOutstanding(2)) u_revbm (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_o(revbm_h2d), .tl_i(revbm_d2h),
    .tag_o(unused_tag_revbm), .tag_i(1'b0));
  cms_tl_host #(.Name("corerevbm"), .Port(PortCoreRevbm), .MaxOutstanding(1)) u_corerevbm (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_o(corerevbm_h2d), .tl_i(corerevbm_d2h),
    .tag_o(unused_tag_corerevbm), .tag_i(1'b0));
  cms_tl_host #(.Name("csr"), .Port(PortCsr), .MaxOutstanding(1)) u_csr (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_o(csr_h2d), .tl_i(csr_d2h),
    .tag_o(unused_tag_csr), .tag_i(1'b0));

  cms_tl_mem #(.Name("dmem"), .Kind(0), .Depth(4)) u_dmem (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_i(dmem_h2d), .tl_o(dmem_d2h));
  cms_tl_mem #(.Name("tbre_mem"), .Kind(1), .Depth(4)) u_tbre_mem (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_i(tbre_h2d), .tl_o(tbre_d2h));
  cms_tl_mem #(.Name("meta"), .Kind(2), .Depth(2)) u_meta (
    .clk_i(clk), .rst_ni(rst_n), .ena_i(ena), .tl_i(meta_h2d), .tl_o(meta_d2h));

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Alert receiver (synchronous, as the DUT is built with AlertAsyncOn = 0)
  ////////////////////////////////////////////////////////////////////////////////////////////////

  logic        alert_pulse, alert_integ_fail, unused_ping_ok;
  int unsigned alert_cnt;
  bit          alert_allowed;

  prim_alert_receiver #(.AsyncOn(1'b0)) u_alert_rx (
    .clk_i       (clk),
    .rst_ni      (rst_n),
    .init_trig_i (prim_mubi_pkg::MuBi4False),
    .ping_req_i  (1'b0),
    .ping_ok_o   (unused_ping_ok),
    .integ_fail_o(alert_integ_fail),
    .alert_o     (alert_pulse),
    .alert_rx_o  (alert_rx[0]),
    .alert_tx_i  (alert_tx[0])
  );

  always @(posedge clk) begin
    if (rst_n && alert_pulse) alert_cnt++;
    if (rst_n && alert_integ_fail) cms_error("alert", "alert differential pair integrity failure");
  end

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // TL-UL protocol checkers (SVA; empty under Verilator)
  ////////////////////////////////////////////////////////////////////////////////////////////////

  tlul_assert #(.EndpointType("Device")) u_assert_core      (.clk_i(clk), .rst_ni(rst_n), .h2d(core_h2d), .d2h(core_d2h));
  tlul_assert #(.EndpointType("Device")) u_assert_revbm     (.clk_i(clk), .rst_ni(rst_n), .h2d(revbm_h2d), .d2h(revbm_d2h));
  tlul_assert #(.EndpointType("Device")) u_assert_corerevbm (.clk_i(clk), .rst_ni(rst_n), .h2d(corerevbm_h2d), .d2h(corerevbm_d2h));
  tlul_assert #(.EndpointType("Device")) u_assert_csr       (.clk_i(clk), .rst_ni(rst_n), .h2d(csr_h2d), .d2h(csr_d2h));
  tlul_assert #(.EndpointType("Host"))   u_assert_dmem      (.clk_i(clk), .rst_ni(rst_n), .h2d(dmem_h2d), .d2h(dmem_d2h));
  tlul_assert #(.EndpointType("Host"))   u_assert_tbre      (.clk_i(clk), .rst_ni(rst_n), .h2d(tbre_h2d), .d2h(tbre_d2h));
  tlul_assert #(.EndpointType("Host"))   u_assert_meta      (.clk_i(clk), .rst_ni(rst_n), .h2d(meta_h2d), .d2h(meta_d2h));

  // Always-on checks of the DUT's host ports, cycle by cycle
  always @(posedge clk) begin
    if (rst_n) begin
      // MODE_GATING: no meta SRAM request unless cheriot_ena_i is strictly MuBi4True.
      if (meta_h2d.a_valid && ena != prim_mubi_pkg::MuBi4True)
        cms_error("mode", $sformatf("meta SRAM a_valid with cheriot_ena_i = %0h", ena));
      // The engine only reads memory.
      if (tbre_h2d.a_valid && tbre_h2d.a_opcode != Get)
        cms_error("tbre", "write on tbre_tl_h");
      // No core data access issued outside CHERIoT mode looks up a tag (checked by the SB on the
      // returned tag); none is ever dropped either: every core request reaches cored_tl_h.
    end
  end

`ifdef CMS_TAG_MUBI
  // REQ_CMS_01: every response carries a valid MuBi4 tag, MuBi4True or MuBi4False (whether it is
  // the right one is the scoreboard's check, on the strict decode). Off while a test corrupts the
  // response encoding on purpose (cms_tag_sideband_mubi).
  bit tag_d2h_corrupting = 1'b0;
  always @(posedge clk) begin
    if (rst_n && core_d2h.d_valid && core_h2d.d_ready && !tag_d2h_corrupting &&
        tag_mubi_d2h != prim_mubi_pkg::MuBi4True && tag_mubi_d2h != prim_mubi_pkg::MuBi4False)
      cms_error("tag", $sformatf("response tag sideband 0x%0h is neither MuBi4True nor MuBi4False",
                                 tag_mubi_d2h));
  end
`endif

  // intr_tbre_done_o is INTR_STATE & INTR_ENABLE (a level interrupt). The model learns the
  // registers from CSR responses, which come a cycle or more after the register changed, so the
  // pin is compared once the CSR port has been quiet for a few cycles.
  cms_sb       sb;           // the reference model (declared here: the check below uses it)
  int unsigned csr_quiet;
  bit          intr_bad;
  always @(posedge clk) begin
    logic [1:0] exp_intr;
    if (!rst_n || sb == null || !u_csr.idle() || u_csr.done_q.size() != 0) begin
      csr_quiet = 0;
    end else begin
      if (csr_quiet < 4) csr_quiet++;
      exp_intr = sb.intr_pin_expect();
      if (csr_quiet >= 4 && !exp_intr[intr_tbre_done]) begin
        if (!intr_bad)
          cms_error("intr", $sformatf("intr_tbre_done_o %b, expected %s", intr_tbre_done,
                                      tagset_str(exp_intr)));
        intr_bad = 1;
      end else begin
        intr_bad = 0;
      end
    end
  end

  // Functional coverage of the RTL internals (the RTOS SoC's covergroups, bound here as well)
  cms_fcov_bind u_fcov_bind ();

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Mutation testing (Xcelium only)
  ////////////////////////////////////////////////////////////////////////////////////////////////

  // +cms_mutate=<name> plants one bug in the unmodified RTL for the whole run, so the tests can be
  // shown to catch real bugs of the subsystem and not only a corrupted prediction
  // (+cms_sb_corrupt): a test that stays green with the bug in is not testing that obligation.
  // Each mutation forces one internal signal to what a plausible design error would drive.
  // Runtime-selected, so one build serves every mutation. hw/ip/cheriot_mem_sys/dv/cms/scripts/cms_mutation_check.py
  // lists the mutations and the tests that must fail with each; `make
  // cheriot-mem-subsys-mutation-xlm` runs them.
  //
  // Activity: "MUTATION <name> active" is printed the first time the forced value differs from the
  // value the RTL would drive -- its own expression, recomputed below from the same inputs --
  // while the signal matters (sampled on the falling edge), not when the force is applied. A
  // mutation the stimulus never exercises therefore cannot pass as caught. The run ends with the
  // number of cycles the two differed.
  //
  // No replacement value depends on the signal it replaces. Forced signals are variables driven
  // inside the RTL, except alert_req_i, an input port connected to an expression (so it cannot
  // be collapsed with the signals its replacement is computed from).
  //
  // Not under Verilator: the lint target never sees this section, the flow is Xcelium's.
`ifndef VERILATOR
`define CMS_TF    u_dut.u_cheriot_tag_filter
`define CMS_WTRC  `CMS_TF.gen_wtrc.u_cheriot_wtrc
`define CMS_MOVER u_dut.u_cheriot_tbre.u_cheriot_tbre_mover
`define CMS_TRVK  u_dut.u_cheriot_tbre.u_cheriot_trvk_tlul.u_cheriot_trvk_core
`define CMS_RMW   u_dut.u_cheriot_rmw_filter
`define CMS_ACT   u_dut.u_cheriot_access_check_trvk

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
  assign mv_lookup = prim_mubi_pkg::mubi4_test_true_strict(ena) && `CMS_TF.addr_tagged &&
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
  assign mv_num_words = (u_dut.tbre_sweep_caps > 1) ? (32'(u_dut.tbre_sweep_caps) - 32'd1) << 1
                                                    : 32'(u_dut.tbre_sweep_caps) << 1;
  // tbre_done_early: busy only while reads are being issued, not until every clear is answered
  assign mv_busy = (`CMS_MOVER.state_q == 1'b1);
  // data_err_dropped: see cms_mut_drop_host_err
  assign mv_core_rsp = cms_mut_drop_host_err(`CMS_TF.host_rsp, `CMS_WTRC.tag_wr || `CMS_WTRC.head_cap,
                                             `CMS_TF.host_rsp_meta_err, `CMS_TF.tl_d_req_ready,
                                             `CMS_TF.tl_d_rsp_valid);
  // mode_loose_mubi: the core's bitmap window takes any cheriot_ena_i but MuBi4False as enabled
  assign mv_fwd = prim_mubi_pkg::mubi4_test_true_loose(ena) &&
                  `CMS_ACT.tl_h_i.a_address >= MetaRevbmBase &&
                  `CMS_ACT.tl_h_i.a_address < MetaNvmTagBase &&
                  `CMS_ACT.tl_h_i.a_address[1:0] == 2'b00 && `CMS_ACT.tl_h_i.a_size == 2'd2 &&
                  (`CMS_ACT.tl_h_i.a_opcode == tlul_pkg::PutFullData ||
                   `CMS_ACT.tl_h_i.a_opcode == tlul_pkg::Get);
  // tbre_read_err_fatal: an error response to a read of the swept memory is a mover fault (the
  // RTL before the merge of PR #31515), so a read-protected page raises fatal_fault
  assign mv_mover_err = (|`CMS_MOVER.tl_err) || `CMS_MOVER.read_d_err;
  // alert_test_unlocked: ALERT_TEST.regwen is ignored, so alert testing cannot be disabled
  assign mv_alert_we = u_dut.u_reg_regs.alert_test_we;

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
          force u_dut.tbre_num_words = mv_num_words;
        end
        "tbre_no_inval": begin
          mut = MutTbreNoInval;
          force `CMS_MOVER.write_a_valid = 1'b0;
        end
        "tbre_epoch_stuck": begin
          mut = MutTbreEpochStuck;
          force u_dut.tbre_epoch_en = 1'b0;
        end
        "tbre_done_early": begin
          mut = MutTbreDoneEarly;
          force `CMS_MOVER.busy_o = mv_busy;
        end
        "snoop_off": begin
          mut = MutSnoopOff;
          force u_dut.tbre_snoop_valid = '0;
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
          force u_dut.u_prim_alert_sender_fatal_fault.alert_req_i = 1'b0;
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
          force u_dut.u_reg_regs.alert_test_gated_we = mv_alert_we;
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
          rtl = prim_mubi_pkg::mubi4_test_true_strict(ena) && `CMS_TF.addr_tagged &&
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
          differs = u_dut.tbre_valid_q &&
                    u_dut.tbre_num_words != 32'(u_dut.tbre_sweep_caps) << 1;
        MutTbreNoInval:
          differs = `CMS_MOVER.write_a_possible && !`CMS_MOVER.payload_fifo_out.rtag &&
                    !`CMS_MOVER.payload_fifo_out.rerr && `CMS_MOVER.write_rtag_q &&
                    `CMS_MOVER.waddr_fifo_out[2] && !`CMS_MOVER.track_stale_q[`CMS_MOVER.track_rptr];
        MutTbreEpochStuck:
          differs = u_dut.tbre_done && !u_dut.tbre_failed_q && !u_dut.tbre_sweep_err;
        MutTbreDoneEarly:
          differs = `CMS_MOVER.busy_o != (`CMS_MOVER.state_q == 1'b1 || `CMS_MOVER.inflight_q != '0);
        MutSnoopOff: begin
          // A core write, during a sweep, to a capability whose lower word's read the engine
          // presents, which it tracks, or whose clear it presents
          sv    = {u_dut.core_wr_valid_q, u_dut.cored_tl_d_i.a_valid && u_dut.core_req_write};
          sa[0] = u_dut.cored_tl_d_i.a_address;
          sa[1] = u_dut.core_wr_addr_q[0];
          sa[2] = u_dut.core_wr_addr_q[1];
          for (int unsigned s = 0; s < 3; s++) begin
            if (u_dut.tbre_active && sv[s] &&
                ((`CMS_MOVER.tl_r_o.a_valid && sa[s][31:3] == `CMS_MOVER.tl_r_o.a_address[31:3]) ||
                 sa[s][31:3] == `CMS_MOVER.track_granule_q[0] ||
                 sa[s][31:3] == `CMS_MOVER.track_granule_q[1] ||
                 (u_dut.tbre_clear_valid && sa[s][31:3] == u_dut.tbre_clear_addr[31:3])))
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
          differs = |u_dut.cheriot_fatal_error;
        MutModeLooseMubi: begin
          rtl = prim_mubi_pkg::mubi4_test_true_strict(ena) &&
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
          differs = u_dut.u_reg_regs.alert_test_we && !u_dut.u_reg_regs.alert_test_regwen_qs;
        default: ;
      endcase
      if (differs) begin
        mut_cycles++;
        if (mut_cycles == 1)
          $display("MUTATION %s active @%0d: the forced value differs from the RTL's", mut_name,
                   cms_cycle);
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

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Reference model and dispatch
  ////////////////////////////////////////////////////////////////////////////////////////////////

  cms_store data_store, meta_store;

  // Completed transactions go to the scoreboard (and the C model) in completion order, core first.
  function automatic void dispatch_done();
    cms_txn_t x;
    while (u_core.done_q.size() > 0) begin
      x = u_core.done_q.pop_front(); sb.check_core(x); cm_core(x);
    end
    while (u_revbm.done_q.size() > 0) begin
      x = u_revbm.done_q.pop_front(); sb.check_revbm(x, 1'b0); cm_bitmap(x, 1'b0);
    end
    while (u_corerevbm.done_q.size() > 0) begin
      x = u_corerevbm.done_q.pop_front(); sb.check_revbm(x, 1'b1); cm_bitmap(x, 1'b1);
    end
    while (u_csr.done_q.size() > 0) begin
      x = u_csr.done_q.pop_front(); sb.check_csr(x); cm_csr(x);
    end
  endfunction
  always @(posedge clk) if (sb != null) dispatch_done();

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // The C reference model (hw/ip/cheriot_mem_sys/dv/cms/cmodel), a second checker beside sb
  ////////////////////////////////////////////////////////////////////////////////////////////////
  // +cms_cmodel=1: every completed transaction also goes to the C model (cms_cmodel_pkg); its
  // mismatches are reported (CMS_CMODEL lines, and a count on the CMS_RESULT line) but do not fail
  // the run, while it is being proven against this bench. +cms_cmodel=2: they fail the run. Its
  // inputs are what the bench observes: the transactions, the crossbar's error answers on
  // cored_tl_h (in request order, one per core request: assumption A7), the NVM programmed through
  // the backdoor, the resets, and what the tests declare to sb about the faults they inject (sb
  // passes it on: a directed core error as a meta error, the sweep faults).
  int unsigned cm_mode, cm_mismatches;
  chandle      cm;
  bit          cm_host_err_q[$];
  initial if (!$value$plusargs("cms_cmodel=%d", cm_mode)) cm_mode = 0;

  always @(posedge clk)
    if (rst_n && cm != null && dmem_d2h.d_valid && dmem_h2d.d_ready)
      cm_host_err_q.push_back(dmem_d2h.d_error);

  function automatic void cm_report(int n, string what);
    if (n == 0) return;
    cm_mismatches += n;
    if (cm_mode == 2) cms_error("cmodel", $sformatf("%s: %s", what, cms_cmodel_pkg::cms_last_messages()));
    else $display("CMS_CMODEL %s: %s", what, cms_cmodel_pkg::cms_last_messages());
  endfunction

  function automatic void cm_core(cms_txn_t t);
    bit he;
    if (cm == null) return;
    he = cm_host_err_q.size() > 0 ? cm_host_err_q.pop_front() : 1'b0;
    // +cms_sb_corrupt=cmodel,<n>: the n-th data read compared by the model is handed to it with one
    // bit flipped, so a run with +cms_cmodel=2 must fail (the model's check can fail).
    if (!is_write(t) && !t.err && t.mask != 0 &&
        sb.corrupt("cmodel", $sformatf("core read 0x%08x to the C model", t.addr)))
      t.rdata[0] = !t.rdata[0];
    cm_report(cms_cmodel_pkg::cms_core(cm, int'(t.opcode), t.addr, int'(t.size), int'(t.mask),
                                       t.wdata, int'(t.tag), int'(t.ena), int'(he),
                                       int'(sb.cm_directed_err),
                                       int'(t.err), t.rdata, int'(t.rtag)), "core");
  endfunction

  function automatic void cm_bitmap(cms_txn_t t, bit core_port);
    if (cm == null) return;
    cm_report(cms_cmodel_pkg::cms_bitmap(cm, int'(core_port), int'(t.opcode), t.addr - MetaRevbmBase,
                                         int'(t.size), int'(t.mask), t.wdata, int'(t.ena),
                                         int'(t.err), t.rdata), core_port ? "corerevbm" : "revbm");
  endfunction

  function automatic void cm_csr(cms_txn_t t);
    logic [5:0] off;
    if (cm == null) return;
    off = t.addr[5:0];
    cms_cmodel_pkg::cms_set_mode(cm, int'(t.ena));
    // The end of a sweep, as the bench sees it: TBRE_STATUS.busy or TBRE_EPOCH.active read 0, or
    // TBRE_REGWEN read 1, while the model's sweep runs.
    if (!is_write(t) && cms_cmodel_pkg::cms_sweep_active(cm) != 0 &&
        ((off == CsrTbreStatus && !t.rdata[StatusBusy]) || (off == CsrTbreEpoch && !t.rdata[0]) ||
         (off == CsrRegwen && t.rdata[0])))
      cms_cmodel_pkg::cms_sweep_end(cm);
    if (t.err) return;  // a refused register access (unmapped, partial write, integrity): sb's
    cm_report(cms_cmodel_pkg::cms_csr(cm, int'(is_write(t)), 32'(off), t.wdata, t.rdata), "csr");
  endfunction

  // The meta SRAM, word by word, against the C model (bits a race left open are not compared).
  function automatic void cm_check_meta();
    if (cm == null) return;
    for (int unsigned off = 0; off < cms_cmodel_pkg::cms_meta_top(cm); off += 4)
      cm_report(cms_cmodel_pkg::cms_check_meta_word(cm, off, meta_store.read(MetaBase + off)),
                "meta");
  endfunction

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Helpers used by the tests
  ////////////////////////////////////////////////////////////////////////////////////////////////

  int unsigned timeout_cycles = 2_000_000;
  string       test_name = "cms_smoke";
  int unsigned seed = 0;

  task automatic wait_cycles(int unsigned n);
    repeat (n) @(posedge clk);
  endtask

  function automatic cms_txn_t mk(tl_a_op_e op, logic [31:0] addr, logic [1:0] size = 2'd2,
                                  logic [31:0] data = 32'h0, logic tag = 1'b0,
                                  logic [3:0] mask = 4'h0);
    cms_txn_t t;
    t = txn_new();
    t.opcode = op;
    t.addr   = (size == 2'd2) ? {addr[31:2], 2'b00} : (size == 2'd1 ? {addr[31:1], 1'b0} : addr);
    t.size   = size;
    t.mask   = (mask != 4'h0) ? mask : size_mask(t.addr, size);
    // Byte lanes follow the address (TL-UL): place sub-word data in its lanes.
    t.wdata  = (size == 2'd2) ? data : (data << (8 * t.addr[1:0]));
    t.tag    = tag;
    return t;
  endfunction

  task automatic run(port_e p, cms_txn_t t, output cms_txn_t r);
    int unsigned     id;
    longint unsigned t0;
    case (p)
      PortCore:      id = u_core.issue(t);
      PortRevbm:     id = u_revbm.issue(t);
      PortCoreRevbm: id = u_corerevbm.issue(t);
      default:       id = u_csr.issue(t);
    endcase
    t0 = cms_cycle;
    forever begin
      @(posedge clk);
      if (p == PortCore && u_core.rsp_by_id.exists(id)) begin
        r = u_core.rsp_by_id[id]; u_core.rsp_by_id.delete(id); break;
      end
      if (p == PortRevbm && u_revbm.rsp_by_id.exists(id)) begin
        r = u_revbm.rsp_by_id[id]; u_revbm.rsp_by_id.delete(id); break;
      end
      if (p == PortCoreRevbm && u_corerevbm.rsp_by_id.exists(id)) begin
        r = u_corerevbm.rsp_by_id[id]; u_corerevbm.rsp_by_id.delete(id); break;
      end
      if (p == PortCsr && u_csr.rsp_by_id.exists(id)) begin
        r = u_csr.rsp_by_id[id]; u_csr.rsp_by_id.delete(id); break;
      end
      if (cms_cycle - t0 > 20000 || !rst_n) begin
        cms_error("tb", $sformatf("no response to %s 0x%08x on port %s", t.opcode.name(), t.addr,
                                  p.name()));
        r = t;
        break;
      end
    end
    // The host and the dispatcher run on the same edge in either order: two edges guarantee the
    // scoreboard has seen the response before the test goes on.
    repeat (2) @(posedge clk);
  endtask

  // Non-blocking core request
  function automatic void core_issue(cms_txn_t t);
    void'(u_core.issue(t));
  endfunction

  // Every host and memory idle and every response handed to the scoreboard
  task automatic wait_quiet();
    longint unsigned t0;
    t0 = cms_cycle;
    while (!(u_core.idle() && u_revbm.idle() && u_corerevbm.idle() && u_csr.idle() &&
             u_dmem.idle() && u_tbre_mem.idle() && u_meta.idle() &&
             u_core.done_q.size() == 0 && u_csr.done_q.size() == 0 &&
             u_revbm.done_q.size() == 0 && u_corerevbm.done_q.size() == 0)) begin
      @(posedge clk);
      if (cms_cycle - t0 > 200000) begin
        cms_error("tb", "testbench never went quiet (hang?)");
        break;
      end
    end
    wait_cycles(3);
  endtask

  task automatic set_ena(prim_mubi_pkg::mubi4_t v);
    @(posedge clk);
    ena <= v;
    @(posedge clk);
  endtask

  // ---- core ----
  task automatic core_store(logic [31:0] addr, logic [1:0] size, logic [31:0] data,
                            output logic err);
    cms_txn_t r;
    run(PortCore, mk(PutFullData, addr, size, data), r);
    err = r.err;
  endtask

  task automatic core_load(logic [31:0] addr, logic [1:0] size, logic hint,
                           output logic [31:0] data, output logic tag, output logic err);
    cms_txn_t r;
    run(PortCore, mk(Get, addr, size, 32'h0, hint), r);
    data = r.rdata; tag = r.rtag; err = r.err;
  endtask

  // A capability store: two word writes back to back, both carrying the tag. Ibex issues nothing
  // else until the second word is answered, which a capability store to the NVM relies on
  // (theory_of_operation.md "Write-to-Read-and-Compare Filter").
  task automatic core_store_cap(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, logic tag);
    cms_txn_t t;
    core_issue(mk(PutFullData, {addr[31:3], 3'b000}, 2'd2, w0, tag));
    t = mk(PutFullData, {addr[31:3], 3'b100}, 2'd2, w1, tag);
    t.hold_after = tag && in_nvm(addr);
    core_issue(t);
  endtask

  // No core request outstanding and every response handed to the scoreboard
  task automatic wait_core_quiet();
    while (!(u_core.idle() && u_core.done_q.size() == 0)) @(posedge clk);
    repeat (2) @(posedge clk);
  endtask

  // Program a capability's two words into the NVM, as software does through the NVM controller
  // before it stores the capability with its tag (programmers_guide.md "Storing Capabilities in
  // the NVM"). The controller is not a core write: the tag is left alone. Done through the
  // backdoor, with no core access outstanding, no CSR access (a sweep could be starting) and no
  // sweep the model knows of over the capability; returns 0 if it had to skip.
  task automatic nvm_program(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, output bit done);
    logic [31:0] ga;
    ga   = {addr[31:3], 3'b000};
    done = 0;
    if (!in_nvm(ga)) return;
    wait_core_quiet();
    if (!u_csr.idle() || (sb.sweep_active && sb.in_sweep(granule(ga), sb.sweep_base, sb.sweep_caps)))
      return;
    data_store.write(ga, w0, 4'hf);
    data_store.write(ga + 4, w1, 4'hf);
    sb.put_data(ga, w0, 4'hf);
    sb.put_data(ga + 4, w1, 4'hf);
    if (cm != null) begin
      cms_cmodel_pkg::cms_backdoor_data(cm, ga, w0);
      cms_cmodel_pkg::cms_backdoor_data(cm, ga + 4, w1);
    end
    done = 1;
  endtask

  // A capability store to either tagged region: to the NVM, its words are programmed first when
  // `prog` is set, so that a tagged store is verified and sets the tag.
  task automatic store_cap(logic [31:0] addr, logic [31:0] w0, logic [31:0] w1, logic tag,
                           bit prog = 1'b1);
    bit done;
    if (prog && tag && in_nvm(addr)) nvm_program(addr, w0, w1, done);
    core_store_cap(addr, w0, w1, tag);
  endtask

  // A capability load: two hinted word reads back to back; the tag comes with the first word and
  // is held for the second (checked by the scoreboard).
  task automatic core_load_cap(logic [31:0] addr, output logic tag);
    cms_txn_t r0, r1;
    int unsigned id0, id1;
    longint unsigned t0;
    id0 = u_core.issue(mk(Get, {addr[31:3], 3'b000}, 2'd2, 32'h0, 1'b1));
    id1 = u_core.issue(mk(Get, {addr[31:3], 3'b100}, 2'd2, 32'h0, 1'b1));
    t0 = cms_cycle;
    while (!(u_core.rsp_by_id.exists(id0) && u_core.rsp_by_id.exists(id1))) begin
      @(posedge clk);
      if (cms_cycle - t0 > 20000) begin
        cms_error("tb", $sformatf("no response to capability load 0x%08x", addr));
        tag = 1'b0;
        return;
      end
    end
    r0 = u_core.rsp_by_id[id0]; r1 = u_core.rsp_by_id[id1];
    u_core.rsp_by_id.delete(id0); u_core.rsp_by_id.delete(id1);
    tag = r0.rtag && r1.rtag && !r0.err && !r1.err;
    repeat (2) @(posedge clk);
  endtask

  // Encode a capability with base `base` (aligned down to 2^e) and a random address that decodes
  // back to it; checks the TB's own encoder against its decoder.
  task automatic make_cap(logic [31:0] base, int unsigned e, logic [5:0] perms,
                          output logic [31:0] w0, output logic [31:0] w1);
    longint unsigned span, off, b;
    int unsigned     ee;
    ee   = (e >= 24) ? 24 : e;
    b    = 64'(base) & ~((64'd1 << ee) - 1);
    span = 64'd1 << (ee + 9);
    off  = {$urandom, $urandom} % span;
    if (b + off > 64'hffff_ffff) off = 0;
    w0 = 32'(b + off);
    w1 = cap_meta(32'(b), ee, perms, 3'($urandom_range(7)), 9'($urandom));
    if (cap_base(w0, w1) != 32'(b))
      cms_error("tb", $sformatf("make_cap: base 0x%08x decodes as 0x%08x", 32'(b), cap_base(w0, w1)));
  endtask

  // ---- revocation bitmap (through the software window) ----
  task automatic revbm_write(int unsigned w, logic [31:0] data);
    cms_txn_t r;
    run(PortRevbm, mk(PutFullData, MetaRevbmBase + w * 4, 2'd2, data), r);
  endtask

  task automatic revbm_set_base(logic [31:0] base, bit value);
    int unsigned b;
    logic [31:0] d;
    if (!revbm_index(base, b)) return;
    d = sb.get_revbm(b / 32);
    d[b % 32] = value;
    revbm_write(b / 32, d);
  endtask

  // ---- CSRs ----
  task automatic csr_write(logic [5:0] off, logic [31:0] data, output logic err);
    cms_txn_t r;
    run(PortCsr, mk(PutFullData, 32'(off), 2'd2, data), r);
    err = r.err;
  endtask

  task automatic csr_read(logic [5:0] off, output logic [31:0] data);
    cms_txn_t r;
    run(PortCsr, mk(Get, 32'(off), 2'd2), r);
    data = r.rdata;
  endtask

  // Poll TBRE_STATUS.busy until the sweep is over (programmers_guide.md step 5).
  task automatic tbre_wait_idle(int unsigned max_polls = 100000);
    logic [31:0] status;
    for (int unsigned i = 0; i < max_polls; i++) begin
      csr_read(CsrTbreStatus, status);
      if (!status[StatusBusy]) return;
      wait_cycles($urandom_range(8));
    end
    cms_error("tb", "TBRE_STATUS.busy never cleared");
  endtask

  task automatic tbre_sweep(logic [31:0] base, logic [31:0] num, bit wait_done = 1'b1);
    logic e;
    tbre_wait_idle();
    csr_write(CsrTbreBase, base, e);
    csr_write(CsrTbreNum, num, e);
    csr_write(CsrTbreStart, 32'h1, e);
    if (wait_done) tbre_wait_idle();
  endtask

  // Wait for the end of the running sweep by its tbre_done interrupt instead of polling
  // (programmers_guide.md step 5 and "Tracking Completed Sweeps"): enable it, wait for the pin,
  // acknowledge it, and re-check busy, as a wake can be stale and a sweep ending in the cycle the
  // interrupt is acknowledged does not raise it again.
  task automatic tbre_wait_intr(int unsigned max_cycles = 200000);
    logic [31:0] d;
    logic        e;
    csr_write(CsrIntrEnable, 32'h1, e);
    for (int unsigned w = 0; w < 4; w++) begin
      for (int unsigned i = 0; i < max_cycles && !intr_tbre_done; i++) @(posedge clk);
      if (!intr_tbre_done) begin
        cms_error("tb", "tbre_done interrupt never raised");
        return;
      end
      csr_read(CsrTbreStatus, d);
      csr_write(CsrIntrState, 32'h1, e);
      if (!d[StatusBusy]) return;
      csr_read(CsrTbreStatus, d);
      if (!d[StatusBusy]) return;
    end
    cms_error("tb", "tbre_done interrupt raised four times with the engine still busy");
  endtask

  // ---- reset ----
  task automatic do_reset(int unsigned cycles = 5);
    logic     inflight [int unsigned];
    cms_txn_t wr_inflight[$];
    // Between edges: nothing moves until the reset is asserted below.
    @(negedge clk);
    // Responses already in: to the scoreboard first
    dispatch_done();
    foreach (u_core.pend_q[i]) if (is_write(u_core.pend_q[i])) wr_inflight.push_back(u_core.pend_q[i]);
    // A request not yet accepted may already have been forked to the meta SRAM or the memory.
    if (u_core.cur_valid && is_write(u_core.cur)) wr_inflight.push_back(u_core.cur);
    // Core writes that may or may not have reached the meta SRAM
    foreach (u_core.pend_q[i])
      if (is_write(u_core.pend_q[i]) && is_tagged(u_core.pend_q[i].addr))
        inflight[granule(u_core.pend_q[i].addr)] = u_core.pend_q[i].tag;
    if (u_core.cur_valid && is_write(u_core.cur) && is_tagged(u_core.cur.addr))
      inflight[granule(u_core.cur.addr)] = u_core.cur.tag;
    rst_n = 1'b0;
    u_core.flush(); u_revbm.flush(); u_corerevbm.flush(); u_csr.flush();
    u_dmem.clear_injections(); u_tbre_mem.clear_injections(); u_meta.clear_injections();
    sb.on_reset(inflight);
    if (cm != null) begin
      cms_cmodel_pkg::cms_dut_reset(cm);
      cm_host_err_q.delete();
      foreach (inflight[g]) cms_cmodel_pkg::cms_open_tag(cm, granule_addr(g));
    end
    // The data half of a write in flight may or may not have reached memory: either is right.
    foreach (wr_inflight[i]) begin
      logic [31:0] old_d, new_d, obs, m;
      m     = {{8{wr_inflight[i].mask[3]}}, {8{wr_inflight[i].mask[2]}},
               {8{wr_inflight[i].mask[1]}}, {8{wr_inflight[i].mask[0]}}};
      old_d = sb.get_data(wr_inflight[i].addr);
      new_d = (old_d & ~m) | (wr_inflight[i].wdata & m);
      obs   = data_store.read(wr_inflight[i].addr);
      if (obs == old_d || obs == new_d) begin
        sb.put_data(wr_inflight[i].addr, obs, 4'hf);
        if (cm != null) cms_cmodel_pkg::cms_backdoor_data(cm, wr_inflight[i].addr, obs);
      end
      else cms_error("tb", $sformatf("data at 0x%08x after reset is 0x%08x, neither 0x%08x nor 0x%08x",
                                      wr_inflight[i].addr, obs, old_d, new_d));
    end
    wait_cycles(cycles);
    alert_cnt = 0;
    alert_allowed = 0;
    @(negedge clk);
    rst_n = 1'b1;
    wait_cycles(2);
  endtask

  // ---- checks ----
  // Compare the meta SRAM with the prediction, skipping granules with a core write in flight.
  task automatic check_tags(string why);
    bit skip [int unsigned];
    foreach (u_core.req_q[i])
      if (is_write(u_core.req_q[i]) && is_tagged(u_core.req_q[i].addr))
        skip[granule(u_core.req_q[i].addr)] = 1;
    foreach (u_core.pend_q[i])
      if (is_write(u_core.pend_q[i]) && is_tagged(u_core.pend_q[i].addr))
        skip[granule(u_core.pend_q[i].addr)] = 1;
    if (u_core.cur_valid && is_write(u_core.cur) && is_tagged(u_core.cur.addr))
      skip[granule(u_core.cur.addr)] = 1;
    foreach (u_core.done_q[i])
      if (is_write(u_core.done_q[i]) && is_tagged(u_core.done_q[i].addr))
        skip[granule(u_core.done_q[i].addr)] = 1;
    sb.check_backdoor(skip, why);
  endtask

  task automatic expect_alert(string why, int unsigned max_wait = 200);
    for (int unsigned i = 0; i < max_wait && alert_cnt == 0; i++) @(posedge clk);
    if (alert_cnt == 0) cms_error("alert", $sformatf("%s: no fatal_fault alert", why));
    alert_allowed = 1;
  endtask

  task automatic expect_no_alert(string why);
    if (alert_cnt != 0) cms_error("alert", $sformatf("%s: unexpected fatal_fault alert", why));
  endtask

  // The WTRC's assertions state what Ibex does (NoReqBeforeTagWr_A: nothing presented between W1
  // and its tag write). A test that drives what the core cannot, to see the subsystem flag it,
  // turns them off around it: a fired RTL assertion fails the run (cms_results.py).
  // Xcelium only; the Verilator lint target never runs a test.
  task automatic wtrc_assertions(bit on);
`ifndef VERILATOR
    if (on) $asserton(0, u_dut.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc);
    else    $assertoff(0, u_dut.u_cheriot_tag_filter.gen_wtrc.u_cheriot_wtrc);
`endif
  endtask

  // Randomise the handshake timing of every port
  task automatic randomize_timing(bit aggressive);
    u_core.a_gap_max        = aggressive ? 0 : $urandom_range(3);
    u_core.d_ready_pct      = aggressive ? 100 : $urandom_range(100, 50);
    u_revbm.a_gap_max       = $urandom_range(4);
    u_revbm.d_ready_pct     = $urandom_range(100, 60);
    u_corerevbm.a_gap_max   = $urandom_range(4);
    u_corerevbm.d_ready_pct = $urandom_range(100, 60);
    u_csr.d_ready_pct       = $urandom_range(100, 60);
    u_dmem.a_ready_pct      = aggressive ? 100 : $urandom_range(100, 50);
    u_dmem.lat_min          = 1;
    u_dmem.lat_max          = aggressive ? 1 : $urandom_range(4, 1);
    u_tbre_mem.a_ready_pct  = $urandom_range(100, 50);
    u_tbre_mem.lat_max      = $urandom_range(4, 1);
    u_meta.a_ready_pct      = aggressive ? 100 : $urandom_range(100, 50);
    u_meta.lat_max          = aggressive ? 1 : $urandom_range(3, 1);
  endtask

  `include "cms_tests.svh"
  // The tests of the Earl Grey 2 specification's REQ_CMS_01, 02 and 07
  `include "cms_tests_eg2.svh"

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Main
  ////////////////////////////////////////////////////////////////////////////////////////////////

  initial begin
    rst_n = 1'b0;
    ena   = prim_mubi_pkg::MuBi4False;
    alert_cnt = 0;
    alert_allowed = 0;
    void'($value$plusargs("test=%s", test_name));
    void'($value$plusargs("seed=%d", seed));
    void'($value$plusargs("timeout_cycles=%d", timeout_cycles));

    // The CSR map the reference model assumes is the generated one.
    if (CsrIntrState != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_INTR_STATE_OFFSET ||
        CsrIntrEnable != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_INTR_ENABLE_OFFSET ||
        CsrIntrTest != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_INTR_TEST_OFFSET ||
        CsrAlertTest != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_ALERT_TEST_OFFSET ||
        CsrRegwen != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_REGWEN_OFFSET ||
        CsrTbreBase != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_BASE_ADDR_OFFSET ||
        CsrTbreNum != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_NUM_CAPS_OFFSET ||
        CsrTbreStart != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_START_OFFSET ||
        CsrTbreStatus != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_STATUS_OFFSET ||
        CsrTbreEpoch != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_TBRE_EPOCH_OFFSET ||
        $bits(CsrTbreEpoch) != cheriot_mem_sys_reg_pkg::RegsAw)
      cms_error("tb", "CSR offsets differ from cheriot_mem_sys_reg_pkg");
    // ... and so do the write byte enables the model requires (reggen's PERMIT table).
    for (int unsigned i = 0; i < cheriot_mem_sys_reg_pkg::NumRegsRegs; i++)
      if (csr_write_bytes(6'(4 * i)) != cheriot_mem_sys_reg_pkg::CHERIOT_MEM_SYS_REGS_PERMIT[i])
        cms_error("tb", $sformatf("write byte enables of CSR 0x%02x differ from cheriot_mem_sys_reg_pkg",
                                  4 * i));

    data_store = new("data");
    meta_store = new("meta");
    sb = new(meta_store, data_store);
`ifdef CMS_EARLGREY_MAP
    if (cm_mode != 0) cm = cms_cmodel_pkg::cms_new(0);
`else
    if (cm_mode != 0) cm = cms_cmodel_pkg::cms_new(1);
`endif
    sb.cm = cm;
    u_dmem.store = data_store;     u_dmem.sb = sb;
    u_tbre_mem.store = data_store; u_tbre_mem.sb = sb;
    u_meta.store = meta_store;     u_meta.sb = sb;

    cms_info("tb", $sformatf("test %s seed %0d; SRAM [0x%08x,0x%08x) NVM [0x%08x,0x%08x) meta [0x%08x,0x%08x)",
                             test_name, seed, MainSramBase, MainSramTop, NvmBase, NvmTop, MetaBase,
                             MetaTop));
    wait_cycles(5);
    rst_n = 1'b1;
    wait_cycles(2);

    fork
      begin
        run_test(test_name);
        wait_quiet();
        check_tags("end of test");
        sb.check_data("end of test");
        cm_check_meta();
      end
      begin
        wait_cycles(timeout_cycles);
        cms_error("tb", $sformatf("test timed out after %0d cycles", timeout_cycles));
      end
    join_any
    disable fork;

    if (alert_cnt != 0 && !alert_allowed)
      cms_error("alert", $sformatf("%0d fatal_fault alert(s) the test did not expect", alert_cnt));

    $display("CMS_STATS %s", sb.stats());
    if (cm != null) $display("CMS_CMODEL_RESULT mismatches=%0d mode=%0d", cm_mismatches, cm_mode);
    $display("CMS_RESULT test=%s seed=%0d status=%s errors=%0d cycles=%0d %s", test_name, seed,
             cms_err_count == 0 ? "PASS" : "FAIL", cms_err_count, cms_cycle, sb.stats());
    $finish;
  end

endmodule
