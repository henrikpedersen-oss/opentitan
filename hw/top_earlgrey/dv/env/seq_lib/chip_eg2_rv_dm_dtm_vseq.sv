// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_dtm (EG2 spec REQ_PBF_20; testpoint chip_rv_dm_dtm): the DTM and the debug module's
// registers, both modes. hartsel 0 selects the core; hartinfo reads the documented values
// (hw/vendor/pulp_riscv_dbg/doc/debug-system.md "hartinfo": nscratch 2, dataaccess 1, datasize
// dm::DataCount = 2, dataaddr dm::DataAddr = 0x380); random words written to data0 and data1
// read back, n_writes times.
class chip_eg2_rv_dm_dtm_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_dtm_vseq)
  `uvm_object_new

  rand int unsigned n_writes;
  constraint n_writes_c {
    n_writes dist {1 := 20, [2:8] :/ 80};
  }

  virtual task body();
    uvm_reg_data_t v, d0, d1;
    super.body();
    wait_ready();
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartsello), .value(0), .blocking(1));
    csr_wr(.ptr(cfg.jtag_dmi_ral.dmcontrol.hartselhi), .value(0), .blocking(1));
    csr_rd(.ptr(cfg.jtag_dmi_ral.hartinfo), .value(v), .blocking(1));
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.hartinfo.nscratch, v), 2, "hartinfo.nscratch")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.hartinfo.dataaccess, v), 1, "hartinfo.dataaccess")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.hartinfo.datasize, v), 2, "hartinfo.datasize")
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.hartinfo.dataaddr, v), 'h380, "hartinfo.dataaddr")
    for (int i = 0; i < n_writes; i++) begin
      `DV_CHECK_STD_RANDOMIZE_FATAL(d0)
      `DV_CHECK_STD_RANDOMIZE_FATAL(d1)
      d0 = d0[31:0];
      d1 = d1[31:0];
      csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[0]), .value(d0), .blocking(1));
      csr_wr(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(d1), .blocking(1));
      csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[0]), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, d0, "data0 read back")
      csr_rd(.ptr(cfg.jtag_dmi_ral.abstractdata[1]), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, d1, "data1 read back")
    end
    finish(2);
  endtask

endclass : chip_eg2_rv_dm_dtm_vseq
