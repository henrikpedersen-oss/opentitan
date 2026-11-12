// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A fault on one of the revocation engine's reads fails the sweep: the sweep still runs to its
// end, sets TBRE_STATUS.sweep_err and is not counted in TBRE_EPOCH; the capability whose read
// failed is not cleared, the others are resolved as usual. An error response (the read was
// denied) raises no alert; an integrity fault, or an error on the engine's tag lookup, raises
// fatal_fault (theory_of_operation.md, "Revocation Engine" and "Error Handling";
// programmers_guide.md, "Errors").
//   case 0, 1: an error response to the upper or the lower word of a capability;
//   case 2, 3: a response or data integrity fault;
//   case 4:    an error response to the engine's tag lookup;
//   case 5:    a sweep over the top of the NVM, whose upper half denies every read.
// Requirements: REQ_TBRE_32–36; REQ_ALT_04.
class cheriot_mem_sys_err_tbre_read_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_tbre_read_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   b, a, cb, w0, w1;
    int unsigned k, n;
    cheriot_on();
    for (int unsigned c = 0; c < 6; c++) begin
      b = (c == 5) ? NvmTop - 8 * 8 : MainSramBase + 32'h800;
      n = (c == 5) ? 8 : 4;
      for (int unsigned i = 0; i < n; i++) begin
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        store_cap(b + 8 * i, w0, w1, 1'b1);
      end
      wait_quiet();
      k = 1 + $urandom_range(2);
      a = b + 8 * k + ((c == 1) ? 0 : 4);
      case (c)
        0, 1:    m_tbre_h_seq.inject(InjErr, a, a + 4, InjReads);
        2:       m_tbre_h_seq.inject(InjRspIntg, a, a + 4, InjReads);
        3:       m_tbre_h_seq.inject(InjDataIntg, a, a + 4, InjReads);
        4:       m_meta_seq.inject(InjErr, tag_word_addr(b + 8 * k), tag_word_addr(b + 8 * k) + 4,
                                   InjReads);
        default: m_tbre_h_seq.inject(InjErr, NvmTop - 8 * 4, NvmTop, InjReads, 8);
      endcase
      tbre_sweep(b, n, 1'b0);
      tbre_wait_idle();
      clear_sweep_err();
      wait_quiet();
      do_reset();
    end
  endtask

endclass
