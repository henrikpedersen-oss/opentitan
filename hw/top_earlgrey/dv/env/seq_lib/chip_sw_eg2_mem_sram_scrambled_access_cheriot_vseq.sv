// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_sram_ctrl_scrambled_access_cheriot (sw/device/tests/eg2_mem/
// eg2_sram_scrambled_access_cheriot.c; EG2 spec REQ_OTP_02): the bench's half, as
// chip_sw_sram_ctrl_scrambled_access_vseq does it for the RISC-V test, at the software's lines:
//   EG2 MEM scr <sram> rekey             (sram: main, sec, ret) the key and nonce otp_ctrl's key
//                                        derivation acknowledges on the controller's slot (main 0,
//                                        sec 1, ret 2) are taken; once the controller's
//                                        STATUS.SCR_KEY_VALID is set it must hold exactly them
//                                        (REQ_OTP_02: the key reaches its consumer)
//   EG2 MEM scr <sram> renew 0x<addr>    the same; then, for main and ret, the 16 words at <addr>
//                                        (after the frame's pattern words) are written through the
//                                        RAM backdoor, scrambled with that key and nonce and with
//                                        integrity (bd(): the software computes the same words
//                                        from bd_seed)
// Sec SRAM has no backdoor model in this bench (no chip_mem_e for it), so only its key is checked
// here; the software writes its backdoor words itself.
class chip_sw_eg2_sram_scrambled_access_cheriot_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_scrambled_access_cheriot_vseq)

  `uvm_object_new

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string PdAon = "tb.dut.top_earlgrey.earlgrey_pd_aon";
  localparam string Kdi = {PdMain, ".u_otp_ctrl.u_otp_ctrl_kdi.sram_otp_key_o"};
  localparam string KeyValid = ".u_reg_regs.status_scr_key_valid_qs";
  localparam int unsigned Words = 16;
  // How long a key derivation and the key's arrival may take (1 ns polls).
  localparam int unsigned KeyTimeoutNs = 5_000_000;

  rand int unsigned n_words;
  rand int unsigned rekey;
  rand bit [31:0] bd_seed;

  // The spec's draws: pattern words 16-1,024 and 0-4 extra key renewals, uniform.
  constraint n_words_c {
    n_words inside {[16:1024]};
  }
  constraint rekey_c {
    rekey inside {[0:4]};
  }

  virtual function void write_params();
    // A pinned value is the one the software reads, so the bench uses it too.
    void'($value$plusargs("eg2_param_bd_seed=%d", bd_seed));
    set_param("n_words", n_words);
    set_param("rekey", rekey);
    set_param("bd_seed", bd_seed);
  endfunction

  // eg2_sram_scrambled_access_cheriot.c backdoor().
  protected function bit [31:0] bd(int unsigned sram, int unsigned i);
    bit [31:0] x = bd_seed ^ (sram * 32'h0100_0193) ^ (i * 32'h9e37_79b9);
    x ^= x >> 16;
    x *= 32'h7feb_352d;
    x ^= x >> 15;
    return x;
  endfunction

  protected function string ctrl_path(int unsigned sram);
    case (sram)
      0: return {PdMain, ".u_sram_ctrl_main"};
      1: return {PdMain, ".u_sram_ctrl_sec"};
      default: return {PdAon, ".u_sram_ctrl_ret"};
    endcase
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), $sformatf("cannot read %s", path))
    return v;
  endfunction

  virtual task cpu_init();
    super.cpu_init();
    // As chip_sw_sram_ctrl_scrambled_access_vseq: no X read from a re-keyed SRAM.
    for (int i = 0; i < cfg.num_ram_ret_tiles; i++) begin
      cfg.mem_bkdr_util_h[chip_mem_e'(RamRet0 + i)].set_mem();
    end
    for (int i = 0; i < cfg.num_ram_main_tiles; i++) begin
      cfg.mem_bkdr_util_h[chip_mem_e'(RamMain0 + i)].set_mem();
    end
    // Behind the core's back: the model has no value for these SRAMs' bytes from now on (review
    // R19, 2026-10-26).
    eg2_iss_wrapper::backdoor_undefined(top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR,
                                        top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES);
    eg2_iss_wrapper::backdoor_undefined(top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR,
                                        top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES);
  endtask

  virtual task body();
    string paths[$];
    for (int s = 0; s < 3; s++) begin
      paths.push_back({ctrl_path(s), ".key_q"});
      paths.push_back({ctrl_path(s), ".nonce_q"});
      paths.push_back({ctrl_path(s), KeyValid});
      paths.push_back($sformatf("%s[%0d].ack", Kdi, s));
    end
    foreach (paths[i]) begin
      `DV_CHECK_FATAL(uvm_hdl_check_path(paths[i]), {"no such path: ", paths[i]})
    end
    // The re-keyed words fail their integrity on purpose (chip_sw_sram_ctrl_scrambled_access_vseq).
    cfg.disable_d_user_data_intg_check_for_passthru_mem = 1;
    cfg.en_scb_mem_chk = 0;
    super.body();
    fork
      check_lines();
    join_none
  endtask

  protected task check_lines();
    forever begin
      string line, name;
      bit [31:0] addr;
      bit backdoor;
      wait_sw_line("EG2 MEM scr ", line, 32'd4_000_000_000);
      if (line == "") return;
      // `renew` lines carry the backdoor words' address, `rekey` lines nothing ($sscanf counts
      // the name alone on a `renew` line too, so the suffix decides).
      backdoor = $sscanf(line, "EG2 MEM scr %s renew 0x%h", name, addr) == 2;
      if (backdoor || (line.len() > 6 && line.substr(line.len() - 6, line.len() - 1) == " rekey" &&
                       $sscanf(line, "EG2 MEM scr %s", name) == 1)) begin
        int unsigned sram = name == "main" ? 0 : name == "sec" ? 1 : 2;
        if (!(name inside {"main", "sec", "ret"})) begin
          `uvm_fatal(`gfn, $sformatf("unknown SRAM in '%s'", line))
        end
        renewal(sram, backdoor, addr);
      end
      // The next line, not this one again.
      @(cfg.sw_logger_vif.printed_log_event);
    end
  endtask

  protected task renewal(int unsigned sram, bit backdoor, bit [31:0] addr);
    string kdi = $sformatf("%s[%0d]", Kdi, sram);
    string ctrl = ctrl_path(sram);
    otp_ctrl_pkg::sram_key_t key, got_key;
    otp_ctrl_pkg::sram_nonce_t nonce, got_nonce;
    bit acked, valid;
    bit [31:0] base, offset;
    // The key derivation's acknowledgement on the controller's slot.
    for (int unsigned t = 0; t < KeyTimeoutNs && !acked; t++) begin
      #1ns;
      if (rd({kdi, ".ack"}) & 1) begin
        key = rd({kdi, ".key"});
        nonce = rd({kdi, ".nonce"});
        acked = 1;
      end
    end
    `DV_CHECK_FATAL(acked, $sformatf("no key from otp_ctrl on slot %0d for %s", sram, ctrl))
    for (int unsigned t = 0; t < KeyTimeoutNs && !valid; t++) begin
      valid = rd({ctrl, KeyValid}) & 1;
      if (!valid) #1ns;
    end
    `DV_CHECK_FATAL(valid, $sformatf("%s: SCR_KEY_VALID never set", ctrl))
    got_key = rd({ctrl, ".key_q"});
    got_nonce = rd({ctrl, ".nonce_q"});
    `DV_CHECK_EQ(got_key, key, $sformatf("%s does not hold the key otp_ctrl delivered", ctrl))
    `DV_CHECK_EQ(got_nonce, nonce, $sformatf("%s does not hold the nonce otp_ctrl delivered", ctrl))
    `uvm_info(`gfn, $sformatf("EG2 scr: %s took OTP's key 0x%0h", ctrl, key), UVM_LOW)
    if (!backdoor || sram == 1) return;
    base = sram == 0 ? top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR
                     : top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR;
    offset = addr - base;
    for (int unsigned i = 0; i < Words; i++) begin
      if (sram == 0) main_sram_bkdr_write32(offset + 4 * i, bd(sram, i), key, nonce[63:0]);
      else ret_sram_bkdr_write32(offset + 4 * i, bd(sram, i), key, nonce[63:0]);
    end
    `uvm_info(`gfn, $sformatf("EG2 scr: %0d backdoor words at %s offset 0x%0h", Words, ctrl,
                              offset), UVM_LOW)
  endtask

endclass : chip_sw_eg2_sram_scrambled_access_cheriot_vseq
