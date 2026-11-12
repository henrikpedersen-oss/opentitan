// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

class cheriot_mem_sys_common_vseq extends cheriot_mem_sys_base_vseq;
  `uvm_object_utils(cheriot_mem_sys_common_vseq)

  constraint num_trans_c {
    num_trans inside {[1:2]};
  }
  `uvm_object_new

  // Fault injection is deliberately internal; its verdict remains the documented
  // external TL error and fatal alert. Never let an empty proxy list pass this test.
  virtual task pre_run_sec_cm_fi_vseq();
    int reg_we_proxies = 0;
    foreach (sec_cm_pkg::sec_cm_if_proxy_q[i]) begin
      if (!sec_cm_pkg::sec_cm_if_proxy_q[i].fi_disabled &&
          !uvm_re_match("*u_prim_reg_we_check*", sec_cm_pkg::sec_cm_if_proxy_q[i].path))
        reg_we_proxies++;
    end
    `DV_CHECK_EQ_FATAL(reg_we_proxies, 1, "register write-enable injection proxy")
    `uvm_info(`gfn, "CMS_SEC_CM: register write-enable fault campaign enabled", UVM_LOW)
  endtask

  virtual task body();
    run_common_vseq_wrapper(num_trans);
  endtask : body

endclass
