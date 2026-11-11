// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Sequences on the agents
`include "cheriot_mem_sys_mem_device_seq.sv"
`include "cheriot_mem_sys_tl_host_seq.sv"

// Virtual sequences
`include "cheriot_mem_sys_base_vseq.sv"
`include "cheriot_mem_sys_common_vseq.sv"
// One per feature test
`include "cheriot_mem_sys_tag_smoke_vseq.sv"
`include "cheriot_mem_sys_tag_store_load_vseq.sv"
`include "cheriot_mem_sys_tag_clear_subword_vseq.sv"
`include "cheriot_mem_sys_cap_load_hint_vseq.sv"
`include "cheriot_mem_sys_access_check_vseq.sv"
`include "cheriot_mem_sys_mode_gating_vseq.sv"
`include "cheriot_mem_sys_rmw_same_word_vseq.sv"
`include "cheriot_mem_sys_rmw_core_tbre_same_word_vseq.sv"
`include "cheriot_mem_sys_nvm_cap_store_vseq.sv"
`include "cheriot_mem_sys_tbre_sweep_vseq.sv"
`include "cheriot_mem_sys_tbre_base_decode_vseq.sv"
`include "cheriot_mem_sys_tbre_csr_vseq.sv"
`include "cheriot_mem_sys_tbre_concurrent_vseq.sv"
`include "cheriot_mem_sys_tbre_snoop_window_vseq.sv"
`include "cheriot_mem_sys_tbre_epoch_vseq.sv"
`include "cheriot_mem_sys_tbre_intr_vseq.sv"
`include "cheriot_mem_sys_alert_test_reg_vseq.sv"
`include "cheriot_mem_sys_err_csr_intg_vseq.sv"
`include "cheriot_mem_sys_err_data_path_vseq.sv"
`include "cheriot_mem_sys_err_meta_intg_vseq.sv"
`include "cheriot_mem_sys_err_tag_path_vseq.sv"
`include "cheriot_mem_sys_err_tbre_read_vseq.sv"
`include "cheriot_mem_sys_err_tbre_revbm_vseq.sv"
`include "cheriot_mem_sys_mixed_random_vseq.sv"
`include "cheriot_mem_sys_cmodel_random_vseq.sv"
`include "cheriot_mem_sys_reset_vseq.sv"
`include "cheriot_mem_sys_tbre_store_race_vseq.sv"
`include "cheriot_mem_sys_latency_vseq.sv"
`include "cheriot_mem_sys_trvk_backpressure_vseq.sv"
`include "cheriot_mem_sys_trvk_fault_vseq.sv"
`include "cheriot_mem_sys_err_cmd_intg_vseq.sv"
