// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The vseqs of the EG2 random-number tests (chip_eg2_rng_tests.hjson; software in
// sw/device/tests/eg2_rng). Compiled with EG2_RNG only (build mode en_eg2_rng), where the bench's
// noise source and its DPI exist.

`ifdef EG2_RNG
`include "chip_sw_eg2_rng_vseq.sv"
`endif
