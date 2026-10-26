// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_prim_tl_access (sw/device/tests/eg2_rram/eg2_rram_prim_tl_access.c; EG2 spec
// REQ_RRAM_25). The bench's half: after `EG2 RRAM prim intg` it flips the command integrity of the
// next read that reaches rram_macro's `prim` register block (at the block's input, one cycle), and
// after `EG2 RRAM prim done` it resets the chip into the second boot.
class chip_sw_eg2_rram_prim_tl_access_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_prim_tl_access_vseq)

  `uvm_object_new

  localparam string PrimTl = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_macro.u_reg_prim.tl_i";

  rand bit lock;
  rand bit fault;

  constraint knobs_c {
    fault dist {1'b1 := 60, 1'b0 := 40};
  }

  virtual function void write_params();
    set_param("lock", lock);
    set_param("fault", fault);
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  virtual task body();
    string line;
    int unsigned t;
    uvm_hdl_data_t intg;
    super.body();
    wait_sw_line("EG2 RRAM prim intg", line, 200_000_000);
    if (line == "") return;
    for (t = 0; t < 200_000; t++) begin
      if (rd({PrimTl, ".a_valid"}) == 1) break;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    end
    `DV_CHECK_LT_FATAL(t, 200_000, "no access reached rram_macro.prim")
    intg = rd({PrimTl, ".a_user.cmd_intg"});
    intg[0] = ~intg[0];
    // TODO(inject-registry): announce() reaches the registry only in EG2_PERIPH_SB builds.
    announce("chip_sw_eg2_rram_prim_tl_access_vseq", "rram_ctrl",
             "rram_macro.prim command integrity bit 0 flipped on one access");
    if (!(uvm_hdl_force({PrimTl, ".a_user.cmd_intg"}, intg))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_force({PrimTl, \".a_user.cmd_intg\"}, intg)) %s", PrimTl))
    @(posedge cfg.chip_vif.cpu_clk_rst_if.clk);
    if (!(uvm_hdl_release({PrimTl, ".a_user.cmd_intg"}))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_release({PrimTl, \".a_user.cmd_intg\"})) %s", PrimTl))
    wait_sw_line("EG2 RRAM prim done", line, 200_000_000, 1);
    if (line == "") return;
    set_param("boot_phase", 1);
    `uvm_info(`gfn, "EG2 RRAM prim: second boot", UVM_LOW)
    apply_reset();
  endtask

endclass : chip_sw_eg2_rram_prim_tl_access_vseq
