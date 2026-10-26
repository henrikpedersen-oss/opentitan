// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_scramble (sw/device/tests/eg2_rram/eg2_rram_scramble.c; EG2 spec REQ_RRAM_07,
// REQ_SEC_18). The bench's half: on each `EG2 RRAM scramble off=.. n=.. scr=.. salt=..` line it
// reads the RRAM macro rows of the region by backdoor and compares each with the plaintext the
// software wrote, scrambled by rram_ctrl_bkdr_util's XEX/PRINCE model (theory_of_operation.md
// "RRAM Scrambling") after the address XOR ("Address-XOR"), with the keys the otp_ctrl ->
// rram_ctrl key interface delivered; unscrambled, the row holds the address-XORed plaintext.
// With key_change it then writes new NVM key seeds into OTP's SECRET1 and resets the chip into
// the software's second boot (boot_phase = 1).
class chip_sw_eg2_rram_scramble_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_scramble_vseq)

  `uvm_object_new

  localparam string RramCtrl = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl";

  rand int unsigned n_words;
  rand int unsigned loc_class;
  rand bit          scramble;
  rand bit          key_change;

  constraint knobs_c {
    // 1 RRAM word to several pages, log-uniform.
    n_words dist {4 := 10, [8:32] :/ 30, [36:128] :/ 40, [132:256] :/ 20};
    n_words % 4 == 0;
    loc_class inside {[0:3]};
    scramble dist {1'b1 := 70, 1'b0 := 30};
    key_change dist {1'b1 := 50, 1'b0 := 50};
  }

  virtual function void write_params();
    set_param("n_words", n_words);
    set_param("loc_class", loc_class);
    set_param("scramble", scramble);
    set_param("key_change", key_change);
  endfunction

  // eg2_rram_pattern (eg2_rram.h), from the software seed.
  protected function bit [31:0] pattern(bit [31:0] seed, bit [31:0] salt, int unsigned i);
    bit [31:0] x = seed ^ salt ^ (i * 32'h9e3779b9);
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
  endfunction

  protected function bit [31:0] sw_seed_read();
    bit [7:0] b[] = new[4];
    sw_symbol_backdoor_read("kEg2Seed", b);
    return {b[3], b[2], b[1], b[0]};
  endfunction

  protected function bit [127:0] read_key(string name);
    uvm_hdl_data_t v;
    if (!(uvm_hdl_read({RramCtrl, ".", name}, v))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({RramCtrl, \".\", name}, v)) %s", {"cannot read ", name}))
    return v[127:0];
  endfunction

  protected task check_macro(string line);
    rram_ctrl_bkdr_util_pkg::rram_ctrl_bkdr_util u;
    bit [31:0] off, n, scr, salt, seed;
    bit [127:0] addr_key, data_key;
    int mismatches;
    if (!($sscanf(line, "EG2 RRAM scramble off=0x%h n=%d scr=%d salt=0x%h", off, n, scr,
                            salt) == 4)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 RRAM scramble off=0x%%h n=%%d scr=%%d salt=0x%%h\", off, n, scr, salt) == 4) %s", line))
    `downcast(u, cfg.mem_bkdr_util_h[RramData])
    seed = sw_seed_read();
    addr_key = read_key("addr_key");
    data_key = read_key("data_key");
    for (int unsigned w = 0; w < n; w += 4) begin
      bit [127:0] plain, exp, got;
      bit [31:0] byte_off = off + 4 * w;
      for (int j = 0; j < 4; j++) begin
        // The address XOR: each bus word with its bus-word address (byte offset / 4).
        plain[32 * j +: 32] = pattern(seed, salt, w + j) ^ ((byte_off + 4 * j) >> 2);
      end
      exp = scr ? u.rram_scramble_data(plain, byte_off >> 4, addr_key, data_key) : plain;
      got = u.read(byte_off);
      if (got !== exp) begin
        mismatches++;
        `uvm_error(`gfn, $sformatf("RRAM row at 0x%0h: macro 0x%032h, model %s 0x%032h", byte_off,
                                   got, scr ? "ciphertext" : "plaintext", exp))
      end
    end
    `uvm_info(`gfn, $sformatf("EG2 RRAM scramble: %0d rows at 0x%0h checked (%s), %0d mismatches",
                              n / 4, off, scr ? "ciphertext" : "plaintext", mismatches), UVM_LOW)
  endtask

  virtual task body();
    string line;
    bit [31:0] kc = key_change;
    super.body();
    void'($value$plusargs("eg2_param_key_change=%d", kc));
    wait_sw_line("EG2 RRAM scramble off=", line);
    if (line == "") return;
    check_macro(line);
    if (kc) begin
      bit [otp_ctrl_reg_pkg::NvmAddrKeySeedSize*8-1:0] addr_seed;
      bit [otp_ctrl_reg_pkg::NvmDataKeySeedSize*8-1:0] data_seed;
      bit [otp_ctrl_reg_pkg::SramDataKeySeedSize*8-1:0] sram_seed;
      wait_sw_line("EG2 RRAM scramble rekey", line);
      if (line == "") return;
      `DV_CHECK_STD_RANDOMIZE_FATAL(addr_seed)
      `DV_CHECK_STD_RANDOMIZE_FATAL(data_seed)
      `DV_CHECK_STD_RANDOMIZE_FATAL(sram_seed)
      otp_write_secret1_partition(cfg.mem_bkdr_util_h[Otp], addr_seed, data_seed, sram_seed);
      set_param("boot_phase", 1);
      `uvm_info(`gfn, "EG2 RRAM scramble: new NVM key seeds, second boot", UVM_LOW)
      apply_reset();
      wait_sw_line("EG2 RRAM scramble off=", line, 200_000_000, 1);
      if (line == "") return;
      check_macro(line);
    end
  endtask

endclass : chip_sw_eg2_rram_scramble_vseq
