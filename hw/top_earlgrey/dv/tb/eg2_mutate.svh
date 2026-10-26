// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// EG2 mutations (included in module tb). `+eg2_mutate=<name>` plants one fault from
// hw/top_earlgrey/dv/eg2_mutations.hjson, so the regression can be shown to notice it. Each
// mutation:
//   - forces one signal where the consumer it breaks reads it: an internal net, or a
//     sub-instance's port whose forced value only that consumer sees. A forced port collapses
//     with the net on its other side, so the driver must not read it back, and the expression a
//     force reads must lie on the driver's side of an assignment, never on the forced net;
//   - reaches both lockstep cores when it breaks the core's view (the lockstep check would
//     otherwise catch it first and hide whether the intended check works), except the two
//     lockstep mutations, which break the lockstep on purpose;
//   - re-encodes the integrity of anything it corrupts, where the check it aims at is not the
//     integrity check;
//   - counts the clock cycles in which it changed the design's behaviour. The log line
//     `EG2_MUTATION <name> active <n>` at the end is what the grader reads: n = 0 is INACTIVE.
// Most mutations hold for the whole run; the one-shot ones (a single bad request, response or
// glitch) start `+eg2_mutate_cycle=<n>` clock cycles (default 20000) after reset or after the
// lockstep comparison is enabled.
// An unknown name stops the run, so a typo cannot pass as a missed mutation.

string       eg2_mut_name;
bit          eg2_mut_on;
int unsigned eg2_mut_active;
int unsigned eg2_mut_cycle = 20000;

// State of the one-shot and tracking mutations.
int unsigned eg2_mut_cycles;          // clock cycles since reset (clk_main)
bit          eg2_mut_window;          // tl_cored_bad_opcode: the next request is corrupted
bit          eg2_mut_done;            // the one-shot fault has been applied
bit          eg2_mut_forced;          // tl_meta_two_pending: the source is being forced
logic [top_pkg::TL_AIW-1:0] eg2_mut_src;
bit          eg2_mut_pend [2**top_pkg::TL_AIW];
bit          eg2_mut_capld_q [$];     // load_tag_set: capability-load flags of open core requests
bit          eg2_mut_reg_armed;       // reg_tag_not_cleared_at_switch: the tag has been planted
bit          eg2_mut_reg_written;     // ... and the register has been written since

`define EG2_SWITCH    `CPU_HIER.u_cheriot_switch
`define EG2_CORE      `CPU_HIER.u_core
`define EG2_LS        `CPU_HIER.u_core.gen_lockstep.u_ibex_lockstep
`define EG2_TRVK      `CPU_HIER.u_core.gen_cheriot_trvk.i_ibex_trvk
`define EG2_RF        `CPU_HIER.u_core.gen_regfile_ff.register_file_i
`define EG2_CMS       `PD_MAIN_HIER.u_cheriot_mem_sys
`define EG2_TF        `PD_MAIN_HIER.u_cheriot_mem_sys.u_cheriot_tag_filter
`define EG2_ACC_TRVK  `PD_MAIN_HIER.u_cheriot_mem_sys.u_cheriot_access_check_trvk
`define EG2_XPERI     `PD_MAIN_HIER.u_xbar_peri
`define EG2_S1N       `EG2_XPERI.u_s1n_25
`define EG2_XMAIN     `PD_MAIN_HIER.u_xbar_main
`define EG2_SRAM_MAIN `PD_MAIN_HIER.u_sram_ctrl_main
`define EG2_SRAM_ADPT `PD_MAIN_HIER.u_sram_ctrl_main.u_tlul_adapter_sram_racl.tlul_adapter_sram
`define EG2_SRAM_META `PD_MAIN_HIER.u_sram_ctrl_meta
`define EG2_SRAM_RET  `PD_AON_HIER.u_sram_ctrl_ret
`define EG2_RRAM      `PD_MAIN_HIER.u_rram_ctrl
`define EG2_RRAM_ADPT `PD_MAIN_HIER.u_rram_ctrl.u_tl_adapter_host
`define EG2_RRAM_RD   `PD_MAIN_HIER.u_rram_ctrl.u_rram_phy.u_rram_phy_rd
`define EG2_OTP       `PD_MAIN_HIER.u_otp_ctrl
`define EG2_DAI       `PD_MAIN_HIER.u_otp_ctrl.u_otp_ctrl_dai
`define EG2_KDI       `PD_MAIN_HIER.u_otp_ctrl.u_otp_ctrl_kdi
`define EG2_DM        `PD_MAIN_HIER.u_rv_dm
`define EG2_PLIC      `PD_MAIN_HIER.u_rv_plic
`define EG2_AH        `PD_MAIN_HIER.u_alert_handler
`define EG2_AES       `PD_MAIN_HIER.u_aes
`define EG2_CHERIOT_ON (`CPU_HIER.cheriot_ena_o == prim_mubi_pkg::MuBi4True)

// rv_core_ibex_cheriot_switch's sparse FSM encoding.
localparam logic [5:0] Eg2SwUnlocked  = 6'b010110;
localparam logic [5:0] Eg2SwLockedEna = 6'b100101;
localparam logic [5:0] Eg2SwLockedDis = 6'b111011;

