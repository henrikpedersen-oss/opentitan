// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The base of every cheriot_mem_sys test. A feature test (one per file) names its vseq in
// `feature_vseq` and is checked by the C reference model. Without one, the test runs the vseq
// given by +UVM_TEST_SEQ, as the common CIP tests do.
class cheriot_mem_sys_base_test extends cip_base_test #(
    .CFG_T(cheriot_mem_sys_env_cfg),
    .ENV_T(cheriot_mem_sys_env)
  );
  `uvm_component_utils(cheriot_mem_sys_base_test)

  protected string feature_vseq;

  `uvm_component_new

  virtual function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    if (feature_vseq != "") cfg.en_cmodel = 1'b1;
  endfunction

  virtual task run_seq(string test_seq_s, uvm_phase phase);
    super.run_seq((feature_vseq != "") ? feature_vseq : test_seq_s, phase);
  endtask

endclass : cheriot_mem_sys_base_test
