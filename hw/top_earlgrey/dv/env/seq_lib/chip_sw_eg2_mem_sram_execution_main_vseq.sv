// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_sram_ctrl_execution_main_cheriot (sw/device/tests/eg2_mem/eg2_sram_execution_main.c; EG2
// spec REQ_SRAM_01, REQ_TL_05), CHERIoT mode. The bench draws the test's knobs and OTP's
// HW_CFG1.EN_SRAM_IFETCH, written by backdoor before reset as chip_sw_sram_ctrl_execution_main_vseq
// does (sram_ctrl theory_of_operation.md "Code Execution from SRAM": with MuBi8True EXEC decides,
// otherwise the life cycle's HW_DEBUG_EN, On in the default RMA state). The software checks every
// outcome itself; the knobs' spec distributions: EXEC on and off uniform, the PCC class
// 55 : 15 : 15 : 15 (covering, ending one instruction early, starting one late, untagged).
class chip_sw_eg2_mem_sram_execution_main_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_mem_sram_execution_main_vseq)

  `uvm_object_new

  rand bit          otp_ifetch;
  rand int unsigned n_calls;
  rand int unsigned n_body;

  constraint otp_ifetch_c {
    otp_ifetch dist {1 := 80, 0 := 20};
  }

  constraint knobs_c {
    n_calls inside {[8:64]};
    n_body inside {[1:32]};
  }

  virtual function void initialize_otp_lc_state();
    super.initialize_otp_lc_state();
    otp_write_hw_cfg1_partition(
        .mem_bkdr_util_h(cfg.mem_bkdr_util_h[Otp]),
        .en_sram_ifetch(otp_ifetch ? prim_mubi_pkg::MuBi8True : prim_mubi_pkg::MuBi8False),
        // Kept as the default RMA image has it (img_rma.24.vmem: 0x96, MuBi8True).
        .dis_rv_dm_late_debug(prim_mubi_pkg::MuBi8True));
    `uvm_info(`gfn, $sformatf("EG2 MEM sram execution: OTP EN_SRAM_IFETCH %s",
                              otp_ifetch ? "MuBi8True" : "MuBi8False"), UVM_LOW)
  endfunction

  virtual function void write_params();
    set_param("n_calls", n_calls);
    set_param("n_body", n_body);
    set_param("w_exec_on", 50);
    set_param("w_exec_off", 50);
    set_param("w_pcc_cover", 55);
    set_param("w_pcc_end_early", 15);
    set_param("w_pcc_start_late", 15);
    set_param("w_pcc_untagged", 15);
    set_param("otp_ifetch", otp_ifetch);
  endfunction

endclass : chip_sw_eg2_mem_sram_execution_main_vseq
