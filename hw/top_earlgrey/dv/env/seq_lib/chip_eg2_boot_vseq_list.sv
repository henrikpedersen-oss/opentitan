// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The vseqs of the EG2 secure-boot area (chip_eg2_boot_tests.hjson; software in
// sw/device/tests/eg2_boot). The base is shared with the security area (chip_eg2_sec_vseq_list.sv,
// included after this file).

`include "chip_sw_eg2_boot_sec_base_vseq.sv"
`include "chip_sw_eg2_boot_outcome_vseq.sv"
`include "chip_sw_eg2_boot_handover_protection_vseq.sv"
