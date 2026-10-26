// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The vseqs of the EG2 security area (chip_eg2_sec_tests.hjson; software in
// sw/device/tests/eg2_sec); after chip_eg2_boot_vseq_list.sv, whose base and SBA sequence
// (chip_sw_eg2_sec_creator_isolation_vseq) they share.

`include "chip_sw_eg2_sec_vseqs.sv"
`include "chip_sw_eg2_sec_rom_ext_e2e_vseq.sv"
