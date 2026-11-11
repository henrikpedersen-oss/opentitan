// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Directed mode/region/size/opcode matrix, including external data-port errors.
// Tagged stores use whole words; partial writes carry no capability tag.
// Requirements: REQ_TF_01–13; REQ_CMS_03–06; REQ_TAG_07–08.
class cheriot_mem_sys_core_matrix_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_core_matrix_vseq)
  `uvm_object_new

  virtual task body();
    bit [31:0] addr[4] = '{MainSramBase + 32'h100, NvmBase + 32'h100,
                           UntaggedBase + 32'h100, DataErrBase};
    prim_mubi_pkg::mubi4_t modes[3] = '{prim_mubi_pkg::MuBi4True,
                                       prim_mubi_pkg::MuBi4False, prim_mubi_pkg::mubi4_t'(0)};
    tlul_pkg::tl_a_op_e ops[3] = '{tlul_pkg::Get, tlul_pkg::PutFullData,
                                  tlul_pkg::PutPartialData};
    foreach (modes[m]) begin
      wait_quiet();
      set_ena(modes[m]);
      foreach (addr[r]) begin
        foreach (ops[o]) begin
          for (int size = 0; size < 3; size++) begin
            for (int fault = 0; fault < 2; fault++) begin
              cheriot_mem_sys_core_item it;
              // Error-region accesses already fail. Plain NVM stores are refused.
              if (fault && (r == 3 || (r == 1 && o != 0))) continue;
              it = core_item(ops[o], addr[r], 2'(size), 32'ha55a_c33c);
              if (fault) m_cored_h_seq.inject(InjErr, addr[r], addr[r] + 4, InjAny);
              core_access(it);
            end
          end
        end
        // Hinted loads and tagged full stores, with and without an external error.
        for (int fault = 0; fault < 2; fault++) begin
          if (fault && r == 3) continue;
          if (fault) m_cored_h_seq.inject(InjErr, addr[r], addr[r] + 4, InjReads);
          core_access(core_item(tlul_pkg::Get, addr[r], 2, 0, 1));
          if (m == 0 && r == 1) begin
            bit e0, e1;
            nvm_program(addr[r], 32'ha55a_c33c, 32'h5aa5_3cc3);
            if (fault) m_cored_h_seq.inject(InjErr, addr[r], addr[r] + 4, InjReads);
            nvm_cap_store_wait(addr[r], 32'ha55a_c33c, 32'h5aa5_3cc3, e0, e1);
          end else begin
            if (fault && r != 1)
              m_cored_h_seq.inject(InjErr, addr[r], addr[r] + 4, InjWrites);
            core_access(core_item(tlul_pkg::PutFullData, addr[r], 2, 32'ha55a_c33c, 1));
          end
        end
      end
    end
    wait_quiet();
  endtask
endclass
