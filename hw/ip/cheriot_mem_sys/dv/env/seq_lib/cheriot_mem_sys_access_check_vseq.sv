// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The software bitmap window (revbm_tl_d) and the core's TRVK port (corerevbm_tl) reach only the
// revocation bitmap, with full-word, word-aligned Get and PutFullData; the tag regions are not
// mapped; software-reachable errors raise no alert (theory_of_operation.md, "Meta SRAM Address
// Map" and "Access Checkers"; cheriot_mem_sys.hjson MEM.SW_NOACCESS).
// Requirements: REQ_CMS_03, REQ_CMS_04; REQ_META_03–07, REQ_META_16.
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
    // Seed real tags before attempts to read/overwrite the tag words. The scoreboard compares
    // the complete meta SRAM, including these tags, at quiet points and test end.
    setup_tagged(MainSramBase, 1);
    setup_tagged(MainSramBase + 256, 1);
    for (int region = 0; region < 2; region++) begin
      bit [31:0] tag_base = region ? MetaSramTagBase : MetaNvmTagBase;
      for (int word = 0; word < 6; word++) begin
        // The register adapter decodes local offsets. 0x28..0x3c are reserved offsets;
        // choose tag-region addresses with these offsets, where refusal is documented.
        bit [31:0] tag_addr = (tag_base & ~32'h3f) + 32'h28 + 4*word;
        for (int write = 0; write < 2; write++) begin
          tlul_pkg::tl_a_op_e tag_op = write ? tlul_pkg::PutFullData : tlul_pkg::Get;
          for (int port = 0; port < 2; port++) begin
            bitmap_access(1'(port), tag_op, tag_addr, 2, '1, 4'hf, rsp);
            `DV_CHECK_EQ(rsp.d_error, 1, "bitmap port must refuse the tag region")
          end
          tl_host_access(p_sequencer.tl_sequencer_h, tag_op, tag_addr, 2, '1, 4'hf, 0, rsp);
          `DV_CHECK_EQ(rsp.d_error, 1, "reserved register offset must refuse a tag access")
        end
      end
    end
    wait_quiet();
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
