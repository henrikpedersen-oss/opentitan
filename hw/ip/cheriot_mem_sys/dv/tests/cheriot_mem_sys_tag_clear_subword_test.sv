// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Runs cheriot_mem_sys_tag_clear_subword_vseq, which describes what it checks.
class cheriot_mem_sys_tag_clear_subword_test extends cheriot_mem_sys_base_test;
  `uvm_component_utils(cheriot_mem_sys_tag_clear_subword_test)

  function new(string name, uvm_component parent);
    super.new(name, parent);
    feature_vseq = "cheriot_mem_sys_tag_clear_subword_vseq";
  endfunction

endclass
