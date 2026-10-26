// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_otp_layout (sw/device/tests/eg2_rram/eg2_rram_otp_layout.c; EG2 spec REQ_RRAM_18).
// The bench's half, for every `EG2 RRAM otp_layout off=.. lo=.. hi=.. w=.. err=..` line: the RRAM
// macro's data array, read directly (not through the OTP backdoor util, whose offsets are what is
// checked), holds the DAI's value at 0x1FF600 + off, byte for byte (no scrambling, no address
// XOR), and the integrity page at 0x1FF400 holds, at off / 8, the Hamming(72,64) check byte of the
// 64-bit chunk (README "Address Map", theory_of_operation.md "Integrity Scheme"). For a
// `part off=.. size=..` line (a partition the DAI cannot read) the integrity bytes of its first and
// last chunk.
class chip_sw_eg2_rram_otp_layout_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_otp_layout_vseq)

  `uvm_object_new

  localparam bit [31:0] OtpStart = 32'h1FF600;
  localparam bit [31:0] OtpIntg  = 32'h1FF400;

  rand int unsigned order;
  constraint order_c {
    order inside {[0:2]};
  }

  virtual function void write_params();
    set_param("order", order);
  endfunction

  protected function bit [7:0] macro_byte(bit [31:0] rram_off);
    bit [127:0] row = rram_ctrl_bkdr_util_pkg::rram_row_read(
        $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), rram_off >> 4));
    return row[8 * rram_off[3:0] +: 8];
  endfunction

  protected function bit [63:0] macro_chunk(bit [31:0] rram_off);
    bit [63:0] c;
    for (int i = 0; i < 8; i++) c[8 * i +: 8] = macro_byte(rram_off + i);
    return c;
  endfunction

  protected function void check_intg(bit [31:0] otp_off);
    bit [31:0] chunk_off = {otp_off[31:3], 3'b000};
    bit [71:0] enc = prim_secded_pkg::prim_secded_hamming_72_64_enc(macro_chunk(OtpStart +
                                                                               chunk_off));
    bit [7:0] got = macro_byte(OtpIntg + chunk_off / 8);
    `DV_CHECK_EQ(got, enc[71:64], $sformatf("OTP integrity byte of chunk 0x%0h at RRAM 0x%0h",
                                            chunk_off, OtpIntg + chunk_off / 8))
  endfunction

  virtual task body();
    string line;
    int unsigned lines;
    super.body();
    forever begin
      bit [31:0] off, lo, hi, w, err, size;
      wait_sw_line("EG2 RRAM otp_layout ", line, 200_000_000, 1);
      if (line == "" || line == "EG2 RRAM otp_layout done") break;
      if ($sscanf(line, "EG2 RRAM otp_layout off=0x%h lo=0x%h hi=0x%h w=%d err=%d", off, lo, hi,
                  w, err) == 5) begin
        if (err == 0) begin
          bit [63:0] exp = {hi, lo};
          for (int i = 0; i < w; i++) begin
            `DV_CHECK_EQ(macro_byte(OtpStart + off + i), exp[8 * i +: 8],
                         $sformatf("OTP byte 0x%0h in the RRAM at 0x%0h", off + i,
                                   OtpStart + off + i))
          end
        end
        check_intg(off);
        lines++;
      end else if ($sscanf(line, "EG2 RRAM otp_layout part off=0x%h size=0x%h", off, size) == 2)
          begin
        `DV_CHECK_LE(OtpStart + off + size, 32'h200000, "partition beyond the RRAM")
        check_intg(off);
        check_intg(off + size - 8);
        lines++;
      end
    end
    `uvm_info(`gfn, $sformatf("EG2 RRAM otp_layout: %0d locations checked", lines), UVM_LOW)
  endtask

endclass : chip_sw_eg2_rram_otp_layout_vseq
