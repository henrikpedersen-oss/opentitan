// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_virtual_sequencer extends cip_base_virtual_sequencer #(
    .CFG_T(cheriot_mem_sys_env_cfg),
    .COV_T(cheriot_mem_sys_env_cov)
  );
  `uvm_component_utils(cheriot_mem_sys_virtual_sequencer)

  cheriot_mem_sys_core_sequencer core_sqr;
  tl_sequencer                   corerevbm_sqr;
  tl_sequencer                   revbm_sqr;
  tl_sequencer                   cored_h_sqr;
  tl_sequencer                   tbre_h_sqr;
  tl_sequencer                   meta_sqr;

  // The backdoor of the memory behind the crossbar (preloading, programming the NVM).
  cheriot_mem_sys_mem_backdoor   mem_backdoor;

  `uvm_component_new

endclass
