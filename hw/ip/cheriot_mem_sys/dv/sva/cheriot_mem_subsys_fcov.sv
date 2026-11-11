// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// RTL-internal observations for functional coverage only. These interfaces have no outputs,
// assertions, predictions, ownership decisions or connections to the model/scoreboard.
// Source: cheriot-rtos-test-suite ae3b0bb, dv/cov/cheriot_mem_subsys_fcov.sv.
// RTOS rev_ctl_tbre is excluded; applicable Ibex TRVK decisions are in cheriot_trvk_fcov.sv.
// Enabled with +en_cov=1, as the UVM environment is.

`include "dv_fcov_macros.svh"

// ---------------------------------------------------------------------------------------------------
// Tag filter: whether an access needs its tag looked up or updated, and what the core gets back.
// Bound to both instances: the core's (TagOnlyWrites = 0) and the TBRE's (TagOnlyWrites = 1).
// ---------------------------------------------------------------------------------------------------
interface cheriot_tag_filter_fcov_if (
  input logic clk_i,
  input logic rst_ni,
  input logic tag_only_writes,
  // request accepted
  input logic req_fire,
  input logic req_is_read,
  input logic req_is_write,
  input logic req_is_partial,
  input logic req_aligned,
  input logic req_cap_hint,     // tag_d_i: capability load hint / tag of a store
  input logic req_in_sram,
  input logic req_in_nvm,
  input logic lookup,
  input logic cheriot_on,
  // capability store to the NVM, verified by the WTRC instead of looked up (core filter only)
  input logic nvm_cap_store,
  // meta path back-pressure while a lookup is being issued
  input logic meta_stall,
  // response delivered
  input logic rsp_fire,
  input logic rsp_lookup,
  input logic rsp_aligned,
  input logic rsp_tag,
  input logic rsp_err,
  input logic rsp_meta_err
);
  covergroup tag_filter_req_cg @(posedge clk_i);
    option.per_instance = 1;
    cp_op: coverpoint {req_is_read, req_is_write, req_is_partial} iff (rst_ni && req_fire) {
      bins get          = {3'b100};
      bins put_full     = {3'b010};
      bins put_partial  = {3'b011};
    }
    cp_region: coverpoint {req_in_sram, req_in_nvm} iff (rst_ni && req_fire) {
      bins untagged = {2'b00};
      bins sram     = {2'b10};
      bins nvm      = {2'b01};
    }
    cp_aligned:  coverpoint req_aligned  iff (rst_ni && req_fire);
    cp_cap_hint: coverpoint req_cap_hint iff (rst_ni && req_fire);
    cp_lookup:   coverpoint lookup       iff (rst_ni && req_fire);
    cp_cheriot:  coverpoint cheriot_on   iff (rst_ni && req_fire);

    // The lookup decision: every read/write shape in every region, with and without CHERIoT.
    op_region_lookup_cross: cross cp_op, cp_region, cp_lookup, cp_cheriot {
      // A lookup only happens in tagged memory in CHERIoT mode (require_lookup).
      ignore_bins no_lookup_untagged = binsof(cp_region.untagged) && binsof(cp_lookup) intersect {1};
      ignore_bins no_lookup_off      = binsof(cp_cheriot) intersect {0} &&
                                       binsof(cp_lookup) intersect {1};
      // Every SRAM write in CHERIoT mode updates its tag, including partial writes.
      ignore_bins sram_write_no_lookup = (binsof(cp_op.put_full) || binsof(cp_op.put_partial)) &&
                                         binsof(cp_region.sram) &&
                                         binsof(cp_cheriot) intersect {1} &&
                                         binsof(cp_lookup) intersect {0};
    }
    // Reads look up only when 64-bit aligned *and* hinted as a capability load. The misaligned
    // hinted read (second word of a capability) must not look up: it reuses the sticky tag.
    read_shape_cross: cross cp_aligned, cp_cap_hint, cp_lookup iff (req_is_read) {
      ignore_bins lookup_unaligned = binsof(cp_aligned) intersect {0} &&
                                     binsof(cp_lookup)  intersect {1};
      ignore_bins lookup_unhinted  = binsof(cp_cap_hint) intersect {0} &&
                                     binsof(cp_lookup)   intersect {1};
    }
    // Stores: a capability store sets the tag, any other store to tagged memory clears it. A
    // capability store to the NVM is not looked up but verified (cp_nvm_cap_store).
    store_tag_cross: cross cp_cap_hint, cp_region iff (req_is_write && lookup) {
      ignore_bins untagged      = binsof(cp_region.untagged);
      ignore_bins nvm_cap_store = binsof(cp_cap_hint) intersect {1} && binsof(cp_region.nvm);
    }
    // Only the core's filter verifies NVM capability stores; the TBRE's only clears tags.
    cp_nvm_cap_store: coverpoint nvm_cap_store iff (rst_ni && req_fire && !tag_only_writes) {
      bins verified = {1'b1};
    }
    cp_meta_stall: coverpoint meta_stall iff (rst_ni) { bins stalled = {1'b1}; }
  endgroup

  covergroup tag_filter_rsp_cg @(posedge clk_i);
    option.per_instance = 1;
    // Tag the requester is given: from the meta SRAM on the first word, the sticky copy on the
    // second.
    cp_rsp_tag: coverpoint rsp_tag iff (rst_ni && rsp_fire && rsp_lookup);
    cp_rsp_word: coverpoint rsp_aligned iff (rst_ni && rsp_fire && rsp_lookup);
    rsp_tag_word_cross: cross cp_rsp_tag, cp_rsp_word {
      // Only stores look up an upper word; store responses carry no tag.
      // Upper-word reads use the sticky tag and do not have rsp_lookup set.
      ignore_bins upper_tag_one = binsof(cp_rsp_word) intersect {0} &&
                                  binsof(cp_rsp_tag) intersect {1};
    }
    cp_rsp_err:      coverpoint rsp_err      iff (rst_ni && rsp_fire);
    cp_rsp_meta_err: coverpoint rsp_meta_err iff (rst_ni && rsp_fire && rsp_lookup);
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(tag_filter_req_cg, en_fcov)
  `DV_FCOV_INSTANTIATE_CG(tag_filter_rsp_cg, en_fcov)
