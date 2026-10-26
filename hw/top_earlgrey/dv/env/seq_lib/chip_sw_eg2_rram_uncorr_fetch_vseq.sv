// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_uncorr_fetch (sw/device/tests/eg2_rram/eg2_rram_uncorr_fetch.c; EG2 spec
// REQ_RRAM_10). The bench's half:
//   - `EG2 RRAM uncorr off=..`: two data bits flipped in one 64-bit half of that RRAM row, ECC bits
//     untouched: uncorrectable for the half's Hamming(72,64) code (theory_of_operation.md
//     "Multi-Bit Error (Uncorrectable)");
//   - `EG2 RRAM uncorr esc off=..` (DIS.LOCAL_ESC_RELBL_ERR set): the same, then, once the software's
//     load has met it, through SBA: a read of the RRAM host window is refused (RRAM disabled:
//     "all host and controller requests are blocked") and FAULT_STATUS still reads, with
//     PHY_RELBL_ERR set ("registers remain accessible after disable");
//   - `EG2 RRAM uncorr done` or the escalation step: the rows repaired (the same bits flipped
//     back), boot_phase = 1, and a reset into the software's second boot, which checks the clean
//     state.
class chip_sw_eg2_rram_uncorr_fetch_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_uncorr_fetch_vseq)

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  rand bit order;
  rand bit escalate;

  constraint knobs_c {
    escalate dist {1'b1 := 60, 1'b0 := 40};
  }

  virtual function void write_params();
    set_param("order", order);
    set_param("escalate", escalate);
  endfunction

  // The flips made, per row byte offset, to undo them before the second boot.
  protected bit [143:0] flipped[bit [31:0]];

  // The reference model's ISS (en_eg2_cosim), told of the faults the corruption makes, as
  // chip_sw_eg2_fetch_intg_vseq does: Spike fetches from its own memory, so without being told it
  // ran the corrupted call without the instruction access fault the chip takes (review
  // 2026-10-25). The words the software writes through the controller reach Spike by
  // eg2_iss_wrapper's own watch of rram_ctrl's writes, also for the second boot. Since review
  // R11/R12 (2026-10-25) the corrupted row's words are armed, and the model takes a fetch fault
  // only on the instruction the chip took one on (Spike, or CHERIoT-Sail in CHERIoT mode); a load's
  // fault reaches either model with its bus error. So the model always knows the fault and the
  // instruction check is not among its targets: a mismatch there is a real one.
  protected eg2_iss_wrapper iss;
  // The `EG2 RRAM uncorr off=` lines seen: the software's first access is the fetch when `order`
  // is 1, its second when 0 (eg2_rram_uncorr_fetch.c access_uncorr).
  protected int unsigned n_uncorr;

  protected function void find_iss();
    uvm_component c = uvm_root::get().find("*.m_eg2_iss");
    if (c != null) void'($cast(iss, c));
  endfunction

  protected function string row_path(bit [31:0] off);
    return $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4);
  endfunction

  protected function void corrupt(bit [31:0] off);
    bit [143:0] mask = '0;
    int unsigned lo = $urandom_range(0, 1) ? 64 : 0;
    int unsigned a = lo + $urandom_range(0, 63);
    int unsigned b;
    do b = lo + $urandom_range(0, 63); while (b == a);
    mask[a] = 1'b1;
    mask[b] = 1'b1;
    // TODO(inject-registry): announce() reaches the registry only in EG2_PERIPH_SB builds.
    // Not "insn": the model is told of the fault (above; R12, 2026-10-25).
    announce("chip_sw_eg2_rram_uncorr_fetch_vseq", "rram_ctrl",
             $sformatf("RRAM row 0x%0h data mask 0x%036h (uncorrectable)", off, mask));
    rram_ctrl_bkdr_util_pkg::rram_row_flip_bits(row_path(off), mask);
    if (flipped.exists(off)) flipped[off] ^= mask;
    else flipped[off] = mask;
  endfunction

  protected task second_boot();
    foreach (flipped[off]) begin
      if (flipped[off] != '0) rram_ctrl_bkdr_util_pkg::rram_row_flip_bits(row_path(off), flipped[off]);
    end
    flipped.delete();
    set_param("boot_phase", 1);
    `uvm_info(`gfn, "EG2 RRAM uncorr: rows repaired, second boot", UVM_LOW)
    apply_reset();
  endtask

  virtual task body();
    string line;
    super.body();
    find_iss();
    forever begin
      bit [31:0] off;
      wait_sw_line("EG2 RRAM uncorr ", line, 200_000_000, 1);
      if (line == "") return;
      if ($sscanf(line, "EG2 RRAM uncorr off=0x%h", off) == 1) begin
        bit fetch = (n_uncorr == 0) ? order : !order;
        n_uncorr++;
        corrupt(off);
        // The call's fetch of the corrupted word fails on the chip; the model's does too, on the
        // instruction the chip took the fault on. Every word of the row is armed: the flipped half
        // is either half of the 16-byte row (R11, 2026-10-25). The load's fault needs nothing: its
        // bus error reaches the model with the access.
        if (fetch && iss != null) begin
          bit [31:0] row = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + {off[31:4], 4'h0};
          bit        told;
          for (int w = 0; w < 4; w++) told = iss.iside_error_event(row + 4 * w);
          if (told) begin
            `uvm_info(`gfn, $sformatf("EG2 RRAM uncorr: the model told fetches of row 0x%0h fail",
                                      off), UVM_LOW)
          end
        end
      end else if ($sscanf(line, "EG2 RRAM uncorr esc off=0x%h", off) == 1) begin
        bit [31:0] data;
        sba_access_err_e st;
        corrupt(off);
        // Once the load meets it rram_ctrl disables itself and every fetch from RRAM, where the
        // software runs, fails until the reset below: nothing the ISS can follow. The ISS stops
        // after that load's fault, so the load and its fault are still compared (R8, 2026-10-25:
        // it stopped here, before the load).
        if (iss != null) iss.stop_after_fault("chip_sw_rram_uncorr_fetch: RRAM local escalation");
        // The software's load comes within a few thousand cycles.
        cfg.chip_vif.cpu_clk_rst_if.wait_clks(20_000);
        cfg.debugger.set_dmactive(1);
        sba_rd32(top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR + off + 4, data, st);
        `DV_CHECK_NE(st, SbaErrNone, "an RRAM host read over SBA succeeded after local escalation")
        sba_rd32(top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR +
                 rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET, data, st);
        `DV_CHECK_EQ(st, SbaErrNone, "FAULT_STATUS not readable after local escalation")
        `DV_CHECK_EQ(data[9], 1'b1, "FAULT_STATUS.PHY_RELBL_ERR not set after local escalation")
        cfg.debugger.set_dmactive(0);
        second_boot();
        break;
      end else if (line == "EG2 RRAM uncorr done") begin
        second_boot();
        break;
      end
    end
  endtask

endclass : chip_sw_eg2_rram_uncorr_fetch_vseq
