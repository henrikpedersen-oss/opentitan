// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The SBA path of chip_sw_eg2_boot_handover_protection[_cheriot] (EG2 verification spec
// REQ_BOOT_10; software eg2_boot_handover_protection.c, path 4) and of
// chip_sw_eg2_sec_creator_isolation[_cheriot] (REQ_SEC_03; eg2_sec_creator_isolation.c, path 3).
// Behind the full secure-boot chain, in RMA (debug enabled); the runs need +use_jtag_dmi=1.
//
// When the owner image has made its own checks it sets `kEg2SbaReady`; the bench then, through
// the debug module's system bus access (rv_dm, the JTAG debugger model):
//   1. writes a word of ROM_EXT's slot A through the RRAM host window: the write is refused (the
//      host window takes reads only, rram_ctrl "Memory-Mapped Reads via host_tl"), and the word's
//      RRAM row is unchanged (backdoor);
//   2. writes MP_REGION_CFG_0 (ROM_EXT's slot A, locked by ROM_EXT: rom_ext_nvm_protect_self) with
//      every field MuBi4True: it reads back unchanged over SBA, and REGION_CFG_REGWEN_0 reads 0.
// It writes the number of failed checks into `kEg2BenchVerdict`, then `kEg2BenchDone`; the software
// checks both (`EG2CHK FAIL bench's SBA checks`).
//
// +eg2_inject_fail=1 expects the host-window write to complete: the bench's check fails
// (`EG2CHK FAIL SBA write into ROM_EXT's slot refused`) and so does the software's.
class chip_sw_eg2_boot_handover_protection_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_boot_handover_protection_vseq)

  `uvm_object_new

  localparam bit [31:0] SbaReady = 32'h4142_5353;  // "SSBA"
  localparam bit [31:0] BenchDone = 32'h454e_4f44;  // "DONE"
  // A word of ROM_EXT's slot A that its image covers (the software's path 1 uses the same one).
  localparam bit [31:0] RomExtWordOff = 32'h400;

  virtual task pre_start();
    tap = TapRvDm;
    super.pre_start();
  endtask

  protected function bit [127:0] rram_row(bit [31:0] off);
    return rram_ctrl_bkdr_util_pkg::rram_row_read(
        $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4));
  endfunction

  virtual task body();
    bit found;
    super.body();
    wait_sym("kEg2SbaReady", 0, SbaReady, found, cfg.sw_test_timeout_ns);
    if (!found) begin
      `uvm_info(`gfn, "the software ended before its SBA step (or does not run it)", UVM_LOW)
      return;
    end
    sba_checks();
    sym_wr32("kEg2BenchVerdict", 0, bench_fails);
    sym_wr32("kEg2BenchDone", 0, BenchDone);
  endtask

  protected task sba_checks();
    bit [31:0] host = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR;
    bit [31:0] core = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR;
    bit [31:0] cfg_before, cfg_after, regwen;
    bit [127:0] row_before;
    sba_access_err_e st;

    cfg.debugger.set_dmactive(1);

    row_before = rram_row(RomExtWordOff);
    if (inject_fail) announce(`gfn, "SBA write into ROM_EXT's slot refused",
                              "+eg2_inject_fail: the write expected to complete");
    sba_wr32(host + RomExtWordOff, ~row_before[32 * RomExtWordOff[3:2] +: 32], st);
    void'(check("SBA write into ROM_EXT's slot refused", (st != SbaErrNone) ^ inject_fail,
                $sformatf("status %s", st.name())));
    void'(check("ROM_EXT's RRAM row unchanged (backdoor)", rram_row(RomExtWordOff) == row_before));

    sba_rd32(core + rram_ctrl_reg_pkg::RRAM_CTRL_MP_REGION_CFG_0_OFFSET, cfg_before, st);
    void'(check("SBA read of MP_REGION_CFG_0", st == SbaErrNone, st.name()));
    sba_wr32(core + rram_ctrl_reg_pkg::RRAM_CTRL_MP_REGION_CFG_0_OFFSET, 32'h0006_6666, st);
    sba_rd32(core + rram_ctrl_reg_pkg::RRAM_CTRL_MP_REGION_CFG_0_OFFSET, cfg_after, st);
    void'(check("MP_REGION_CFG_0 unchanged after an SBA write", cfg_after == cfg_before,
                $sformatf("before 0x%08h after 0x%08h", cfg_before, cfg_after)));
    sba_rd32(core + rram_ctrl_reg_pkg::RRAM_CTRL_REGION_CFG_REGWEN_0_OFFSET, regwen, st);
    void'(check("REGION_CFG_REGWEN_0 reads 0 over SBA", regwen == 0,
                $sformatf("0x%08h", regwen)));
  endtask

endclass : chip_sw_eg2_boot_handover_protection_vseq

// chip_sw_eg2_sec_creator_isolation[_cheriot] (REQ_SEC_03): the same SBA path, the software's
// path 3 (eg2_sec_creator_isolation.c). System bus reads of the Creator info pages are not
// possible (they have no host window), so the SBA path is the writes above.
class chip_sw_eg2_sec_creator_isolation_vseq extends chip_sw_eg2_boot_handover_protection_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_creator_isolation_vseq)

  `uvm_object_new

endclass : chip_sw_eg2_sec_creator_isolation_vseq
