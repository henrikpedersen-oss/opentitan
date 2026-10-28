// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_otp_rsw (sw/device/tests/eg2_rram/eg2_rram_otp_rsw.c; EG2 spec REQ_RRAM_17). The
// bench's half: on `EG2 RRAM otp_rsw intg off=..` it flips one bit of the OTP word's integrity
// byte in the RRAM's OTP integrity page (theory_of_operation.md "Integrity Scheme": one
// Hamming(72,64) byte per 64-bit chunk at 0x1FF400 + chunk / 8), rewriting the row so the macro's
// own ECC stays consistent: only rram_ctrl_otp's integrity check can see it. On
// `EG2 RRAM otp_rsw done` it restores the byte and resets the chip into the second boot.
class chip_sw_eg2_rram_otp_rsw_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_otp_rsw_vseq)

  `uvm_object_new

  localparam bit [31:0] OtpIntg = 32'h1FF400;

  rand int unsigned n_words;
  rand int unsigned set_class;
  rand bit          intg;

  constraint knobs_c {
    n_words inside {[1:14]};
    n_words dist {[1:2] :/ 30, [3:14] :/ 70};
    set_class inside {[0:2]};
    intg dist {1'b1 := 60, 1'b0 := 40};
  }

  virtual function void write_params();
    set_param("n_words", n_words);
    set_param("set_class", set_class);
    set_param("intg", intg);
  endfunction

  protected function string row_path(bit [31:0] rram_off);
    return $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), rram_off >> 4);
  endfunction

  virtual task body();
    string line;
    bit [31:0] off, rram_off;
    bit [127:0] saved, row;
    int unsigned bit_idx;
    super.body();
    wait_sw_line("EG2 RRAM otp_rsw intg off=", line, 200_000_000, 1);
    if (line == "") return;
    if (!($sscanf(line, "EG2 RRAM otp_rsw intg off=0x%h", off) == 1)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 RRAM otp_rsw intg off=0x%%h\", off) == 1) %s", line))
    rram_off = OtpIntg + off[31:3];
    saved = rram_ctrl_bkdr_util_pkg::rram_row_read(row_path(rram_off));
    row = saved;
    bit_idx = 8 * rram_off[3:0] + $urandom_range(0, 7);
    row[bit_idx] = ~row[bit_idx];
    announce("chip_sw_eg2_rram_otp_rsw_vseq", "otp_ctrl",
             $sformatf("OTP integrity byte of word 0x%0h (RRAM 0x%0h) bit %0d flipped", off,
                       rram_off, bit_idx % 8));
    rram_ctrl_bkdr_util_pkg::rram_row_write(row_path(rram_off), row);
    wait_sw_line("EG2 RRAM otp_rsw done", line, 200_000_000, 1);
    rram_ctrl_bkdr_util_pkg::rram_row_write(row_path(rram_off), saved);
    if (line == "") return;
    set_param("boot_phase", 1);
    `uvm_info(`gfn, "EG2 RRAM otp_rsw: integrity byte restored, second boot", UVM_LOW)
    apply_reset();
  endtask

endclass : chip_sw_eg2_rram_otp_rsw_vseq
