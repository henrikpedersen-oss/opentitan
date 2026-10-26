// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The stub CPU's TL sequences hold every request until it is accepted, like the Ibex LSU they
// stand in for (data_req_o stays high until data_gnt_i). cheriot_mem_sys relies on that
// (CoredReqHeld_M, ticket F26 in eg2_top_verification status/rtl_tickets.md). cip_lib's TL error
// tasks (cip_base_vseq__tl_errors.svh) give each request a random chance to be retracted
// (req_abort_pct); chip_env overrides their tl_host_custom_seq with this one, which clears the
// retraction after randomization. OpenTitan's library and the block-level benches are unchanged.
class eg2_tl_hold_custom_seq extends tl_host_custom_seq #(cip_tl_seq_item);
  `uvm_object_utils(eg2_tl_hold_custom_seq)
  `uvm_object_new

  virtual function void post_randomize_req(REQ req, int idx);
    super.post_randomize_req(req, idx);
    req.req_abort_after_a_valid_len = 1'b0;
  endfunction
endclass

// The same for tl_protocol_err's sequence (cip_base_vseq__tl_errors.svh tl_protocol_err), which
// is not a tl_host_custom_seq but inherits the same random retraction from tl_host_seq.
class eg2_tl_hold_protocol_err_seq extends tl_host_protocol_err_seq #(cip_tl_seq_item);
  `uvm_object_utils(eg2_tl_hold_protocol_err_seq)
  `uvm_object_new

  virtual function void post_randomize_req(REQ req, int idx);
    super.post_randomize_req(req, idx);
    req.req_abort_after_a_valid_len = 1'b0;
  endfunction
endclass

// And for cip_base_vseq's tl_access_w_abort (cip_tl_host_single_seq), which the memory
// partial-access task calls with a random req_abort_pct.
class eg2_tl_hold_cip_single_seq extends cip_tl_host_single_seq;
  `uvm_object_utils(eg2_tl_hold_cip_single_seq)
  `uvm_object_new

  virtual function void post_randomize_req(REQ req, int idx);
    super.post_randomize_req(req, idx);
    req.req_abort_after_a_valid_len = 1'b0;
  endfunction
endclass
