// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// One TL-UL request with exactly the fields the sequence gives. TL-UL allows a PutPartialData mask
// with gaps; tl_seq_item's mask_contiguous_c forbids it, and the tests ported from the cms testbench
// use such masks on purpose (a store that clears a tag whatever bytes it writes).
class cheriot_mem_sys_tl_host_seq extends cip_tl_host_single_seq;
  `uvm_object_utils(cheriot_mem_sys_tl_host_seq)
  `uvm_object_new

  virtual function void randomize_req(REQ req, int idx);
    req.mask_contiguous_c.constraint_mode(0);
    super.randomize_req(req, idx);
  endfunction

endclass
