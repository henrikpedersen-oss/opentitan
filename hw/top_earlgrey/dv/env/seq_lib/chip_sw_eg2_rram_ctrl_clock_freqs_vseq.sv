// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_ctrl_clock_freqs (sw/device/tests/eg2_rram/eg2_rram_ctrl_clock_freqs.c; EG2 spec
// REQ_RRAM_27: the clock setting). The jittery clock at boot is drawn here and written to the OTP
// item CREATOR_SW_CFG_JITTER_EN, which the test ROM applies (as +en_jitter=1 does in
// chip_sw_base_vseq); +en_jitter=<0|1> pins it. The software toggles clkmgr's JITTER_ENABLE
// between rounds.
class chip_sw_eg2_rram_ctrl_clock_freqs_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_ctrl_clock_freqs_vseq)
  `uvm_object_new

  rand bit jitter_at_boot;
  rand int unsigned n_rounds, jitter_sw, max_words;

  constraint knobs_c {
    jitter_at_boot dist {1 := 50, 0 := 50};
    n_rounds inside {[3:12]};
    jitter_sw dist {1 := 70, 0 := 30};
    max_words dist {[4:64] :/ 40, [65:1024] :/ 60};
  }

  virtual task cpu_init();
    super.cpu_init();
    if (!$test$plusargs("en_jitter")) begin
      cfg.mem_bkdr_util_h[Otp].write32(otp_ctrl_reg_pkg::CreatorSwCfgJitterEnOffset,
                                       jitter_at_boot ? prim_mubi_pkg::MuBi4True :
                                                        prim_mubi_pkg::MuBi4False);
      `uvm_info(`gfn, $sformatf("EG2 jitter at boot = %0d (random)", jitter_at_boot), UVM_LOW)
    end
  endtask

  virtual function void write_params();
    set_param("n_rounds", n_rounds);
    set_param("jitter_sw", jitter_sw);
    set_param("max_words", max_words);
  endfunction
endclass : chip_sw_eg2_rram_ctrl_clock_freqs_vseq
