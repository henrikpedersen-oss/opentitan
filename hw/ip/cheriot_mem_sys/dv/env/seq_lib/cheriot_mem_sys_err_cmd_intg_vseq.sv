// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Corrupt command or data integrity on W0 and W1 of a core capability store to NVM, with the
// existing tag both clear and set. The documented WTRC contract requires d_error, unchanged tag
// and latched fatal_fault (theory_of_operation.md, WTRC and Error Handling).
// Ordinary core requests and both bitmap ports lack a documented command-integrity response:
// CMS-CMD-INTG asks for it; this sequence deliberately makes no prediction for those cases.
// Requirements: REQ_WTRC_10–12; REQ_CMS_18 (block part); BUS.INTEGRITY (LOGIC.SHADOW gap remains).
class cheriot_mem_sys_err_cmd_intg_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_err_cmd_intg_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] w0, w1;
    bit tag, err;
    cheriot_on();
    for (int old_tag = 0; old_tag < 2; old_tag++) begin
      for (int data_fault = 0; data_fault < 2; data_fault++) begin
        for (int upper = 0; upper < 2; upper++) begin
          bit [31:0] addr = NvmBase + 8 * (4*old_tag + 2*data_fault + upper);
          cheriot_mem_sys_core_item lo, hi;
          `uvm_info("CMS_CMD_INTG_CASE", $sformatf("old_tag=%0d kind=%s word=%0d",
                    old_tag, data_fault ? "data" : "command", upper), UVM_LOW)
          make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
          nvm_program(addr, w0, w1);
          if (old_tag) core_store_cap(addr, w0, w1, 1);
          else core_store(addr, 2, w0, err);  // refused plain store clears its tag
          wait_quiet();
          lo = core_item(tlul_pkg::PutFullData, addr, 2, w0, 1);
          hi = core_item(tlul_pkg::PutFullData, addr+4, 2, w1, 1);
          if (upper) begin
            hi.bad_cmd_intg = !data_fault;
            hi.bad_data_intg = data_fault;
          end else begin
            lo.bad_cmd_intg = !data_fault;
            lo.bad_data_intg = data_fault;
          end
          wtrc_assertions(0);
          core_access(lo);
          core_access(hi);
          wtrc_assertions(1);
          `DV_CHECK_EQ(hi.err, 1, "bad-integrity capability store must fail")
          if (!upper) `DV_CHECK_EQ(lo.err, 1, "bad-integrity W0 must fail")
          core_load_cap(addr, tag);
          `DV_CHECK_EQ(tag, 1'(old_tag), "bad integrity must preserve the NVM tag")
          wait_cycles(600);
          do_reset();
        end
      end
    end
  endtask
endclass
