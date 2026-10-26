// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The EG2 memory area's sequences (chip_eg2_mem_tests.hjson), and the base class the memory,
// RRAM, debug and bus areas share: chip_vseq_list.sv includes this list before theirs.
`include "chip_sw_eg2_chk_base_vseq.sv"
`include "chip_sw_eg2_mem_sram_ctrl_meta_vseq.sv"
`include "chip_sw_eg2_mem_otp_ctrl_dai_ecc_error_vseq.sv"
`include "chip_sw_eg2_mem_keymgr_dpe_sideload_kmac_error_vseq.sv"
`include "chip_sw_eg2_mem_usb_suspend_vseq.sv"
`include "chip_sw_eg2_mem_knobs_vseq.sv"
`include "chip_sw_eg2_mem_sram_execution_main_vseq.sv"
`include "chip_sw_eg2_mem_sram_ctrl_meta_lc_escalation_vseq.sv"
`include "chip_sw_eg2_mem_sram_scrambled_access_cheriot_vseq.sv"
