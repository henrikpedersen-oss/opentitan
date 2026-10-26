// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_otbn_sb and _fault (eg2_top_verification spec REQ_PBF_11; software
// sw/device/tests/eg2_pbf/eg2_pbf_otbn_{sb,fault}.c). Behind ROM_EXT the signed image keeps its
// defaults (chip_sw_eg2_base_vseq) and the bench follows them.

// ECDSA P-256 sign and verify on OTBN.
class chip_sw_eg2_pbf_otbn_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_otbn_sb_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [1:0] op;       // 0 sign, 1 verify, 2 both
  rand bit [2:0] vec;      // SigVer vector 0-3 (0-1 valid, 2-3 invalid), 4 drawn
  rand bit [6:0] bad_pct;

  constraint knobs_c {
    op inside {[0 : 2]};
    op dist {2 :/ 60, [0 : 1] :/ 40};
    vec inside {[0 : 4]};
    bad_pct == 20;
  }

  virtual function void write_params();
    set_param("op", op);
    set_param("vec", vec);
    set_param("bad_pct", bad_pct);
  endfunction

  virtual task body();
    // REQ_PBF_05: OTBN executed (STATUS BUSY_EXECUTE, 0x01) at least once per operation.
    bit [31:0] o = cfg.rom_ext_assembled ? 2 : pinned("op", op);
    add_activity("otbn executes", {PD, ".u_otbn.u_reg.status_qs"}, o == 2 ? 2 : 1);
    super.body();
  endtask
endclass

// An ECDSA verification on OTBN faulted: an IMEM or DMEM integrity error made here through the
// backdoor while OTBN runs, or an escalation (made by the software).
class chip_sw_eg2_pbf_otbn_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_otbn_fault_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [1:0] fault;        // 0 IMEM, 1 DMEM integrity error, 2 escalation
  rand bit [1:0] fault_point;  // early, middle, late in the operation

  constraint knobs_c {
    fault inside {[0 : 2]};
    fault_point inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("fault", fault);
    set_param("fault_point", fault_point);
  endfunction

  protected function uvm_hdl_data_t otbn_status();
    uvm_hdl_data_t v;
    void'(uvm_hdl_read({PD, ".u_otbn.u_reg.status_qs"}, v));
    return v[7:0];
  endfunction

  // One bit flipped in every word of the memory: the next read of it is an integrity error.
  protected function void corrupt(chip_mem_e mem);
    mem_bkdr_util m = cfg.mem_bkdr_util_h[mem];
    for (uint32_t i = 0; i < m.get_depth(); i++) m.inject_errors(i * m.get_bytes_per_word(), 1);
  endfunction

  protected task inject(bit [31:0] f, bit [31:0] point);
    // The verification runs about 0.3 million OTBN cycles; the fault comes early, in the middle
    // or late in it.
    `DV_WAIT(otbn_status() == 8'h01, "EG2_PBF otbn: never busy executing", 100_000_000)
    #((point == 0 ? 20 : point == 1 ? 800 : 2000) * 1us);
    if (otbn_status() != 8'h01) begin
      `uvm_info(`gfn, "EG2_PBF otbn: the operation ended before the fault point", UVM_LOW)
    end
    void'(announce_fault("eg2_pbf_otbn_fault", "otbn",
                         f == 0 ? "IMEM integrity error" : "DMEM integrity error"));
    if (f == 0) begin
      corrupt(OtbnImem);
    end else begin
      for (int i = 0; i < cfg.num_otbn_dmem_tiles; i++) corrupt(chip_mem_e'(OtbnDmem0 + i));
    end
    `uvm_info(`gfn, $sformatf("EG2_PBF otbn: %s corrupted", f == 0 ? "IMEM" : "DMEM"), UVM_LOW)
    #10us;
    if (otbn_status() != 8'hff) `uvm_error(`gfn, "EG2_PBF otbn: not LOCKED after the integrity error")
  endtask

  // An escalation: OTBN must lock (a LIFECYCLE_ESCALATION fatal error) before the reset.
  protected task check_escalation();
    uvm_hdl_data_t v;
    forever begin
      void'(uvm_hdl_read({PD, ".lc_ctrl_lc_escalate_en"}, v));
      if (v[3:0] == 4'b0101) break;
      #100ns;
    end
    void'(announce_fault("eg2_pbf_otbn_fault escalation", "otbn", "life-cycle escalation"));
    #2us;
    if (otbn_status() != 8'hff) `uvm_error(`gfn, "EG2_PBF otbn: not LOCKED under escalation")
  endtask

  virtual task body();
    bit [31:0] f = pinned("fault", fault), p = pinned("fault_point", fault_point);
    if (cfg.rom_ext_assembled) begin
      f = 1;  // the image's defaults
      p = 1;
    end
    add_activity("otbn executes", {PD, ".u_otbn.u_reg.status_qs"}, 1);
    super.body();
    fork
      if (f < 2) inject(f, p);
      check_escalation();
    join_none
  endtask
endclass
