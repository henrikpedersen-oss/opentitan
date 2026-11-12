// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// A sweep of TBRE_NUM_CAPS capabilities from TBRE_BASE_ADDR, in the main SRAM or the NVM, clears
// the tag of exactly the tagged, non-sealing capabilities whose base has its revocation bit set,
// and nothing else; it never writes memory (theory_of_operation.md, "Revocation Engine";
// programmers_guide.md, "Revoking Capabilities"). Whole regions, random sub-ranges and single
// capabilities at both ends. An isolated sweep checks that completion waits for
// external read/write responses, rather than merely checking memory after quiet.
// Requirements: REQ_TBRE_13–21, REQ_TBRE_37–38.
class cheriot_mem_sys_tbre_sweep_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_tbre_sweep_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0]   b;
    int unsigned n;
    cheriot_on();
    isolated_sweep_completion(0);
    randomize_timing(0);

    // The main SRAM
    fill_caps(MainSramBase, SramCaps);
    tbre_sweep(MainSramBase, SramCaps);
    wait_quiet();
    for (int unsigned i = 0; i < 6; i++) begin
      b = rand_granule(MainSramBase, MainSramTop);
      n = 1 + $urandom_range((MainSramTop - b) / 8 - 1);
      fill_caps(b, n);
      tbre_sweep(b, n);
      wait_quiet();
    end
    sweep_one(MainSramBase);
    sweep_one(MainSramTop - 8);
    wait_quiet();
    load_all_caps(MainSramBase, 128);

    // The NVM
    fill_caps(NvmBase, NvmCaps);
    tbre_sweep(NvmBase, NvmCaps);
    wait_quiet();
    b = rand_granule(NvmBase, NvmTop);
    n = 1 + $urandom_range((NvmTop - b) / 8 - 1);
    fill_caps(b, n);
    tbre_sweep(b, n);
    sweep_one(NvmBase);
    sweep_one(NvmTop - 8);
    wait_quiet();
    load_all_caps(NvmBase, 64);
  endtask

  protected task sweep_one(bit [31:0] addr);
    fill_caps(addr, 1);
    tbre_sweep(addr, 1);
  endtask

endclass