endinterface

// ---------------------------------------------------------------------------------------------------
// RMW filter: the read-modify-write of one tag bit in the meta SRAM.
// ---------------------------------------------------------------------------------------------------
interface cheriot_rmw_filter_fcov_if (
  input logic       clk_i,
  input logic       rst_ni,
  input logic [1:0] state_q,         // Passthrough, Fill, WriteBackReq, WriteBackAck
  input logic       fill_rsp,        // Fill and the meta word arrived
  input logic       old_tag,         // stored bit
  input logic       new_tag,         // bit to write
  input logic       fill_err,
  input logic [4:0] bit_sel,
  input logic       data_intg_err,
  input logic       rsp_intg_err,
  input logic       device_err,
  // a request taken in the cycle the previous read is answered (back-to-back reads, possibly from
  // the other requester behind the shared filter)
  input logic       req_on_read_rsp
);
  covergroup rmw_cg @(posedge clk_i);
    option.per_instance = 1;
    cp_state: coverpoint state_q iff (rst_ni) {
      bins pass_to_fill   = (2'h0 => 2'h1);
      bins fill_skip      = (2'h1 => 2'h0);
      bins fill_to_wb     = (2'h1 => 2'h2);
      bins wb_req_to_ack  = (2'h2 => 2'h3);
      bins wb_ack_to_pass = (2'h3 => 2'h0);
    }
    // All four old/new combinations: two skip the write-back, two change the word.
    cp_old_tag: coverpoint old_tag iff (rst_ni && fill_rsp && !fill_err);
    cp_new_tag: coverpoint new_tag iff (rst_ni && fill_rsp && !fill_err);
    old_new_cross: cross cp_old_tag, cp_new_tag;
    cp_fill_err: coverpoint fill_err iff (rst_ni && fill_rsp);
    cp_bit_sel: coverpoint bit_sel iff (rst_ni && fill_rsp) {
      bins low[]  = {[0:3]};
      bins mid    = {[4:27]};
      bins high[] = {[28:31]};
    }
    cp_errors: coverpoint {device_err, rsp_intg_err, data_intg_err} iff (rst_ni) {
      wildcard bins device    = {3'b1??};
      wildcard bins rsp_intg  = {3'b?1?};
      wildcard bins data_intg = {3'b??1};
    }
    cp_read_back_to_back: coverpoint req_on_read_rsp iff (rst_ni) { bins taken = {1'b1}; }
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(rmw_cg, en_fcov)
endinterface

// ---------------------------------------------------------------------------------------------------
// TBRE mover: the sweep and what happens to every capability in it.
// ---------------------------------------------------------------------------------------------------
interface cheriot_tbre_mover_fcov_if (
  input logic        clk_i,
  input logic        rst_ni,
  input logic        sweep_start,   // start: renamed, a covergroup has a built-in start() method
  input logic [31:0] num_words,
  input logic        running,
  input logic        busy,
  // join of a word's read result with its write address
  input logic        join_fire,
  input logic        second_word,    // waddr_fifo_out[WordOffsetW]: the word that decides
  input logic        first_tag,      // write_rtag_q: tag seen on the capability's first word
  input logic        rtag,           // tag after the TRVK filter on this word
  input logic        rerr,
  input logic        stale,          // track_stale_q of the capability: the core wrote it
  input logic        invalidate,     // write_a_valid
  input logic [3:0]  inflight,
  // {read error response (not cleared, sweep_err, no alert), read integrity, write malformed or
  //  error, write integrity}
  input logic [3:0]  errs,
  // core writes against the capability tracker
  input logic        snoop_read_hit,   // to the capability whose lower word is being read
  input logic        snoop_track_mark, // marks a tracked capability stale
  input logic [1:0]  mem_requests,     // {read request, invalidation request}
  input logic        snoop_frozen_hit  // to the tracked capability whose clear is presented
);
  covergroup tbre_mover_cg @(posedge clk_i);
    option.per_instance = 1;
    // Replaces old cp_concur_mem_reqs at the mover's two TL request ports.
    cp_concur_mem_reqs: coverpoint mem_requests iff (rst_ni) {
      bins quiet = {0}; bins invalidate_only = {1}; bins read_only = {2}; bins both = {3};
    }
    // Old cp_tbre_fsm/os_cnt/mem_err map to cp_running/cp_inflight/cp_errs.
    // FIFO hazard and wrapper block-1 cancellation described a different core engine;
    // they have no corresponding documented operation in this mover.
    cp_sweep_len: coverpoint num_words iff (rst_ni && sweep_start) {
      bins one_cap   = {2};
      bins few_caps  = {[4:16]};
      bins many_caps = {[18:$]};
    }
    cp_running: coverpoint running iff (rst_ni) {
      bins start_sweep = (1'b0 => 1'b1);
      bins end_sweep   = (1'b1 => 1'b0);
    }
    // Draining after the last read was issued: running ended, words still in flight.
    cp_drain: coverpoint (busy && !running) iff (rst_ni) { bins draining = {1'b1}; }

    // Per-capability outcome, decided on its second word. A revoked capability the core wrote
    // since its lower word was read is not cleared (kept_stale).
    cp_outcome: coverpoint {rerr, first_tag, rtag, stale, invalidate}
                iff (rst_ni && join_fire && second_word) {
      wildcard bins read_error   = {5'b1????};
      wildcard bins untagged     = {5'b000?0};
      wildcard bins kept         = {5'b011?0};
      bins          revoked      = {5'b01001};
      bins          kept_stale   = {5'b01010};
      wildcard illegal_bins invalidate_kept     = {5'b011?1};
      wildcard illegal_bins invalidate_untagged = {5'b00??1};
      illegal_bins  invalidate_stale    = {5'b01011};
    }
    // cheriot_tbre instantiates the mover with MaxInflight = 2.
    cp_inflight: coverpoint inflight iff (rst_ni) { bins levels[] = {[0:2]}; }
    // Core writes the tracker sees: during the lower word's read, to a tracked capability, and to
    // the one whose clear is already presented (left to the subsystem's clear squash).
    cp_snoop: coverpoint {snoop_read_hit, snoop_track_mark, snoop_frozen_hit} iff (rst_ni) {
      wildcard bins read_hit   = {3'b1??};
      wildcard bins track_mark = {3'b?1?};
      wildcard bins frozen_hit = {3'b??1};
    }
    cp_errs: coverpoint errs iff (rst_ni) {
      wildcard bins read_err      = {4'b1???};
      wildcard bins read_tl_intg  = {4'b?1??};
      wildcard bins write_tl      = {4'b??1?};
      wildcard bins write_tl_intg = {4'b???1};
    }
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(tbre_mover_cg, en_fcov)
endinterface

// ---------------------------------------------------------------------------------------------------
// Access checkers: every request to the meta SRAM, forwarded or refused, and why.
// ---------------------------------------------------------------------------------------------------
interface cheriot_access_check_fcov_if (
  input logic clk_i,
  input logic rst_ni,
  input logic req_fire,
  input logic cheriot_on,
  input logic in_range,
  input logic op_ok,
  input logic full_word,
  input logic forwarded
);
  covergroup access_check_cg @(posedge clk_i);
    option.per_instance = 1;
    cp_decision: coverpoint {forwarded, cheriot_on, in_range, op_ok, full_word}
                 iff (rst_ni && req_fire) {
      bins          forwarded     = {5'b11111};
      wildcard bins denied_mode   = {5'b00???};
      wildcard bins denied_range  = {5'b010??};
      wildcard bins denied_op     = {5'b0110?};
      bins          denied_size   = {5'b01110};
      wildcard illegal_bins forwarded_bad = {5'b10???, 5'b1?0??, 5'b1??0?, 5'b1???0};
    }
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(access_check_cg, en_fcov)
endinterface

// ---------------------------------------------------------------------------------------------------
// Subsystem top: sweep starts, arbitration, and the core/TBRE race.
// ---------------------------------------------------------------------------------------------------
interface cheriot_top_fcov_if (
  input logic       clk_i,
  input logic       rst_ni,
  // TBRE_START written with 1
  input logic       start_write,
  input logic       start_nonzero,
  input logic       start_in_range,
  input logic       start_in_nvm,    // in range, in the NVM rather than the main SRAM
  input logic       start_cheriot,
  input logic       start_accepted,  // tbre_valid_d rose
  input logic       start_err,       // ignored start, sets TBRE_STATUS.start_err
  input logic       start_clamped,   // sweep shortened at the top of its tagged region
  input logic       start_while_busy,
  // end of a sweep
  input logic       tbre_done,       // the engine stopped being active
  input logic       sweep_err,       // sets TBRE_STATUS.sweep_err
  input logic       epoch_counted,   // TBRE_EPOCH counts the sweep
  input logic       intr,            // intr_tbre_done_o
  // contention
  input logic [1:0] rmw_req,         // {TBRE, core} tag traffic onto the RMW filter
  input logic [3:0] meta_req,        // {tbre revbm, sys revbm, tags, core revbm} onto the meta SRAM
  // race: the core stores to the capability the TBRE has an invalidation pending for
  input logic       core_store_fire,
  input logic       tbre_inval_pending,
  input logic       same_cap,
  // a core write overtakes a presented tag clear (marks it stale), which then reaches the RMW
  // filter as a read
  input logic       clear_stale_mark,
  input logic       clear_squash,
  input logic       fatal_alert
);
  covergroup cheriot_top_cg @(posedge clk_i);
    option.per_instance = 1;
    cp_start: coverpoint {start_accepted, start_nonzero, start_in_range, start_cheriot}
              iff (rst_ni && start_write && !start_while_busy) {
      bins          accepted          = {4'b1111};
      wildcard bins dropped_zero      = {4'b00??};
      wildcard bins dropped_range     = {4'b010?};
      bins          dropped_not_cheri = {4'b0110};
    }
    // Sweeps of either tagged region
    cp_start_region: coverpoint start_in_nvm iff (rst_ni && start_accepted) {
      bins sram = {1'b0};
      bins nvm  = {1'b1};
    }
    cp_start_err: coverpoint start_err iff (rst_ni) { bins ignored = {1'b1}; }
    cp_start_while_busy: coverpoint start_while_busy iff (rst_ni) {
      bins ignored = {1'b1};
    }
    cp_clamped: coverpoint start_clamped iff (rst_ni && start_accepted);
    // A sweep ends counted, or with an error and not counted.
    cp_sweep_end: coverpoint {sweep_err, epoch_counted} iff (rst_ni && tbre_done) {
      bins          counted   = {2'b01};
      bins          failed    = {2'b10};
      illegal_bins  err_count = {2'b11};
    }
    cp_intr: coverpoint intr iff (rst_ni) {
      bins raised  = (1'b0 => 1'b1);
      bins cleared = (1'b1 => 1'b0);
    }
    cp_rmw_contention: coverpoint rmw_req iff (rst_ni) {
      bins core_only = {2'b01};
      bins tbre_only = {2'b10};
      bins both      = {2'b11};
    }
    cp_meta_requesters: coverpoint meta_req iff (rst_ni) {
      bins core_revbm = {4'b0001};
      bins tags       = {4'b0010};
      bins sys_revbm  = {4'b0100};
      bins tbre_revbm = {4'b1000};
      bins several    = {[4'b0011:4'b1111]} with ($countones(item) > 1);
    }
    // A fresh capability stored where the TBRE is about to clear a stale one's tag: the TBRE's
    // write must not remove the new capability's tag. Needs a directed test to reach.
    cp_tbre_core_store_race: coverpoint (tbre_inval_pending && same_cap)
                             iff (rst_ni && core_store_fire) {
      bins race = {1'b1};
    }
    cp_clear_squash: coverpoint {clear_stale_mark, clear_squash} iff (rst_ni) {
      wildcard bins marked   = {2'b1?};
      wildcard bins squashed = {2'b?1};
    }
    cp_fatal_alert: coverpoint fatal_alert iff (rst_ni) { bins raised = {1'b1}; }
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(cheriot_top_cg, en_fcov)
endinterface

// ---------------------------------------------------------------------------------------------------
// WTRC: capability stores to the read-only NVM, verified against its contents (core tag filter).
// ---------------------------------------------------------------------------------------------------
interface cheriot_wtrc_fcov_if (
  input logic       clk_i,
  input logic       rst_ni,
  input logic [1:0] cap_state,   // cap_state_q: CapIdle, CapW0, CapW1, CapTagWr
  input logic       rsp_w0,      // the response to W0 is taken
  input logic       rsp_w1,      // the response to W1 is taken, or its tag write is issued
  input logic       mismatch,    // the word read is not the one stored
  input logic       w0_ok,       // W0 matched
  input logic       tag_wr,      // W1 verified: its response becomes the tag write
  input logic       seq_err,     // a request out of the W0-then-W1 sequence is taken, or one is
                                 // presented between W1 and its tag write
  input logic       err          // fatal error
);
  covergroup wtrc_cg @(posedge clk_i);
    option.per_instance = 1;
    cp_state: coverpoint cap_state iff (rst_ni) {
      bins idle_to_w0   = (2'd0 => 2'd1);
      bins w0_to_w1     = (2'd1 => 2'd2);
      bins w1_to_tag_wr = (2'd2 => 2'd3);
      bins w1_to_idle   = (2'd2 => 2'd0);
      bins tag_wr_idle  = (2'd3 => 2'd0);
      bins w1_to_w0     = (2'd2 => 2'd1);
      bins tag_wr_to_w0 = (2'd3 => 2'd1);
    }
    // Each word matches or not; W1 only sets the tag if both did.
    cp_w0_match: coverpoint !mismatch iff (rst_ni && rsp_w0);
    cp_w1: coverpoint {w0_ok, !mismatch, tag_wr} iff (rst_ni && rsp_w1) {
      bins          set_tag     = {3'b111};
      bins          w1_mismatch = {3'b100};
      wildcard bins w0_mismatch = {3'b0?0};
      wildcard illegal_bins tag_without_match = {3'b0?1, 3'b?01};
    }
    cp_seq_err: coverpoint seq_err iff (rst_ni) { bins raised = {1'b1}; }
    cp_err:     coverpoint err     iff (rst_ni) { bins raised = {1'b1}; }
  endgroup

  bit en_fcov;
  initial void'($value$plusargs("en_cov=%d", en_fcov));
  `DV_FCOV_INSTANTIATE_CG(wtrc_cg, en_fcov)
endinterface
