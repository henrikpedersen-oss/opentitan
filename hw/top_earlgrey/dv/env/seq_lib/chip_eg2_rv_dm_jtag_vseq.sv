// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_jtag (EG2 spec REQ_PBF_20; testpoint chip_rv_dm_jtag): rv_dm's JTAG TAP, both modes.
//  - IDCODE reads the top's RvDmIdcodeValue, jtag_id_pkg::RV_DM_JTAG_IDCODE
//    (hw/top_earlgrey/rtl/jtag_id_pkg.sv: version 1, part type 2, TAP type 1, lowRISC's JEDEC
//    manufacturer id {bank 12, 0x6f}, bit 0 set), and a write of a random value leaves it so.
//    The expected value is built here from those documented fields, not taken from
//    cfg.m_jtag_dtm_ral, whose IDCODE reset is the block bench's `BUILD_SEED` (chip_env_cfg.sv:
//    RV_DM_JTAG_IDCODE), which the chip top does not use.
//  - BYPASS (IR 0x00 and IR 0x1f) holds 0.
//  - DTMCS: version 1 (RISC-V Debug Specification 0.13.2, which rv_dm follows: README), dmistat
//    0 (no DMI error), abits at least 7.
// Each check is repeated n_rounds times, at the drawn moments after the software's ready line.
class chip_eg2_rv_dm_jtag_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_jtag_vseq)
  `uvm_object_new

  localparam bit [31:0] RvDmIdcode = {4'h1, 12'h002, 4'h1, 4'd12, 7'b110_1111, 1'b1};

  rand int unsigned n_rounds;
  constraint n_rounds_c {
    n_rounds dist {1 := 40, [2:4] :/ 60};
  }

  virtual task body();
    uvm_reg_data_t v, wr;
    super.body();
    wait_ready();
    for (int i = 0; i < n_rounds; i++) begin
      csr_rd(.ptr(cfg.m_jtag_dtm_ral.idcode), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, RvDmIdcode, "IDCODE")
      `DV_CHECK_STD_RANDOMIZE_FATAL(wr)
      csr_wr(.ptr(cfg.m_jtag_dtm_ral.idcode), .value(wr), .blocking(1), .predict(0));
      csr_rd(.ptr(cfg.m_jtag_dtm_ral.idcode), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, RvDmIdcode, "IDCODE after a write")
      csr_rd(.ptr(cfg.m_jtag_dtm_ral.bypass0), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, 0, "BYPASS (IR 0x00)")
      csr_rd(.ptr(cfg.m_jtag_dtm_ral.bypass1), .value(v), .blocking(1));
      `DV_CHECK_EQ(v, 0, "BYPASS (IR 0x1f)")
      csr_rd(.ptr(cfg.m_jtag_dtm_ral.dtmcs), .value(v), .blocking(1));
      `DV_CHECK_EQ(get_field_val(cfg.m_jtag_dtm_ral.dtmcs.version, v), 1, "DTMCS.version")
      `DV_CHECK_EQ(get_field_val(cfg.m_jtag_dtm_ral.dtmcs.dmistat, v), 0, "DTMCS.dmistat")
      `DV_CHECK_GE(get_field_val(cfg.m_jtag_dtm_ral.dtmcs.abits, v), 7, "DTMCS.abits")
    end
    finish(1);
  endtask

endclass : chip_eg2_rv_dm_jtag_vseq
