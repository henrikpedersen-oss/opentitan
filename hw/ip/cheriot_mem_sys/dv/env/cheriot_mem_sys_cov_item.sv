// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Coverage-only snapshots of independent model state and external monitor transactions.
// No expected response, ownership or check reads these items or their consumers.
class cheriot_mem_sys_cov_item extends uvm_object;
  `uvm_object_utils(cheriot_mem_sys_cov_item)
  `uvm_object_new
  typedef enum {SweepCap, TbreStart, TbreEnd, SweepSnoop, NvmCap, MetaAccess, SweepRace} kind_e;
  kind_e kind;
  bit [31:0] addr;
  logic [1:0] s_tag;
  logic       s_sealing;
  logic [1:0] s_base_class;  // 0 in bitmap range, 1 below the heap, 2 above the bitmap
  logic [1:0] s_bit;
  logic [1:0] s_revoked;
  int         s_exp;
  logic       s_corr;        // a[E+8:E] < B: the base's block is below the address's
  logic [2:0] st_outcome;  // 0 accepted, 1 num 0, 2 base below the SRAM, 3 base above it and
  logic       st_clamped;
  logic       st_nvm;      // accepted sweep of the NVM
  logic [1:0] e_failed;     // a fault the test injected ended it with TBRE_STATUS.sweep_err
  logic e_denied;     // ... and that fault was only an error response to a read of the swept
  logic e_nvm;
  logic e_intr_en;    // INTR_ENABLE.tbre_done set as it ended
  logic [1:0] w_window;  // 0 before the lower word's read could be presented, 1 around it
  logic       w_upper;   // the write is to the upper word
  logic [1:0] w_rev;     // it stores a tagged capability the sweep would revoke
  logic       w_nvm;
  logic       n_upper;
  logic [1:0] n_outcome;  // 0 matched, 1 data differs, 2 not a full-word PutFullData,
  logic [1:0] n_old_tag;  // the tag before the store (as predicted)
  logic       a_port;      // 0 revbm, 1 corerevbm
  logic [2:0] a_outcome;   // 0 allowed, 1 not CHERIoT, 2 below, 3 tag region / above, 4 misaligned,
  logic       a_write;
  logic [1:0] r_kind;  // 0 outside the sweep range, 1 in range other meta word, 2 same meta word,
endclass
