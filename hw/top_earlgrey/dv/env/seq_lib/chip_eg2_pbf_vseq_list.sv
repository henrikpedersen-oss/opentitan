// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The vseqs of the EG2 post-boot feature area (chip_eg2_pbf_tests.hjson; software in
// sw/device/tests/eg2_pbf). The peripheral pair is in chip_eg2_periph_vseq_list.sv; the debug pair
// (chip_sw_eg2_pbf_debug_vseq.sv, REQ_PBF_20) belongs to the debug area and is not included here.

`include "chip_sw_eg2_pbf_base_vseq.sv"
`include "chip_sw_eg2_pbf_memory_vseq.sv"
`include "chip_sw_eg2_pbf_sym_crypto_vseq.sv"
`include "chip_sw_eg2_pbf_otbn_vseq.sv"
`include "chip_sw_eg2_pbf_entropy_vseq.sv"
`include "chip_sw_eg2_pbf_keymgr_vseq.sv"
`include "chip_sw_eg2_pbf_lc_otp_vseq.sv"
`include "chip_sw_eg2_pbf_alert_vseq.sv"
`include "chip_sw_eg2_pbf_power_vseq.sv"
`include "chip_sw_eg2_pbf_sensor_vseq.sv"
`include "chip_sw_eg2_pbf_meta_zeroed_vseq.sv"
`include "chip_sw_eg2_pbf_rom_ext_vseq.sv"
`include "chip_sw_eg2_pbf_cheriot_vseq.sv"
