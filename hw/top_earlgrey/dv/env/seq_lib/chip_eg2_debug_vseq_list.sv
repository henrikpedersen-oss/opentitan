// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The EG2 debug area's sequences (chip_eg2_debug_tests.hjson); after chip_eg2_mem_vseq_list.sv.
`include "chip_eg2_rv_dm_base_vseq.sv"
`include "chip_eg2_rv_dm_jtag_vseq.sv"
`include "chip_eg2_rv_dm_dtm_vseq.sv"
`include "chip_eg2_rv_dm_control_status_vseq.sv"
`include "chip_eg2_rv_dm_sba_tl_errors_vseq.sv"
`include "chip_eg2_rv_dm_sba_cheriot_mode_vseq.sv"
`include "chip_eg2_rv_dm_lc_disabled_vseq.sv"
`include "chip_eg2_rv_dm_corners_base_vseq.sv"
`include "chip_eg2_rv_dm_halt_corners_vseq.sv"
`include "chip_eg2_rv_dm_abstract_errors_vseq.sv"
`include "chip_eg2_rv_dm_reset_corners_vseq.sv"
`include "chip_eg2_rv_dm_lc_drop_vseq.sv"
`include "chip_eg2_rv_dm_sba_corners_vseq.sv"
`include "chip_eg2_rv_dm_step_corners_vseq.sv"
`include "chip_eg2_rv_dm_counters_vseq.sv"
`include "chip_eg2_rv_dm_triggers_vseq.sv"
`include "chip_eg2_rv_dm_cap_base_vseq.sv"
`include "chip_eg2_rv_dm_cap_bypass_vseq.sv"
`include "chip_eg2_rv_dm_halt_revocation_vseq.sv"
`include "chip_eg2_rv_dm_priv_vseq.sv"
