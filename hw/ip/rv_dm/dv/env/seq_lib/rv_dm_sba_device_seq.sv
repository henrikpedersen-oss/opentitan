// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The device on rv_dm's system-bus port. tl_seq_item::valid_delay_c caps every response delay at
// 50 cycles, while the system-bus tests ask for longer ones (rv_dm_delayed_resp_sba_tl_access: up
// to 2000 cycles; rv_dm_bad_sba_ and _autoincr_sba_tl_access: up to 600). Whenever min_rsp_delay
// was above 50 no response could be randomized ("Cannot randomize rsp", tl_device_seq.sv:151). The
// sequence's own [min_rsp_delay:max_rsp_delay] constraint bounds the delay instead.
class rv_dm_sba_device_seq extends cip_tl_device_seq;
  `uvm_object_utils(rv_dm_sba_device_seq)
  `uvm_object_new

  virtual function void randomize_rsp(cip_tl_seq_item rsp);
    rsp.valid_delay_c.constraint_mode(0);
    super.randomize_rsp(rsp);
  endfunction
endclass
