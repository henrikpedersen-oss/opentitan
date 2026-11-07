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
// chip_rv_dm_perform_debug, chip_rv_dm_perform_debug_cheriot (REQ_CDM_08).
`include "chip_eg2_rv_dm_perform_debug_vseq.sv"
// chip_eg2_rv_dm_code_patch, chip_eg2_rv_dm_lockstep (REQ_CDM_08, REQ_CIB_03).
`include "chip_eg2_rv_dm_code_patch_vseq.sv"
`include "chip_eg2_rv_dm_lockstep_vseq.sv"
// chip_sw_rv_dm_cheriot_mode_switch, chip_eg2_rv_dm_reg_tags, chip_rv_dm_cheriot_progbuf_cap
// (REQ_CDM_04, REQ_CDM_08, REQ_CDM_09).
`include "chip_eg2_rv_dm_cdm_base_vseq.sv"
`include "chip_sw_rv_dm_cheriot_mode_switch_vseq.sv"
`include "chip_eg2_rv_dm_reg_tags_vseq.sv"
`include "chip_rv_dm_cheriot_progbuf_cap_vseq.sv"
