// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The vseqs of the EG2 peripheral area (chip_eg2_periph_tests.hjson). Compiled with EG2_PERIPH_SB
// only (build mode en_eg2_periph): its tests run in that build alone.

`ifdef EG2_PERIPH_SB
`include "chip_sw_eg2_pbf_periph_vseq.sv"
`endif