// Data bit 0 of a 39-bit TL-UL word ({data_intg, data}) flipped, with its integrity kept
// matching: the inverted SECDED code is affine, so the check bits change by the code of the
// flipped bit alone, whatever the word.
function automatic logic [38:0] eg2_mut_flip_word(logic [38:0] w);
  return {w[38:32] ^ tlul_pkg::get_data_intg(32'h1) ^ tlul_pkg::get_data_intg(32'h0),
          w[31:0] ^ 32'h1};
endfunction

// An integer store's aligned write data with bit 0 of its lowest enabled byte lane flipped, in
// CHERIoT mode only (cheriot_store_lane_flip, review R18, 2026-10-26): the load/store unit's
// wdata_int as it computes it (lsu_wdata_i rotated by the byte offset), so RVFI, which records
// lsu_wdata before the alignment, keeps the instruction's value.
function automatic logic [31:0] eg2_mut_store_lane(logic [31:0] wdata, logic [1:0] offset,
                                                   logic [3:0] be, logic cheriot_on);
  logic [31:0] w = (offset == 2'b00) ? wdata : (wdata << (8 * offset)) | (wdata >> (32 - 8 * offset));
  if (!cheriot_on) return w;
  for (int b = 0; b < 4; b++) if (be[b]) return w ^ (32'h1 << (8 * b));
  return w;
endfunction

// rram_ctrl's hardwired OTP exclusion rule with read access allowed.
function automatic rram_ctrl_pkg::mp_region_cfg_t eg2_mut_otp_rule_readable();
  rram_ctrl_pkg::mp_region_cfg_t rule = rram_ctrl_pkg::SwInitDataCfg;
  rule.cfg = rram_ctrl_pkg::CfgAllowRd;
  return rule;
endfunction

initial begin : eg2_mutate
  if ($value$plusargs("eg2_mutate=%s", eg2_mut_name)) begin
    eg2_mut_on = 1'b1;
    void'($value$plusargs("eg2_mutate_cycle=%d", eg2_mut_cycle));
    $display("EG2_MUTATION %s applied", eg2_mut_name);
    case (eg2_mut_name)
      ////////////////////
      // The mode switch
      ////////////////////
      // The core, its shadow and the TRVK stay in RISC-V mode while cheriot_mem_sys sees CHERIoT.
      "cheriot_ena_core_stuck_false":
        force `CPU_HIER.cheriot_enable_ibex = ibex_pkg::IbexMuBiOff;
      // The core's tag filter behaves as in RISC-V mode: no tag lookup, no tag write.
      "cheriot_ena_tag_filter_stuck_false":
        force `EG2_TF.require_lookup = 1'b0;
      // The access check of the core's bitmap port behaves as in RISC-V mode: every read errors.
      "cheriot_ena_trvk_check_stuck_false":
        force `EG2_ACC_TRVK.allow_forward = 1'b0;
      // A locked switch takes a second lock: CHERIOT_ENA stays writable after CHERIOT_LOCK.
      "cheriot_lock_ignored":
        // Forced at the sparse flop's plain-bits input: switch_state_d is an enum.
        force `EG2_SWITCH.u_state_regs.u_state_flop.d_i =
            (`EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis} &&
             `EG2_SWITCH.lock_access_i) ?
                (prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.ena_i) ? Eg2SwLockedEna
                                                                           : Eg2SwLockedDis) :
            (`EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis}) ?
                `EG2_SWITCH.switch_state_q :
            (`EG2_SWITCH.switch_state_q == Eg2SwUnlocked && `EG2_SWITCH.lock_access_i) ?
                ((prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.lock_i) &&
                  prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.ena_i)) ? Eg2SwLockedEna :
                 (prim_mubi_pkg::mubi4_test_true_strict(`EG2_SWITCH.lock_i) &&
                  prim_mubi_pkg::mubi4_test_false_strict(`EG2_SWITCH.ena_i)) ? Eg2SwLockedDis :
                 `EG2_SWITCH.switch_state_q) :
            `EG2_SWITCH.switch_state_q;
      // At the switch, x4's capability half holds the tagged memory root (the shared register
      // bank keeps x20's zero-extended data there in RISC-V mode, so its tag is normally 0). The
      // flop keeps the value after the release until x4 is written.
      "reg_tag_not_cleared_at_switch":
        fork
          begin
            @(posedge `CPU_HIER.clk_i iff `CPU_HIER.cheriot_enable_ibex == ibex_pkg::IbexMuBiOn);
            force `EG2_RF.g_cheriot_rf.g_rf_shared_flops[4].rf_reg_q =
                ibex_cheriot_pkg::cheriot_regcap_to_vec(ibex_cheriot_pkg::ROOT_CAP_TM);
            // Since F41 (2026-10-25) an entry reads as null until written in CHERIoT mode
            // (cap_written_q), which would mask the forced root: the bug modelled is x4 counted as
            // written at the switch, so set its bit too: the whole vector (a bit-select of a
            // variable cannot be forced), all others 0 as in the switch cycle (the bits are
            // cleared while the pin is Off). 2026-10-26.
            force `EG2_RF.g_cheriot_rf.cap_written_q = 16'h0010;
            @(posedge `CPU_HIER.clk_i);
            release `EG2_RF.g_cheriot_rf.g_rf_shared_flops[4].rf_reg_q;
            release `EG2_RF.g_cheriot_rf.cap_written_q;
            eg2_mut_reg_armed = 1'b1;
          end
        join_none

      ////////////////////////////////////////////////////////////
      // Core to cheriot_mem_sys: TL-UL, tag sideband, bitmap port
      ////////////////////////////////////////////////////////////
      // Every load returns tag 0 to both cores.
      "load_tag_dropped":
        force `CPU_HIER.main_core_data_tag_d2h = 1'b0;
      // Every load returns tag 1 to both cores.
      "load_tag_set":
        force `CPU_HIER.main_core_data_tag_d2h = 1'b1;
      // In CHERIoT mode an integer store's bus data has one enabled byte lane corrupted (bit 0 of
      // the lowest), in both cores; RVFI keeps the instruction's value (review R18, 2026-10-26).
      "cheriot_store_lane_flip": begin
        force `EG2_CORE.u_ibex_core.load_store_unit_i.wdata_int = eg2_mut_store_lane(
            `EG2_CORE.u_ibex_core.load_store_unit_i.lsu_wdata_i,
            `EG2_CORE.u_ibex_core.load_store_unit_i.data_offset,
            `EG2_CORE.u_ibex_core.load_store_unit_i.data_be, `EG2_CHERIOT_ON);
        force `EG2_LS.u_shadow_core.load_store_unit_i.wdata_int = eg2_mut_store_lane(
            `EG2_LS.u_shadow_core.load_store_unit_i.lsu_wdata_i,
            `EG2_LS.u_shadow_core.load_store_unit_i.data_offset,
            `EG2_LS.u_shadow_core.load_store_unit_i.data_be, `EG2_CHERIOT_ON);
      end
      // A capability store writes tag 0: only the TBRE's tag requests keep theirs.
      "store_tag_dropped":
        force `EG2_TF.tag_m_o = `EG2_TF.cap_tag_req.a_valid;
      // The bitmap response is ignored: nothing is ever revoked.
      "revbm_rsp_ignored":
        force `EG2_TRVK.revbm_revoked = 1'b0;
      // The revocation bit is read one granule off.
      "revbm_index_off_by_one":
        force `EG2_TRVK.revbm_bit_select = `EG2_TRVK.revbm_bit_addr[4:0] + 5'd1;
      // One core data request with an undefined opcode. The fifo before the gate passes the
      // request through (FifoPass), so its input is the request as the core made it.
      "tl_cored_bad_opcode":
        force `CPU_HIER.tl_d_fifo2gate.a_opcode =
            eg2_mut_window ? tlul_pkg::tl_a_op_e'(3'h3)
                           : `CPU_HIER.tl_d_ibex2fifo_main_core.a_opcode;
      // One bitmap response with no request outstanding.
      "tl_revbm_rsp_without_req":
        fork
          begin
            wait (`CPU_HIER.rst_ni === 1'b1);
            repeat (eg2_mut_cycle) @(posedge `CPU_HIER.clk_i);
            @(posedge `CPU_HIER.clk_i iff (!`EG2_TRVK.revbm_outstanding_q &&
                                           !`CPU_HIER.corerevbm_tl_o.a_valid &&
                                           !`CPU_HIER.corerevbm_tl_i.d_valid));
            force `EG2_ACC_TRVK.tl_h_o.d_valid = 1'b1;
            eg2_mut_active++;
            @(posedge `CPU_HIER.clk_i);
            release `EG2_ACC_TRVK.tl_h_o.d_valid;
          end
        join_none
      // A second meta SRAM request on a pending source: the force is applied and released by
      // the tracking block below.
      "tl_meta_two_pending": ;

      ///////////////////////////////
      // The on-chip network
      ///////////////////////////////
      // UART1's device select routed to UART2 (device indices of xbar_peri: uart0 = 0, ...).
      "xbar_peri_uart1_to_uart2":
        force `EG2_S1N.dev_select_t = (`EG2_S1N.dev_select_i == 5'd1) ? 5'd2
                                                                      : `EG2_S1N.dev_select_i;
      // An unmapped address answered without an error. Forced before the responder's
      // tlul_rsp_intg_gen, so the response integrity matches the error-free response.
      "xbar_peri_unmapped_no_error":
        force `EG2_S1N.gen_err_resp.err_resp.tl_h_o_int.d_error = 1'b0;
      "xbar_main_unmapped_no_error":
        force `EG2_XMAIN.u_s1n_41.gen_err_resp.err_resp.tl_h_o_int.d_error = 1'b0;
      // An unmapped address on the core data socket (device 31, the error responder) decoded to
      // the ROM (device 0).
      "xbar_main_unmapped_to_rom":
        force `EG2_XMAIN.u_s1n_41.dev_select_t =
            (`EG2_XMAIN.u_s1n_41.dev_select_i == 5'd31) ? 5'd0 : `EG2_XMAIN.u_s1n_41.dev_select_i;
      // A response for the TBRE (host 2 of u_sm1_38) routed to the core's data host (host 1).
      // Read from the crossbar's port, before its assignment to the socket's net.
      "xbar_main_tbre_rsp_to_core":
        force `EG2_XMAIN.u_sm1_38.tl_d_i.d_source =
            {`EG2_XMAIN.tl_sram_ctrl_main__ram_i.d_source[top_pkg::TL_AIW-1:2],
             (`EG2_XMAIN.tl_sram_ctrl_main__ram_i.d_source[1:0] == 2'd2) ?
                 2'd1 : `EG2_XMAIN.tl_sram_ctrl_main__ram_i.d_source[1:0]};

      ///////////////////////////////
      // sram_ctrl
      ///////////////////////////////
      // Main SRAM read data bit 0 flipped with its integrity re-encoded, in CHERIoT mode.
      // sram_rdata is sram_rdata_scr through an assignment (no ECC correction in this top).
      "sram_main_rdata_flip_cheriot":
        force `EG2_SRAM_MAIN.sram_rdata = `EG2_CHERIOT_ON ?
            eg2_mut_flip_word(`EG2_SRAM_MAIN.sram_rdata_scr) : `EG2_SRAM_MAIN.sram_rdata_scr;
      // The same in RISC-V mode, where the main SRAM holds data only (the code runs from ROM and
      // RRAM): only loads see it. Passed silently while the ISS took every load's value from the
      // chip; must fail since the load check (review R6, 2026-10-25).
      "sram_main_rdata_flip_riscv":
        force `EG2_SRAM_MAIN.sram_rdata = !(`EG2_CHERIOT_ON) ?
            eg2_mut_flip_word(`EG2_SRAM_MAIN.sram_rdata_scr) : `EG2_SRAM_MAIN.sram_rdata_scr;
      // A key renewal of the retention SRAM keeps the old key and nonce.
      "sram_ret_otp_key_not_taken": begin
        force `EG2_SRAM_RET.key_d = `EG2_SRAM_RET.key_q;
        force `EG2_SRAM_RET.nonce_d = `EG2_SRAM_RET.nonce_q;
      end
      // The meta SRAM's INIT writes ones (with matching integrity) instead of zeros.
      "meta_init_not_zeroing":
        force `EG2_SRAM_META.init_data = '1;
      // A fetch the main SRAM's execution enable refuses is answered.
      "sram_main_exec_enable_ignored":
        force `EG2_SRAM_ADPT.instr_error = 1'b0;
      // The meta SRAM integrity errors do not reach cheriot_mem_sys's fatal_fault.
      "meta_intg_not_escalated":
        force `EG2_CMS.u_prim_alert_sender_fatal_fault.alert_req_i =
            `EG2_CMS.cheriot_fatal_error.csr_intg             |
            `EG2_CMS.cheriot_fatal_error.rmw_error            |
            `EG2_CMS.cheriot_fatal_error.tbre_mover_error     |
            `EG2_CMS.cheriot_fatal_error.tbre_revbm_intg      |
            `EG2_CMS.cheriot_fatal_error.tbre_revbm_data_intg |
            `EG2_CMS.cheriot_fatal_error.tbre_revbm_error     |
            `EG2_CMS.cheriot_fatal_error.wtrc_error;

      ///////////////////////////////
      // rram_ctrl
      ///////////////////////////////
      // Host read data bit 0 flipped with its integrity re-encoded. host_rd_data is
      // phy_host_rd_data through rram_ctrl_mp's assignment.
      "rram_host_rdata_flip":
        force `EG2_RRAM.host_rd_data = eg2_mut_flip_word(`EG2_RRAM.phy_host_rd_data);
      // A fetch EXEC (or the RRAM disable) refuses is answered.
      "rram_exec_ignored":
        force `EG2_RRAM_ADPT.instr_error = 1'b0;
      // A host write is answered without d_error.
      "rram_host_write_accepted":
        force `EG2_RRAM_ADPT.wr_vld_error = 1'b0;
      // A controller write leaves the read buffers of its page valid.
      "rram_rd_buf_stale":
        force `EG2_RRAM_RD.buf_invalidate = '0;
      // The word a controller Write sends to the PHY has bit 0 flipped, its integrity re-encoded:
      // the array holds a word the core never sent (review R15, 2026-10-26). wr_data is
      // rram_ctrl_wr's rram_data_o, its write FIFO's output.
      "rram_wr_data_flip":
        force `EG2_RRAM.wr_data = `EG2_RRAM.sw_sel ? eg2_mut_flip_word(`EG2_RRAM.wr_fifo_rdata)
                                                   : `EG2_RRAM.wr_fifo_rdata;
      // The same for the word a Rewrite writes back (the loopback of the word it read): the
      // rewrite stores uncorrected-looking data with fresh ECC (review R15, 2026-10-26).
      "rram_rewrite_data_flip":
        force `EG2_RRAM.wr_data = `EG2_RRAM.hw_loopback_sel ?
            eg2_mut_flip_word(`EG2_RRAM.wr_fifo_rdata) : `EG2_RRAM.wr_fifo_rdata;
      // An uncorrectable ECC error never reaches FAULT_STATUS nor fatal_err.
      "rram_uncorr_err_not_raised":
        force `EG2_RRAM.phy_relbl_err = 1'b0;
      // The hardwired OTP exclusion allows host reads.
      "rram_host_otp_page_reachable":
        force `EG2_RRAM.host_region_cfgs[0] = eg2_mut_otp_rule_readable();

      ///////////////////////////////
      // otp_ctrl
      ///////////////////////////////
      // DIRECT_ACCESS_RDATA returns the working register's words swapped.
      "otp_dai_rdata_words_swapped":
        force `EG2_DAI.dai_rdata_o = `EG2_OTP.dai_idle ?
            {`EG2_DAI.data_q[31:0], `EG2_DAI.data_q[63:32]} : '0;
      // The DAI sees every partition unlocked.
      "otp_dai_lock_ignored":
        force `EG2_DAI.part_access_i =
            {(2 * otp_ctrl_reg_pkg::NumPart){prim_mubi_pkg::MuBi8False}};
      // An ECC error on a DAI read is not reported.
      "otp_dai_ecc_error_suppressed":
        force `EG2_DAI.otp_err = otp_ctrl_top_specific_pkg::NoError;
      // The acknowledgement for sram_ctrl_meta's key (slot 3, request 6) goes to sram_ctrl_main
      // (slot 0, request 3).
      "otp_sram_key_ack_misrouted": begin
        force `EG2_KDI.sram_otp_key_o[0].ack = `EG2_KDI.gnt[3] | `EG2_KDI.gnt[6];
        force `EG2_KDI.sram_otp_key_o[3].ack = 1'b0;
      end

      ///////////////////////////////
      // rv_dm
      ///////////////////////////////
      // The TAP and DMI work whatever the life-cycle state.
      "rv_dm_lc_gate_open":
        force `EG2_DM.gen_jtag_gating.pinmux_hw_debug_en = '{default: lc_ctrl_pkg::On};
      // An SBA write to the main SRAM (device 4 of u_s1n_75) decoded to the sec SRAM (device 5).
      "rv_dm_sba_write_to_sec_sram":
        force `EG2_XMAIN.u_s1n_75.dev_select_t =
            (`EG2_XMAIN.u_s1n_75.dev_select_i == 5'd4 &&
             `EG2_XMAIN.u_s1n_75.tl_h_i.a_opcode != tlul_pkg::Get) ?
                5'd5 : `EG2_XMAIN.u_s1n_75.dev_select_i;
      // SBA read data bit 0 flipped after the adapter's integrity check.
      "rv_dm_sba_rdata_flipped":
        force `EG2_DM.host_r_rdata = `EG2_DM.sba_tl_h_i_int.d_data ^ 32'h1;
      // An SBA bus error does not set sberror.
      "rv_dm_sba_error_suppressed":
        force `EG2_DM.host_r_err = 1'b0;

      ///////////////////////////////
      // rv_plic
      ///////////////////////////////
      // A claim returns the neighbouring ID (bit 0 inverted); the gateway is claimed correctly.
      "rv_plic_claim_id_neighbour":
        force `EG2_PLIC.u_reg.hw2reg.cc0.d =
            (`EG2_PLIC.irq_id_o[0] != '0) ? (`EG2_PLIC.irq_id_o[0] ^ 'h1) : '0;
      // Target 0 sees every source enabled.
      "rv_plic_masked_irq_let_through":
        force `EG2_PLIC.gen_target[0].u_target.ie_i = '1;

      ///////////////////////////////
      // Alerts and escalation
      ///////////////////////////////
      // rram_ctrl_recov_err (alert 35) handshakes but never reaches the classifier.
      "alert_rram_recov_err_dropped":
        force `EG2_AH.u_class.alert_trig_i[35] = 1'b0;
      // Escalation phase 0 never reaches the core's NMI.
      "escalation_to_core_dropped":
        force `CPU_HIER.esc_irq_nm = 1'b0;

      ///////////////////////////////
      // End-to-end bus integrity
      ///////////////////////////////
      // The cores' data and fetch integrity checks and the TRVK's bitmap checks raise nothing.
      "core_bus_intg_check_disabled":
        force `CPU_HIER.alert_major_bus = 1'b0;

      ///////////////////////////////
      // Lockstep
      ///////////////////////////////
      // One shadow core output inverted for 3 cycles: the comparator must report it.
      "lockstep_shadow_output_glitch":
        fork
          begin
            ibex_pkg::ibex_mubi_t busy;
            wait (`EG2_LS.enable_cmp_q == ibex_pkg::IbexMuBiOn);
            repeat (eg2_mut_cycle) @(posedge `CPU_HIER.clk_i);
            busy = `EG2_LS.u_shadow_core.core_busy_o;
            force `EG2_LS.u_shadow_core.core_busy_o = ibex_pkg::ibex_mubi_t'(~busy);
            repeat (3) begin
              @(posedge `CPU_HIER.clk_i);
              if (`EG2_LS.enable_cmp_q != ibex_pkg::IbexMuBiOff) eg2_mut_active++;
            end
            release `EG2_LS.u_shadow_core.core_busy_o;
          end
        join_none
      // A divergence goes unreported.
      "lockstep_comparator_disabled":
        force `EG2_LS.outputs_mismatch = 1'b0;

      ///////////////////////////////
      // Peripherals in CHERIoT mode
      ///////////////////////////////
      // AES DATA_OUT_0 byte-swapped in CHERIoT mode.
      "aes_data_out_byte_swap_cheriot":
        force `EG2_AES.u_reg.hw2reg.data_out[0].d = `EG2_CHERIOT_ON ?
            {`EG2_AES.u_aes_core.data_out_q[0][7:0],   `EG2_AES.u_aes_core.data_out_q[0][15:8],
             `EG2_AES.u_aes_core.data_out_q[0][23:16], `EG2_AES.u_aes_core.data_out_q[0][31:24]} :
            `EG2_AES.u_aes_core.data_out_q[0];

      default:
        $fatal(1, "EG2_MUTATION unknown mutation '%s' (hw/top_earlgrey/dv/eg2_mutations.hjson)",
               eg2_mut_name);
    endcase
  end
end

// Activation (and the one-shot mutations' control) in the core's clock domain: the core,
// cheriot_mem_sys, xbar_main, sram_ctrl_main and _meta, rram_ctrl and rv_dm share clk_main.
always @(posedge `CPU_HIER.clk_i or negedge `CPU_HIER.rst_ni) begin
  if (!`CPU_HIER.rst_ni) begin
    eg2_mut_cycles = 0;
    eg2_mut_capld_q.delete();
  end else if (eg2_mut_on) begin
    eg2_mut_cycles++;
    case (eg2_mut_name)
      "cheriot_ena_core_stuck_false":
        if (`CPU_HIER.cheriot_ena_o == prim_mubi_pkg::MuBi4True) eg2_mut_active++;
      "cheriot_ena_tag_filter_stuck_false":
        if (prim_mubi_pkg::mubi4_test_true_strict(`EG2_TF.cheriot_ena_i) &&
            `EG2_TF.tl_d_i.a_valid && `EG2_TF.addr_tagged && !`EG2_TF.cap_store &&
            ((`EG2_TF.tl_d_is_read && `EG2_TF.tl_d_is_aligned && `EG2_TF.tag_d_i) ||
             `EG2_TF.tl_d_is_write))
          eg2_mut_active++;
      "cheriot_ena_trvk_check_stuck_false":
        if (prim_mubi_pkg::mubi4_test_true_strict(`EG2_ACC_TRVK.cheriot_ena_i) &&
            `EG2_ACC_TRVK.tl_h_i.a_valid)
          eg2_mut_active++;
      "cheriot_lock_ignored":
        if (`EG2_SWITCH.lock_access_i &&
            `EG2_SWITCH.switch_state_q inside {Eg2SwLockedEna, Eg2SwLockedDis}) eg2_mut_active++;
      "reg_tag_not_cleared_at_switch":
        if (eg2_mut_reg_armed && !eg2_mut_reg_written) begin
`ifdef RVFI
          if (`CPU_HIER.rvfi_valid &&
              (`CPU_HIER.rvfi_rs1_addr == 5'd4 || `CPU_HIER.rvfi_rs2_addr == 5'd4))
            eg2_mut_active++;
          if (`CPU_HIER.rvfi_valid && `CPU_HIER.rvfi_rd_addr == 5'd4) eg2_mut_reg_written = 1'b1;
`else
          if (`EG2_RF.raddr_a_i == 5'd4 || `EG2_RF.raddr_b_i == 5'd4) eg2_mut_active++;
          if (`EG2_RF.we_a_i && `EG2_RF.waddr_a_i == 5'd4) eg2_mut_reg_written = 1'b1;
`endif
        end
      "load_tag_dropped":
        if (`CPU_HIER.cored_tag_d2h_i) eg2_mut_active++;
      // A capability load (a read with the tag hint) answered with tag 0. The core data port
      // answers in order.
      "load_tag_set": begin
        if (`EG2_CMS.cored_tl_d_o.d_valid && `EG2_CMS.cored_tl_d_i.d_ready &&
            eg2_mut_capld_q.size() > 0) begin
          if (eg2_mut_capld_q.pop_front() && !`EG2_CMS.cored_tag_d2h_o) eg2_mut_active++;
        end
        if (`EG2_CMS.cored_tl_d_i.a_valid && `EG2_CMS.cored_tl_d_o.a_ready)
          eg2_mut_capld_q.push_back(`EG2_CMS.cored_tl_d_i.a_opcode == tlul_pkg::Get &&
                                    `EG2_CMS.cored_tag_h2d_i);
      end
      "cheriot_store_lane_flip":
        if (`EG2_CHERIOT_ON && `EG2_CORE.u_ibex_core.load_store_unit_i.data_req_o &&
            `EG2_CORE.u_ibex_core.load_store_unit_i.data_we_o &&
            !`EG2_CORE.u_ibex_core.load_store_unit_i.lsu_is_cap_i)
          eg2_mut_active++;
      "store_tag_dropped":
        if (`EG2_TF.tl_d_i.a_valid && `EG2_TF.tag_d_i && `EG2_TF.tl_d_is_write &&
            !`EG2_TF.cap_tag_req.a_valid)
          eg2_mut_active++;
      "revbm_rsp_ignored":
        if (`EG2_TRVK.revbm_rvalid_i &&
            (`EG2_TRVK.revbm_rdata_i[`EG2_TRVK.revbm_bit_addr[4:0]] || `EG2_TRVK.revbm_err_i))
          eg2_mut_active++;
      "revbm_index_off_by_one":
        if (`EG2_TRVK.revbm_rvalid_i &&
            (`EG2_TRVK.revbm_rdata_i[`EG2_TRVK.revbm_bit_addr[4:0]] !=
             `EG2_TRVK.revbm_rdata_i[5'(`EG2_TRVK.revbm_bit_addr[4:0] + 5'd1)]))
          eg2_mut_active++;
      // The window opens after eg2_mut_cycle cycles and closes with the first request it
      // corrupted.
      "tl_cored_bad_opcode": begin
        if (eg2_mut_window && `CPU_HIER.tl_d_fifo2gate.a_valid &&
            `CPU_HIER.tl_d_gate2fifo.a_ready) begin
          eg2_mut_active++;
          eg2_mut_window = 1'b0;
          eg2_mut_done   = 1'b1;
        end else if (!eg2_mut_done && eg2_mut_cycles >= eg2_mut_cycle) begin
          eg2_mut_window = 1'b1;
        end
      end
      // Pending sources on the meta SRAM port; while one is pending, the next request is forced
      // onto it.
      "tl_meta_two_pending": begin
        if (eg2_mut_forced && `EG2_CMS.meta_sram_tl_o.a_valid &&
            `EG2_CMS.meta_sram_tl_i.a_ready) begin
          eg2_mut_active++;
          eg2_mut_done = 1'b1;
        end
        if (`EG2_CMS.meta_sram_tl_i.d_valid && `EG2_CMS.meta_sram_tl_o.d_ready)
          eg2_mut_pend[`EG2_CMS.meta_sram_tl_i.d_source] = 1'b0;
        if (`EG2_CMS.meta_sram_tl_o.a_valid && `EG2_CMS.meta_sram_tl_i.a_ready)
          eg2_mut_pend[`EG2_CMS.meta_sram_tl_o.a_source] = 1'b1;
        if (eg2_mut_forced && (eg2_mut_done || !eg2_mut_pend[eg2_mut_src])) begin
          release `EG2_CMS.u_tlul_socket_m1.tl_d_o.a_source;
          eg2_mut_forced = 1'b0;
        end
        if (!eg2_mut_done && !eg2_mut_forced) begin
          foreach (eg2_mut_pend[s]) begin
            if (eg2_mut_pend[s] && !eg2_mut_forced) begin
              eg2_mut_src    = top_pkg::TL_AIW'(s);
              eg2_mut_forced = 1'b1;
              force `EG2_CMS.u_tlul_socket_m1.tl_d_o.a_source = eg2_mut_src;
            end
          end
        end
      end
      "xbar_main_unmapped_no_error":
        if (`EG2_XMAIN.u_s1n_41.gen_err_resp.err_resp.tl_h_o.d_valid) eg2_mut_active++;
      "xbar_main_unmapped_to_rom":
        if (`EG2_XMAIN.u_s1n_41.tl_h_i.a_valid && `EG2_XMAIN.u_s1n_41.dev_select_i == 5'd31)
          eg2_mut_active++;
      "xbar_main_tbre_rsp_to_core":
        if (`EG2_XMAIN.tl_sram_ctrl_main__ram_i.d_valid &&
            `EG2_XMAIN.tl_sram_ctrl_main__ram_i.d_source[1:0] == 2'd2)
          eg2_mut_active++;
      "sram_main_rdata_flip_cheriot":
        if (`EG2_CHERIOT_ON && `EG2_SRAM_MAIN.sram_rvalid) eg2_mut_active++;
      "sram_main_rdata_flip_riscv":
        if (!(`EG2_CHERIOT_ON) && `EG2_SRAM_MAIN.sram_rvalid) eg2_mut_active++;
      "meta_init_not_zeroing":
        if (`EG2_SRAM_META.init_req) eg2_mut_active++;
      "sram_main_exec_enable_ignored":
        if (`EG2_SRAM_ADPT.tl_i.a_valid &&
            `EG2_SRAM_ADPT.tl_i.a_user.instr_type == prim_mubi_pkg::MuBi4True &&
            prim_mubi_pkg::mubi4_test_false_loose(`EG2_SRAM_ADPT.en_ifetch_i))
          eg2_mut_active++;
      "meta_intg_not_escalated":
        if (`EG2_CMS.cheriot_fatal_error.meta_sram_intg ||
            `EG2_CMS.cheriot_fatal_error.meta_sram_data_intg)
          eg2_mut_active++;
      "rram_host_rdata_flip":
        if (`EG2_RRAM.host_rd_done) eg2_mut_active++;
      "rram_exec_ignored":
        if (`EG2_RRAM_ADPT.tl_i.a_valid &&
            `EG2_RRAM_ADPT.tl_i.a_user.instr_type == prim_mubi_pkg::MuBi4True &&
            prim_mubi_pkg::mubi4_test_false_loose(`EG2_RRAM_ADPT.en_ifetch_i))
          eg2_mut_active++;
      "rram_host_write_accepted":
        if (`EG2_RRAM_ADPT.tl_i.a_valid && `EG2_RRAM_ADPT.tl_i.a_opcode != tlul_pkg::Get)
          eg2_mut_active++;
      "rram_rd_buf_stale":
        if (`EG2_RRAM_RD.wr_req_i) begin
          bit hit;
          hit = 1'b0;
          for (int i = 0; i < rram_ctrl_pkg::NumRdBuf; i++) begin
            hit |= (`EG2_RRAM_RD.rd_buf[i].attr != rram_ctrl_pkg::Invalid) &&
                   (`EG2_RRAM_RD.rd_buf[i].addr[rram_ctrl_pkg::AddrW-1 -: rram_ctrl_pkg::PageW] ==
                    `EG2_RRAM_RD.wr_page_addr_i) &&
                   (`EG2_RRAM_RD.rd_buf[i].part == `EG2_RRAM_RD.wr_part_i);
          end
          if (hit) eg2_mut_active++;
        end
      "rram_wr_data_flip":
        if (`EG2_RRAM.sw_sel && `EG2_RRAM.u_rram_phy.u_rram_phy_wr.ack_o) eg2_mut_active++;
      "rram_rewrite_data_flip":
        if (`EG2_RRAM.hw_loopback_sel && `EG2_RRAM.u_rram_phy.u_rram_phy_wr.ack_o) eg2_mut_active++;
      "rram_uncorr_err_not_raised":
        if (`EG2_RRAM_RD.rd_fifo_req && `EG2_RRAM_RD.rd_ecc_err_i[2]) eg2_mut_active++;
      "rram_host_otp_page_reachable":
        if (`EG2_RRAM.u_rram_ctrl_mp.host_req_i &&
            `EG2_RRAM.u_rram_ctrl_mp.host_page_addr >= rram_ctrl_pkg::OtpStartPage)
          eg2_mut_active++;
      "rv_dm_sba_write_to_sec_sram":
        if (`EG2_XMAIN.u_s1n_75.tl_h_i.a_valid && `EG2_XMAIN.u_s1n_75.dev_select_i == 5'd4 &&
            `EG2_XMAIN.u_s1n_75.tl_h_i.a_opcode != tlul_pkg::Get)
          eg2_mut_active++;
      "rv_dm_sba_rdata_flipped":
        if (`EG2_DM.sba_tl_h_i_int.d_valid &&
            `EG2_DM.sba_tl_h_i_int.d_opcode == tlul_pkg::AccessAckData)
          eg2_mut_active++;
      "rv_dm_sba_error_suppressed":
        if (`EG2_DM.sba_tl_h_i_int.d_valid && `EG2_DM.sba_tl_h_i_int.d_error) eg2_mut_active++;
      "core_bus_intg_check_disabled":
        if (`EG2_CORE.core_alert_major_bus || `EG2_CORE.lockstep_alert_major_bus ||
            `EG2_CORE.trvk_revbm_data_intg_error || `EG2_CORE.trvk_revbm_device_error)
          eg2_mut_active++;
      "lockstep_comparator_disabled":
        if (`EG2_LS.enable_cmp_q != ibex_pkg::IbexMuBiOff &&
            `EG2_LS.shadow_outputs_q != `EG2_LS.core_outputs_q[0])
          eg2_mut_active++;
      default: ;
    endcase
  end
end

// The peripheral crossbar runs on its own clock.
always @(posedge `EG2_XPERI.clk_peri_i) begin
  if (eg2_mut_on) begin
    case (eg2_mut_name)
      "xbar_peri_uart1_to_uart2":
        if (`EG2_S1N.tl_h_i.a_valid && `EG2_S1N.dev_select_i == 5'd1) eg2_mut_active++;
      "xbar_peri_unmapped_no_error":
        if (`EG2_S1N.gen_err_resp.err_resp.tl_h_o.d_valid) eg2_mut_active++;
      default: ;
    endcase
  end
end

// The other blocks, each on its own clock.
always @(posedge `EG2_SRAM_RET.clk_i) begin
  if (eg2_mut_on && eg2_mut_name == "sram_ret_otp_key_not_taken" && `EG2_SRAM_RET.key_ack)
    eg2_mut_active++;
end

always @(posedge `EG2_OTP.clk_i) begin
  if (eg2_mut_on) begin
    case (eg2_mut_name)
      "otp_dai_rdata_words_swapped":
        if (`EG2_DAI.dai_req_i && `EG2_OTP.dai_idle &&
            `EG2_DAI.dai_cmd_i == otp_ctrl_top_specific_pkg::DaiRead)
          eg2_mut_active++;
      "otp_dai_lock_ignored":
        if (`EG2_DAI.dai_req_i && `EG2_OTP.dai_idle &&
            (prim_mubi_pkg::mubi8_test_true_loose(
                 `EG2_OTP.part_access_dai[`EG2_DAI.part_idx].read_lock) ||
             prim_mubi_pkg::mubi8_test_true_loose(
                 `EG2_OTP.part_access_dai[`EG2_DAI.part_idx].write_lock)))
          eg2_mut_active++;
      "otp_dai_ecc_error_suppressed":
        if (`EG2_DAI.otp_rvalid_i && `EG2_DAI.otp_err_i != otp_ctrl_macro_pkg::NoError)
          eg2_mut_active++;
      "otp_sram_key_ack_misrouted":
        if (`EG2_KDI.gnt[6]) eg2_mut_active++;
      default: ;
    endcase
  end
end

always @(posedge `EG2_DM.jtag_i.tck) begin
  if (eg2_mut_on && eg2_mut_name == "rv_dm_lc_gate_open" &&
      !lc_ctrl_pkg::lc_tx_test_true_strict(`EG2_DM.pinmux_hw_debug_en_i))
    eg2_mut_active++;
end

always @(posedge `EG2_PLIC.clk_i) begin
  if (eg2_mut_on) begin
    case (eg2_mut_name)
      "rv_plic_claim_id_neighbour":
        if (`EG2_PLIC.claim_re[0] && `EG2_PLIC.irq_id_o[0] != '0) eg2_mut_active++;
      "rv_plic_masked_irq_let_through": begin
        bit hit;
        hit = 1'b0;
        for (int s = 0; s < $bits(`EG2_PLIC.ip); s++) begin
          hit |= `EG2_PLIC.ip[s] && !`EG2_PLIC.ie[0][s] &&
                 (`EG2_PLIC.prio[s] > `EG2_PLIC.threshold[0]);
        end
        if (hit) eg2_mut_active++;
      end
      default: ;
    endcase
  end
end

always @(posedge `EG2_AH.clk_i) begin
  if (eg2_mut_on && eg2_mut_name == "alert_rram_recov_err_dropped" &&
      `EG2_AH.gen_alerts[35].u_alert_receiver.alert_o)
    eg2_mut_active++;
end

always @(posedge `CPU_HIER.clk_esc_i) begin
  if (eg2_mut_on && eg2_mut_name == "escalation_to_core_dropped" &&
      `CPU_HIER.u_prim_esc_receiver.esc_req_q)
    eg2_mut_active++;
end

always @(posedge `EG2_AES.clk_i) begin
  if (eg2_mut_on && eg2_mut_name == "aes_data_out_byte_swap_cheriot" && `EG2_CHERIOT_ON &&
      `EG2_AES.u_aes_core.data_out_re[0])
    eg2_mut_active++;
end

final begin
  if (eg2_mut_on) $display("EG2_MUTATION %s active %0d", eg2_mut_name, eg2_mut_active);
end

`ifdef EG2_PERIPH_SB
// EG2 periph runs (+eg2_periph_sb=1): the mutation is announced in the injection registry when it
// first changes the design's behaviour, for the checks +eg2_mutate_targets names (the mutation
// table's `targets`, which scripts/gen_eg2_mutation_cfg.py adds to its runs). A mismatch of such a
// check from then on is EXPECTED, and each named check must report one (else MISSED).
initial begin : eg2_mutate_announce
  string targets;
  wait (eg2_mut_active > 0);
  if (!$value$plusargs("eg2_mutate_targets=%s", targets)) targets = "";
  if (chip_env_pkg::eg2_inject_registry::active()) begin
    // Open until the end on purpose: a mutation stays in the design across chip resets (the
    // registry's default window ends at the first mismatch or reset; R12, 2026-10-25).
    void'(chip_env_pkg::eg2_inject_registry::announce({"eg2_mutate ", eg2_mut_name}, targets,
                                                      "mutation", $realtime,
                                                      chip_env_pkg::eg2_inject_registry::UntilEnd));
  end
end
`endif

`undef EG2_SWITCH
`undef EG2_CORE
`undef EG2_LS
`undef EG2_TRVK
`undef EG2_RF
`undef EG2_CMS
`undef EG2_TF
`undef EG2_ACC_TRVK
`undef EG2_XPERI
`undef EG2_S1N
`undef EG2_XMAIN
`undef EG2_SRAM_MAIN
`undef EG2_SRAM_ADPT
`undef EG2_SRAM_META
`undef EG2_SRAM_RET
`undef EG2_RRAM
`undef EG2_RRAM_ADPT
`undef EG2_RRAM_RD
`undef EG2_OTP
`undef EG2_DAI
`undef EG2_KDI
`undef EG2_DM
`undef EG2_PLIC
`undef EG2_AH
`undef EG2_AES
`undef EG2_CHERIOT_ON
