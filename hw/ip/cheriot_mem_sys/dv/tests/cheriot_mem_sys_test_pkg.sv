// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

package cheriot_mem_sys_test_pkg;
  // dep packages
  import uvm_pkg::*;
  import cip_base_pkg::*;
  import cheriot_mem_sys_env_pkg::*;

  // macro includes
  `include "uvm_macros.svh"
  `include "dv_macros.svh"

  // package sources
  `include "cheriot_mem_sys_base_test.sv"
  // The feature tests, one per file
  `include "cheriot_mem_sys_access_check_test.sv"
  `include "cheriot_mem_sys_alert_test_reg_test.sv"
  `include "cheriot_mem_sys_cap_load_hint_test.sv"
  `include "cheriot_mem_sys_cmodel_random_test.sv"
  `include "cheriot_mem_sys_err_csr_intg_test.sv"
  `include "cheriot_mem_sys_err_data_path_test.sv"
  `include "cheriot_mem_sys_err_meta_intg_test.sv"
  `include "cheriot_mem_sys_err_tag_path_test.sv"
  `include "cheriot_mem_sys_err_tbre_read_test.sv"
  `include "cheriot_mem_sys_err_tbre_revbm_test.sv"
  `include "cheriot_mem_sys_mixed_random_test.sv"
  `include "cheriot_mem_sys_mode_gating_test.sv"
  `include "cheriot_mem_sys_nvm_cap_store_test.sv"
  `include "cheriot_mem_sys_rmw_core_tbre_same_word_test.sv"
  `include "cheriot_mem_sys_rmw_same_word_test.sv"
  `include "cheriot_mem_sys_tag_clear_subword_test.sv"
  `include "cheriot_mem_sys_tag_smoke_test.sv"
  `include "cheriot_mem_sys_tag_store_load_test.sv"
  `include "cheriot_mem_sys_tbre_base_decode_test.sv"
  `include "cheriot_mem_sys_tbre_concurrent_test.sv"
  `include "cheriot_mem_sys_tbre_csr_test.sv"
  `include "cheriot_mem_sys_tbre_epoch_test.sv"
  `include "cheriot_mem_sys_tbre_intr_test.sv"
  `include "cheriot_mem_sys_tbre_snoop_window_test.sv"
  `include "cheriot_mem_sys_tbre_sweep_test.sv"

endpackage
