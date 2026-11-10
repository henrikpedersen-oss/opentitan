// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The software bitmap window (revbm_tl_d) and the core's TRVK port (corerevbm_tl) reach only the
// revocation bitmap, with full-word, word-aligned Get and PutFullData; the tag regions are not
// mapped; software-reachable errors raise no alert (theory_of_operation.md, "Meta SRAM Address
// Map" and "Access Checkers"; cheriot_mem_sys.hjson MEM.SW_NOACCESS).
class cheriot_mem_sys_access_check_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_access_check_vseq)
  `uvm_object_new

  virtual task body();
    tlul_pkg::tl_a_op_e op;
    tl_seq_item         rsp;
    bit [31:0]          a;
    bit [3:0]           mask;
    bit [1:0]           size;
    cheriot_on();
    randomize_timing(0);
    for (int unsigned i = 0; i < 600; i++) begin
      // Mostly CHERIoT mode; now and then another encoding
      if ($urandom_range(15) == 0) begin
        set_ena(prim_mubi_pkg::mubi4_t'($urandom_range(15)));
      end else if (cfg.m_core_agent_cfg.vif.cheriot_ena != prim_mubi_pkg::MuBi4True) begin
        cheriot_on();
      end
      size = $urandom_range(2);
      case ($urandom_range(5))
        0:       a = MetaBase - 32'h40 + $urandom_range(32'h3f);
        1:       a = MetaNvmTagBase + $urandom_range(MetaTop - MetaNvmTagBase + 32'h3f);
        2:       a = MetaNvmTagBase - 4;
        default: a = MetaRevbmBase + $urandom_range(RevbmBytes - 1);
      endcase
      a    = (size == 2) ? {a[31:2], 2'b00} : (size == 1) ? {a[31:1], 1'b0} : a;
      mask = cheriot_mem_sys_core_item::lanes(a, size);
      case ($urandom_range(3))
        0:       op = tlul_pkg::PutFullData;
        1: begin
          // Some of the lanes
          op   = tlul_pkg::PutPartialData;
          mask = mask & 4'($urandom_range(15, 1));
          if (mask == 0) mask = cheriot_mem_sys_core_item::lanes(a, size);
        end
        default: op = tlul_pkg::Get;
      endcase
      bitmap_access(rbit(), op, a, size, $urandom, mask, rsp);
    end
    cheriot_on();
    wait_quiet();
  endtask

endclass
