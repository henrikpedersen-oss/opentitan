// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The EG2 CHERIoT suite's sequences (chip_eg2_cheriot_tests.hjson): the constrained-random layers
// (chip_sw_eg2_rand_vseq, chip_sw_eg2_cheriot_vseq) and one vseq per test.
`include "chip_sw_eg2_rand_vseq.sv"
`include "chip_sw_eg2_cheriot_vseq.sv"
`include "chip_sw_eg2_cheriot_memsys_vseqs.sv"
`include "chip_sw_eg2_cheriot_core_vseqs.sv"
`include "chip_sw_eg2_cheriot_revocation_vseqs.sv"
`include "chip_sw_eg2_trvk_fault_vseq.sv"
`include "chip_sw_eg2_revocation_corners_vseqs.sv"
`include "chip_sw_eg2_revbm_errors_vseq.sv"
