// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One access of the core: sends `req` (filled in by the caller) and waits for its response.
// `granted` rises when the driver takes the request, so a caller that issues accesses one after
// another without waiting for responses keeps them in its order.
class cheriot_mem_sys_core_access_seq extends dv_base_seq #(
    .REQ        (cheriot_mem_sys_core_item),
    .CFG_T      (cheriot_mem_sys_core_agent_cfg),
    .SEQUENCER_T(cheriot_mem_sys_core_sequencer)
  );
  `uvm_object_utils(cheriot_mem_sys_core_access_seq)

  bit granted;
  bit done;

  `uvm_object_new

  virtual task body();
    start_item(req);
    granted = 1'b1;
    finish_item(req);
    get_response(rsp, req.get_transaction_id());
    done = 1'b1;
  endtask

endclass
