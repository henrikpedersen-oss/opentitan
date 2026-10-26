// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_otp_ctrl_dai_ecc_error (sw/device/tests/eg2_mem/eg2_otp_ctrl_dai_ecc_error.c; EG2 spec
// REQ_OTP_01). The bench's half:
//   EG2 MEM otp plant 0x<addr>       one bit of the 32-bit OTP word at otp_ctrl offset <addr>
//                                    flipped in the RRAM row that holds it; the row's macro ECC is
//                                    re-encoded (rram_row_write), the OTP integrity page is not, so
//                                    the OTP plug's integrity check fails (REQ_RRAM_17)
//   EG2 MEM otp restore              the row written back as it was
//   EG2 MEM otp restore and reset    the same, then boot_phase = 1 and a power-on reset
class chip_sw_eg2_otp_ctrl_dai_ecc_error_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_otp_ctrl_dai_ecc_error_vseq)

  `uvm_object_new

  rand int unsigned partition;
  rand int unsigned word;

  // VENDOR_TEST (the recoverable case) one run in five; the other unbuffered partitions share the
  // rest. The word is drawn by the software within the partition.
  constraint partition_c {
    partition dist {0 := 20, [1:4] :/ 80};
    word == 16'hffff;
  }

  virtual function void write_params();
    set_param("partition", partition);
    set_param("word", word);
    set_param("boot_phase", 0);
  endfunction

  protected function string row_path(bit [31:0] addr, output int unsigned byte_in_row);
    int unsigned abs_addr = addr + rram_ctrl_bkdr_util_pkg::rram_ctrl_otp_bkdr_util::OtpDataByteOffset;
    byte_in_row = abs_addr % 16;
    return $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[Otp].get_full_path(0), abs_addr >> 4);
  endfunction

  virtual task body();
    super.body();
    fork
      plant_and_restore();
    join_none
  endtask

  protected task plant_and_restore();
    string line, path;
    bit [31:0] addr;
    int unsigned byte_in_row, bit_idx;
    bit [rram_ctrl_bkdr_util_pkg::EmulDataWidth-1:0] row, bad;
    wait_sw_line("EG2 MEM otp plant ", line, 32'd4_000_000_000);
    if (!($sscanf(line, "EG2 MEM otp plant 0x%h", addr) == 1)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 MEM otp plant 0x%%h\", addr) == 1) %s", line))
    path = row_path(addr, byte_in_row);
    row = rram_ctrl_bkdr_util_pkg::rram_row_read(path);
    bit_idx = byte_in_row * 8 + $urandom_range(0, 31);
    bad = row;
    bad[bit_idx] = ~bad[bit_idx];
    announce("chip_sw_eg2_otp_ctrl_dai_ecc_error_vseq", "otp_ctrl",
             $sformatf("OTP word 0x%0h: data bit %0d flipped, integrity kept", addr, bit_idx));
    rram_ctrl_bkdr_util_pkg::rram_row_write(path, bad);
    @(cfg.sw_logger_vif.printed_log_event);
    wait_sw_line("EG2 MEM otp restore", line, 32'd4_000_000_000);
    rram_ctrl_bkdr_util_pkg::rram_row_write(path, row);
    if (line == "EG2 MEM otp restore and reset") begin
      set_param("boot_phase", 1);
      `uvm_info(`gfn, "EG2 OTP: uncorrectable error seen; power-on reset into the second boot",
                UVM_LOW)
      apply_reset();
    end
  endtask

endclass : chip_sw_eg2_otp_ctrl_dai_ecc_error_vseq
