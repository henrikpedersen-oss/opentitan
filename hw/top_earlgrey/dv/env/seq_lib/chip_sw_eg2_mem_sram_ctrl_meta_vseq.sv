// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_sram_ctrl_meta (sw/device/tests/eg2_mem/eg2_sram_ctrl_meta.c; EG2 spec REQ_CMS_14,
// REQ_OTP_02, REQ_SEC_18). The knobs under the spec's weights, and the bench's half of the test,
// at the software's DV log lines:
//   EG2 MEM meta written <seed> <first> <n>  the meta RAM words first..first+n-1 hold ciphertext:
//                                            at most n/8 of them equal the software's pattern
//   EG2 MEM meta snap <w> / same <w>         RISC-V mode: the RAM word w unchanged by the refused
//                                            accesses
//   EG2 MEM meta init begin / done           every sampled RAM word written (no X) by INIT
//   EG2 MEM meta renew begin / done          the controller's key changed (it took a new key)
//   EG2 MEM meta escalate / escalated        lc_escalate_en forced at sram_ctrl_meta (internal
//                                            copy, so no other receiver sees it), then released,
//                                            boot_phase = 1 and a power-on reset: the second boot
//                                            checks the clean controller
// The meta RAM has no address scrambling (NumAddrScrRounds = 0, top_earlgrey.hjson): RAM word w
// is bitmap word w.
class chip_sw_eg2_sram_ctrl_meta_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sram_ctrl_meta_vseq)

  `uvm_object_new

  localparam string MetaHier = "tb.dut.top_earlgrey.earlgrey_pd_main.u_sram_ctrl_meta";
  localparam string MetaMem = {MetaHier, ".u_prim_ram_1p_scr.u_prim_ram_1p_adv.gen_ram_inst[0]",
                               ".u_mem.mem"};
  localparam string MetaKey = {MetaHier, ".key_q"};
  localparam string MetaEsc = {MetaHier, ".escalate_en"};
  localparam int unsigned MetaWords = 38912 / 4;

  rand int unsigned n_ops;
  rand int unsigned w_op_access, w_op_init, w_op_renew, w_op_escalate, w_op_readback;
  rand int unsigned n_words;

  constraint n_ops_c {
    n_ops inside {[2:12]};
  }
  // The spec's default weights (40 : 15 : 15 : 10 : 20), +-50 % per seed.
  constraint w_op_c {
    w_op_access inside {[20:60]};
    w_op_init inside {[8:22]};
    w_op_renew inside {[8:22]};
    w_op_escalate inside {[5:15]};
    w_op_readback inside {[10:30]};
  }
  constraint n_words_c {
    n_words dist {[16:64] :/ 40, [65:256] :/ 40, [257:1024] :/ 20};
  }

  virtual function void write_params();
    set_param("n_ops", n_ops);
    set_param("w_op_access", w_op_access);
    set_param("w_op_init", w_op_init);
    set_param("w_op_renew", w_op_renew);
    set_param("w_op_escalate", w_op_escalate);
    set_param("w_op_readback", w_op_readback);
    set_param("n_words", n_words);
    set_param("boot_phase", 0);
  endfunction

  protected function bit [38:0] raw(int unsigned w);
    uvm_hdl_data_t v;
    if (!(uvm_hdl_read($sformatf("%s[%0d]", MetaMem, w), v))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read($sformatf(\"%%s[%%0d]\", MetaMem, w), v)) %s", $sformatf("cannot read %s[%0d]", MetaMem, w)))
    return v[38:0];
  endfunction

  protected function bit [127:0] key();
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(MetaKey, v), $sformatf("cannot read %s", MetaKey))
    return v[127:0];
  endfunction

  // eg2_sram_ctrl_meta.c pattern().
  protected function bit [31:0] pattern(bit [31:0] seed, bit [31:0] i);
    bit [31:0] x = seed ^ (i * 32'h9e3779b9);
    x ^= x >> 16;
    x *= 32'h7feb352d;
    x ^= x >> 15;
    return x;
  endfunction

  virtual task body();
    `DV_CHECK_FATAL(uvm_hdl_check_path(MetaMem), {"no meta RAM at ", MetaMem})
    `DV_CHECK_FATAL(uvm_hdl_check_path(MetaKey), {"no key register at ", MetaKey})
    `DV_CHECK_FATAL(uvm_hdl_check_path(MetaEsc), {"no escalation signal at ", MetaEsc})
    super.body();
    fork
      check_lines();
    join_none
  endtask

  protected task check_lines();
    bit [38:0]  snap[int unsigned];
    bit [127:0] key_before;
    forever begin
      string line;
      bit [31:0] seed;
      int unsigned first, n, w, equal;
      wait_sw_line("EG2 MEM meta ", line, 32'd4_000_000_000);
      if ($sscanf(line, "EG2 MEM meta written 0x%h %d %d", seed, first, n) == 3) begin
        equal = 0;
        for (int unsigned i = 0; i < n; i++) begin
          bit [38:0] r = raw(first + i);
          if (r[31:0] == pattern(seed, i)) equal++;
        end
        `DV_CHECK_LE(equal, n / 8, $sformatf(
                     "meta RAM holds plaintext: %0d of %0d words equal the written data", equal, n))
        `uvm_info(`gfn, $sformatf("EG2 meta: %0d words ciphertext at rest", n), UVM_LOW)
      end else if ($sscanf(line, "EG2 MEM meta snap %d", w) == 1) begin
        snap[w] = raw(w);
      end else if ($sscanf(line, "EG2 MEM meta same %d", w) == 1) begin
        `DV_CHECK_EQ(raw(w), snap[w], $sformatf("meta RAM word %0d changed by a refused access", w))
      end else if (line == "EG2 MEM meta init done") begin
        for (int i = 0; i < 64; i++) begin
          int unsigned r = $urandom_range(0, MetaWords - 1);
          `DV_CHECK(!$isunknown(raw(r)), $sformatf("meta RAM word %0d not written by INIT", r))
        end
      end else if (line == "EG2 MEM meta renew begin") begin
        key_before = key();
      end else if (line == "EG2 MEM meta renew done") begin
        `DV_CHECK_NE(key(), key_before, "sram_ctrl_meta kept its old key after the renewal")
      end else if (line == "EG2 MEM meta escalate") begin
        lc_ctrl_pkg::lc_tx_t [1:0] on = {lc_ctrl_pkg::On, lc_ctrl_pkg::On};
        announce("chip_sw_eg2_sram_ctrl_meta_vseq", "sram_ctrl_meta", "lc_escalate_en forced On");
        `DV_CHECK_FATAL(uvm_hdl_force(MetaEsc, on))
      end else if (line == "EG2 MEM meta escalated") begin
        cfg.clk_rst_vif.wait_clks(100);
        `DV_CHECK_FATAL(uvm_hdl_release(MetaEsc))
        set_param("boot_phase", 1);
        `uvm_info(`gfn, "EG2 meta: escalated; power-on reset into the second boot", UVM_LOW)
        apply_reset();
      end
      // The next line, not this one again.
      @(cfg.sw_logger_vif.printed_log_event);
    end
  endtask

endclass : chip_sw_eg2_sram_ctrl_meta_vseq
