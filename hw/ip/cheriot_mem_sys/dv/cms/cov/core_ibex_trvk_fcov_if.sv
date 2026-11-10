// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Functional coverage for the temporal revocation (TRVK) block, ibex_trvk.
//
// ibex_trvk sits on the data bus between the core and memory. When a load returns
// a capability it looks up the revocation bitmap and clears the tag if the
// capability has been revoked, which is what enforces temporal safety (REQ_TMP).
//
// This is bound to ibex_trvk rather than ibex_core: the block is instantiated at
// ibex_top (`i_ibex_trvk`), so the ibex_core-scoped core_ibex_fcov_if cannot reach
// it. Binding to the module directly also means `.*` picks up the internal decision
// signals by name, which are the ones worth covering -- the ports alone do not show
// why a capability was revoked.

interface core_ibex_trvk_fcov_if #(
  // The bound filter's RevBitmapSizeBytes (ibex_trvk, cheriot_trvk_core): the revocable window is
  // 64 times it from the heap base. 2026-10-25 (EG2 F38).
  parameter int unsigned RevBitmapSizeBytes = 32'd2048
) (
  input logic        clk_i,
  input logic        rst_ni,

  // Revocation decision
  input logic        revbm_revoked,
  input logic        revbm_out_of_range,
  input logic [31:0] revbm_bit_addr,
  input logic        revbm_req_required,
  input logic        revbm_outstanding_q,
  input logic [ 4:0] revbm_bit_select,
  input logic [31:0] revbm_rdata_i,
  input logic [ 1:0] revbm_rsp_data_intg_error,

  // Revocation bitmap port
  input logic        revbm_req_o,
  input logic        revbm_gnt_i,
  input logic        revbm_rvalid_i,
  input logic        revbm_err_i,

  // Capability classification
  input logic        is_sealing_cap,
  input logic        ptr_storage_valid_q,
  input logic        misalign_flag_out,
  input logic        misalign_flag_out_valid,

  // Upstream (core-facing) response
  input logic        upstream_rvalid_o,
  input logic        upstream_tag_o
);

  `include "dv_fcov_macros.svh"

  // Which term of revbm_revoked fired. revbm_revoked ORs three sources together, so
  // without splitting them the "revoked" bin can be filled entirely by error
  // injection while the functional bitmap path stays untested.
  logic revoked_by_bitmap;
  assign revoked_by_bitmap = revbm_rdata_i[revbm_bit_select];

  covergroup trvk_cg @(posedge clk_i);
    option.per_instance = 1;
    option.name = "trvk_cg";

    // The revocation verdict itself, sampled only when the bitmap response is valid.
    cp_trvk_revoked: coverpoint revbm_revoked iff (rst_ni && revbm_rvalid_i) {
      bins not_revoked = {1'b0};
      bins revoked     = {1'b1};
    }

    // Revoked via the bitmap bit -- the functional path, as opposed to an error.
    cp_trvk_revoked_by_bitmap: coverpoint revoked_by_bitmap iff (rst_ni && revbm_rvalid_i);

    // Revoked because the bitmap memory reported a device error.
    cp_trvk_revbm_err: coverpoint revbm_err_i iff (rst_ni && revbm_rvalid_i);

    // Revoked because the bitmap response failed its ECC check.
    cp_trvk_intg_error: coverpoint (|revbm_rsp_data_intg_error) iff (rst_ni && revbm_rvalid_i);

    // Bit position within the bitmap word. A revocation scheme that only ever
    // exercises bit 0 has not tested the bit-select decode.
    cp_trvk_bit_select: coverpoint revbm_bit_select iff (rst_ni && revbm_rvalid_i) {
      bins bit_low[]   = {[0:7]};
      bins bit_mid     = {[8:23]};
      bins bit_high[]  = {[24:31]};
    }

    // Capability base outside the bitmap range: revocation is skipped entirely.
    cp_trvk_out_of_range: coverpoint revbm_out_of_range iff (rst_ni && ptr_storage_valid_q);

    // Sealing capabilities are exempt from revocation; both arms must be seen or the
    // exemption is untested.
    cp_trvk_sealing_cap: coverpoint is_sealing_cap iff (rst_ni && ptr_storage_valid_q);

    // A lookup is required for this response.
    cp_trvk_req_required: coverpoint revbm_req_required iff (rst_ni);

    // Second word of a capability, which is when the lookup is issued.
    cp_trvk_misalign: coverpoint misalign_flag_out iff (rst_ni && misalign_flag_out_valid);

    // Bitmap port handshake. The backpressured bin (req without grant) is the one
    // that stalls the upstream load.
    cp_trvk_revbm_handshake: coverpoint {revbm_req_o, revbm_gnt_i} iff (rst_ni) {
      bins quiet         = {2'b00};
      bins backpressured = {2'b10};
      bins accepted      = {2'b11};
      ignore_bins gnt_without_req = {2'b01};
    }

    // A second lookup is held off while one is outstanding.
    cp_trvk_outstanding: coverpoint revbm_outstanding_q iff (rst_ni);

    // Tag presented to the core. Tag=0 here after a revocation is the observable
    // effect of the whole block.
    cp_trvk_upstream_tag: coverpoint upstream_tag_o iff (rst_ni && upstream_rvalid_o);

    // Revocation must be reached through the bitmap, not only through error paths.
    // ibex_trvk.sv: revbm_revoked = bitmap bit || revbm_err_i || |intg_error, and every term is
    // sampled in the same cycle, so a source without the verdict, or the verdict without a source,
    // is a defect. Several sources at once is reachable (+revbm_err_kind=intg|both).
    trvk_revoked_source_cross: cross cp_trvk_revoked, cp_trvk_revoked_by_bitmap,
                                     cp_trvk_revbm_err, cp_trvk_intg_error {
      illegal_bins source_without_verdict =
          binsof(cp_trvk_revoked.not_revoked) &&
          (binsof(cp_trvk_revoked_by_bitmap) intersect {1'b1} ||
           binsof(cp_trvk_revbm_err) intersect {1'b1} ||
           binsof(cp_trvk_intg_error) intersect {1'b1});
      illegal_bins verdict_without_source =
          binsof(cp_trvk_revoked.revoked) &&
          binsof(cp_trvk_revoked_by_bitmap) intersect {1'b0} &&
          binsof(cp_trvk_revbm_err) intersect {1'b0} &&
          binsof(cp_trvk_intg_error) intersect {1'b0};
    }

    // Revocation arriving while another lookup is outstanding.
    trvk_revoked_outstanding_cross: cross cp_trvk_revoked, cp_trvk_outstanding {
      // A bitmap response only answers an outstanding lookup (ibex_trvk.sv
      // RevbmRspOnlyWhenOutstanding_A); revbm_outstanding_q clears only on the response itself
      illegal_bins rsp_not_outstanding = binsof(cp_trvk_outstanding) intersect {1'b0};
    }

    // A sealing capability must never be revoked: the exemption check is upstream of
    // the request, so this cross should leave the {revoked, sealing} bin empty.
    trvk_sealing_cross: cross cp_trvk_revoked, cp_trvk_sealing_cap {
      // No lookup is made for a sealing capability (revbm_req_required needs !is_sealing_cap),
      // and the stream join holds the response head that is_sealing_cap decodes until
      // revbm_rvalid_i (ibex_trvk.sv), so a bitmap response never sees one
      illegal_bins lookup_for_sealing_cap = binsof(cp_trvk_sealing_cap) intersect {1'b1};
    }
  endgroup

  bit en_trvk_cov;
  initial begin
    void'($value$plusargs("enable_ibex_fcov=%d", en_trvk_cov));
  end

  `DV_FCOV_INSTANTIATE_CG(trvk_cg, en_trvk_cov)

  // Where a capability's base falls against the bitmap (EG2 F38): in the window, in the band
  // between the bitmap's size and the next power of two (what a width-only check looked up, past
  // the bitmap's end), or beyond. The band exists only when the size is not a power of two, so the
  // covergroup is generated only then; for a power-of-two bitmap cp_trvk_out_of_range covers it.
  localparam int unsigned SizeBits = RevBitmapSizeBytes * 8;
  localparam int unsigned Pow2Bits = (32'd1 << $clog2(RevBitmapSizeBytes)) * 8;

  if (SizeBits < Pow2Bits) begin : g_window_band
    covergroup trvk_window_cg @(posedge clk_i);
      option.per_instance = 1;
      option.name = "trvk_window_cg";

      cp_trvk_base_window: coverpoint revbm_bit_addr iff (rst_ni && ptr_storage_valid_q) {
        bins in_window   = {[0 : SizeBits - 1]};
        bins past_size   = {[SizeBits : Pow2Bits - 1]};
        bins beyond_pow2 = {[Pow2Bits : 32'hffff_ffff]};
      }
      cp_trvk_out_of_range_w: coverpoint revbm_out_of_range iff (rst_ni && ptr_storage_valid_q);

      // Only the window is looked up: a base past the bitmap's size must be out of range (F38
      // looked it up, read past the bitmap and revoked it), and one inside must not be.
      trvk_window_range_cross: cross cp_trvk_base_window, cp_trvk_out_of_range_w {
        illegal_bins past_size_looked_up =
            binsof(cp_trvk_base_window.past_size) && binsof(cp_trvk_out_of_range_w) intersect {1'b0};
        illegal_bins beyond_looked_up =
            binsof(cp_trvk_base_window.beyond_pow2) && binsof(cp_trvk_out_of_range_w) intersect {1'b0};
        illegal_bins in_window_skipped =
            binsof(cp_trvk_base_window.in_window) && binsof(cp_trvk_out_of_range_w) intersect {1'b1};
      }
    endgroup

    `DV_FCOV_INSTANTIATE_CG(trvk_window_cg, en_trvk_cov)
  end

endinterface
