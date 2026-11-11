// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One base test for feature and common CIP sequences. The testlist selects UVM_TEST_SEQ;
// feature entries set +cheriot_mem_sys_cmodel=1, read by cfg.initialize() during build.
// Common CIP entries keep their usual sequence selection and RAL checking.
class cheriot_mem_sys_base_test extends cip_base_test #(
    .CFG_T(cheriot_mem_sys_env_cfg),
    .ENV_T(cheriot_mem_sys_env)
  );
  `uvm_component_utils(cheriot_mem_sys_base_test)
  `uvm_component_new
endclass : cheriot_mem_sys_base_test
