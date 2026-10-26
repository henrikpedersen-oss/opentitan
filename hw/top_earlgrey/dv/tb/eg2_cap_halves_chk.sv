// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_cheriot_cap_halves's assertion (eg2_top_verification spec "Revocation", "Corner cases";
// REQ_CMS_10, REQ_CDM_09): the condition in the headers of ibex_trvk and cheriot_trvk_tlul that
// the formal proofs assume, checked on a TL-UL port of the chip:
//
//   "a tagged capability load always arrives downstream as exactly two consecutive, uninterrupted
//    32-bit responses from a single, in-order requester."
//
// A capability load is a Get at a 64-bit-aligned address, hinted (the core port: the tag sideband
// on a read, cheriot_mem_sys theory_of_operation.md "Tag Filter") or not (the TBRE's port, whose
// reads are all capability words). Checked, from the port's A and D handshakes:
//   - in order: the next A handshake after the lower word's is the Get of the upper word
//     (address + 4);
//   - back to back: the next D handshake after the lower word's response is the upper word's
//     response (no other response in between), and the lower word is answered before the upper.
// A violation is a uvm_error `EG2_CAP_HALVES`. Off unless +eg2_cap_halves=1 (eg2_cap_halves.svh);
// +eg2_cap_halves_selftest=1 expects the upper word at address + 8 instead, so the first
// capability load fails it (the must-fail run that shows the check can fail).
module eg2_cap_halves_chk #(
  parameter string Name = "core"
) (
  input logic              clk_i,
  input logic              rst_ni,
  input tlul_pkg::tl_h2d_t h2d,
  input tlul_pkg::tl_d2h_t d2h,
  input logic              hint,
  input bit                use_hint,
  input bit                on,
  input bit                selftest
);
  import uvm_pkg::*;
  `include "uvm_macros.svh"

  typedef struct packed {
    logic [top_pkg::TL_AIW-1:0] src;
    logic [31:0]                addr;
    logic                       lo;
    logic                       hi;
  } req_t;

  req_t q[$];
  bit expect_hi_a, expect_hi_d;
  logic [31:0] lo_addr;
  int unsigned caps, fails;

  function automatic void fail(string msg);
    fails++;
    `uvm_error("EG2_CAP_HALVES", $sformatf("%s port: %s", Name, msg))
  endfunction

  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      q.delete();
      expect_hi_a = 1'b0;
      expect_hi_d = 1'b0;
    end else if (on) begin
      // A channel.
      if (h2d.a_valid && d2h.a_ready) begin
        req_t r;
        bit is_read;
        is_read = h2d.a_opcode == tlul_pkg::Get;
        r.src = h2d.a_source;
        r.addr = h2d.a_address;
        r.lo = 1'b0;
        r.hi = 1'b0;
        if (expect_hi_a) begin
          logic [31:0] want;
          want = lo_addr + (selftest ? 32'd8 : 32'd4);
          if (!is_read || h2d.a_address != want) begin
            fail($sformatf({"the request after a capability's lower word (0x%08x) is a %s of ",
                            "0x%08x, not the read of 0x%08x"}, lo_addr,
                           is_read ? "read" : "write", h2d.a_address, want));
          end else begin
            r.hi = 1'b1;
          end
          expect_hi_a = 1'b0;
        end else if (is_read && h2d.a_address[2:0] == 3'b0 && (!use_hint || hint)) begin
          r.lo = 1'b1;
          expect_hi_a = 1'b1;
          lo_addr = h2d.a_address;
        end
        q.push_back(r);
      end
      // D channel.
      if (d2h.d_valid && h2d.d_ready) begin
        int idx;
        idx = -1;
        foreach (q[i]) begin
          if (q[i].src == d2h.d_source) begin
            idx = i;
            break;
          end
        end
        if (idx < 0) begin
          fail($sformatf("a response (source %0d) without an outstanding request", d2h.d_source));
        end else begin
          req_t r;
          r = q[idx];
          if (expect_hi_d) begin
            if (!r.hi) begin
              fail($sformatf({"the response after a capability's lower word is the one to ",
                              "0x%08x, not its upper word"}, r.addr));
            end else begin
              caps++;
            end
            expect_hi_d = 1'b0;
          end else if (r.hi) begin
            fail($sformatf("a capability's upper word (0x%08x) answered before its lower word",
                           r.addr));
          end
          if (r.lo) begin
            expect_hi_d = 1'b1;
            if (idx != 0) begin
              fail($sformatf("a capability's lower word (0x%08x) answered out of order", r.addr));
            end
          end
          q.delete(idx);
        end
      end
    end
  end

  final begin
    if (on) begin
      `uvm_info("EG2_CAP_HALVES", $sformatf("%s port: %0d capability loads checked, %0d failures",
                                            Name, caps, fails), UVM_LOW)
    end
  end
endmodule
