// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_ecc_corr_rewrite (sw/device/tests/eg2_rram/eg2_rram_ecc_corr_rewrite.c; EG2 spec
// REQ_RRAM_09). The bench's half: on `EG2 RRAM ecc off=.. bits=..` it flips data bits of that
// RRAM row by backdoor, leaving the row's ECC bits as they are: one bit in one 64-bit half for a
// single-bit (corr1) error, one bit in each half for a double-bit (corr2) error (README: two
// Hamming(72,64) codes, one per half; theory_of_operation.md "RRAM ECC Error Handling"). On
// `EG2 RRAM ecc rewritten off=..` it checks that the row is a clean codeword again.
class chip_sw_eg2_rram_ecc_corr_rewrite_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_ecc_corr_rewrite_vseq)

  `uvm_object_new

  rand int unsigned n_errors;
  rand int unsigned w_bits_1, w_bits_2;
  rand int unsigned path;
  rand int unsigned loc_class;
  rand bit          scramble;

  constraint knobs_c {
    n_errors inside {[1:8]};
    n_errors dist {[1:2] :/ 30, [3:8] :/ 70};
    // Both kinds always reachable; one dominates in a third of the runs.
    w_bits_1 inside {[10:90]};
    w_bits_2 inside {[10:90]};
    path dist {0 := 30, 1 := 30, 2 := 40};
    loc_class inside {[0:3]};
    scramble dist {1'b1 := 60, 1'b0 := 40};
  }

  virtual function void write_params();
    set_param("n_errors", n_errors);
    set_param("w_bits_1", w_bits_1);
    set_param("w_bits_2", w_bits_2);
    set_param("path", path);
    set_param("loc_class", loc_class);
    set_param("scramble", scramble);
  endfunction

  protected function string row_path(bit [31:0] off);
    return $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4);
  endfunction

  virtual task body();
    string line;
    int unsigned rounds = 0;
    bit [31:0] n = n_errors;
    super.body();
    void'($value$plusargs("eg2_param_n_errors=%d", n));
    repeat (n) begin
      bit [31:0] off, bits;
      bit [143:0] mask, raw;
      uvm_hdl_data_t v;
      wait_sw_line("EG2 RRAM ecc off=", line, 200_000_000, rounds > 0);
      if (line == "") return;
      if (!($sscanf(line, "EG2 RRAM ecc off=0x%h bits=%d", off, bits) == 2)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 RRAM ecc off=0x%%h bits=%%d\", off, bits) == 2) %s", line))
      mask = '0;
      if (bits == 2) begin
        mask[$urandom_range(0, 63)] = 1'b1;
        mask[$urandom_range(64, 127)] = 1'b1;
      end else begin
        mask[$urandom_range(0, 1) ? $urandom_range(64, 127) : $urandom_range(0, 63)] = 1'b1;
      end
      announce("chip_sw_eg2_rram_ecc_corr_rewrite_vseq", "rram_ctrl,insn",
               $sformatf("RRAM row 0x%0h data mask 0x%036h (%0d-bit correctable)", off, mask,
                         bits));
      rram_ctrl_bkdr_util_pkg::rram_row_flip_bits(row_path(off), mask);
      wait_sw_line("EG2 RRAM ecc rewritten off=", line, 200_000_000, 1);
      if (line == "") return;
      `DV_CHECK_FATAL(uvm_hdl_read(row_path(off), v), row_path(off))
      raw = v[143:0];
      `DV_CHECK_EQ(raw, rram_ctrl_bkdr_util_pkg::emul_encode_word(raw[127:0]),
                   $sformatf("RRAM row 0x%0h is not a clean codeword after the Rewrite", off))
      rounds++;
      `uvm_info(`gfn, $sformatf("EG2 RRAM ecc: round %0d at 0x%0h (%0d bits) clean after Rewrite",
                                rounds, off, bits), UVM_LOW)
    end
  endtask

endclass : chip_sw_eg2_rram_ecc_corr_rewrite_vseq
