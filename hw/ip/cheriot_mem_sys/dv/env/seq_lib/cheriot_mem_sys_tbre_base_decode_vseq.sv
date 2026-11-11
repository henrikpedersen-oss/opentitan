// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The revocation decision is right for every exponent (0-14, and 24 of encoding 15), for both
// base corrections (the address in the base's 2^(E+9) block or the next), for bits 0 and 31 of a
// bitmap word, for the last heap granule, and for the first base past the bitmap (out of range:
// never revoked) (theory_of_operation.md, "Revocation Engine"; the CHERIoT ISA's bounds encoding).
// Requirements: REQ_TBRE_13–21, REQ_TBRE_41; REQ_CMS_08 (block part).
class cheriot_mem_sys_tbre_base_decode_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_base_decode_vseq)
  `uvm_object_new

  // The capabilities are stored from here, in the upper half of the SRAM.
  localparam bit [31:0] Slots = MainSramBase + SramBytes / 2;
  protected int unsigned n_slots;

  virtual task body();
    int unsigned exps[16] = '{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 24};
    bit [31:0]   base;
    cheriot_on();
    foreach (exps[i]) begin
      for (int unsigned v = 0; v < 4; v++) begin
        if (exps[i] >= 14) base = MainSramBase;
        else base = MainSramBase + (32'($urandom_range(((SramBytes / 2) >> exps[i]) - 1)) << exps[i]);
        case (v)
          0: revbm_set_base(base, 1);  // its own bit set
          1: begin                     // only the next granule's bit set
            revbm_set_base(base, 0);
            revbm_set_base(base + 8, 1);
          end
          2: begin                     // only the previous granule's bit set
            revbm_set_base(base, 0);
            if (base > MainSramBase) revbm_set_base(base - 8, 1);
          end
          default: revbm_set_base(base, rbit());
        endcase
        store_slot(base, exps[i]);
        throttle();
      end
    end
    // Bits 0 and 31 of bitmap words
    for (int unsigned w = 0; w < 4; w++) begin
      base = MainSramBase + 256 * $urandom_range(RevbmBytes / 4 - 1) + ((w % 2) ? 31 * 8 : 0);
      revbm_set_base(base, w < 2);
      store_slot(base, 0);
    end
    // The last heap granule, and the first base past the bitmap
    revbm_set_base(MainSramTop - 8, 1);
    store_slot(MainSramTop - 8, 0);
    store_slot(MainSramTop, 0);
    wait_quiet();
    tbre_sweep(Slots, n_slots);
    wait_quiet();
    load_all_caps(Slots, n_slots);
  endtask

  // A capability with base `base` and exponent `e`, in the next free slot.
  protected task store_slot(bit [31:0] base, int unsigned e);
    bit [31:0] w0, w1;
    make_cap(base, e, PermsMemRw, w0, w1);
    core_store_cap(Slots + 8 * n_slots, w0, w1, 1'b1);
    n_slots++;
  endtask

endclass
