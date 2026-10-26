// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The EG2 RRAM area's sequences (chip_eg2_rram_tests.hjson); after chip_eg2_mem_vseq_list.sv.
`include "chip_eg2_rram_jtag_lc_gate_vseq.sv"
`include "chip_sw_eg2_rram_crash_alert_vseq.sv"
`include "chip_sw_eg2_rram_ctrl_clock_freqs_vseq.sv"
`include "chip_sw_eg2_rram_ctrl_ops_vseq.sv"
`include "chip_sw_eg2_rram_ecc_corr_rewrite_vseq.sv"
`include "chip_sw_eg2_rram_exec_en_vseq.sv"
`include "chip_sw_eg2_rram_fetch_during_write_vseq.sv"
`include "chip_sw_eg2_rram_fetch_perf_vseq.sv"
`include "chip_sw_eg2_rram_host_exec_vseq.sv"
`include "chip_sw_eg2_rram_host_intg_vseq.sv"
`include "chip_sw_eg2_rram_host_wr_reject_vseq.sv"
`include "chip_sw_eg2_rram_idle_low_power_vseq.sv"
`include "chip_sw_eg2_rram_init_vseq.sv"
`include "chip_sw_eg2_rram_mem_protection_vseq.sv"
`include "chip_sw_eg2_rram_otp_isolation_vseq.sv"
`include "chip_sw_eg2_rram_otp_layout_vseq.sv"
`include "chip_sw_eg2_rram_otp_rsw_vseq.sv"
`include "chip_sw_eg2_rram_prim_tl_access_vseq.sv"
`include "chip_sw_eg2_rram_scramble_vseq.sv"
`include "chip_sw_eg2_rram_uncorr_fetch_vseq.sv"
`include "chip_sw_eg2_rram_write_then_fetch_vseq.sv"
`include "chip_sw_eg2_rram_sec_cm_vseq.sv"
