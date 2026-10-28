// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_host_intg (sw/device/tests/eg2_rram/eg2_rram_host_intg.c; EG2 spec REQ_RRAM_12).
// The bench's half: on `EG2 RRAM host_intg data|fetch off=..` it finds the RRAM read-buffer entry
// that holds that RRAM word, waits until its shadow read has verified it, and flips one data bit
// of the word's first bus word in the entry, leaving its integrity bits as they are
// (theory_of_operation.md "RRAM Read Buffer": four 39-bit words, data and bus integrity). On
// `EG2 RRAM host_intg done` it resets the chip into the software's second boot.
class chip_sw_eg2_rram_host_intg_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_host_intg_vseq)

  `uvm_object_new

  localparam string RdPath =
      "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl.u_rram_phy.u_rram_phy_rd";
  localparam int NumRdBuf = 4;
  localparam int BusFullWidth = 39;
  localparam bit [1:0] AttrInvalid = 2'h0, AttrVerified = 2'h3;

  // The reference model's ISS (en_eg2_cosim), told of the corrupted fetch (R16, 2026-10-26).
  protected eg2_iss_wrapper iss;

  rand int unsigned paths;
  constraint paths_c {
    paths dist {1 := 30, 2 := 30, 3 := 40};
  }

  virtual function void write_params();
    set_param("paths", paths);
  endfunction

  protected function string buf_path(int i, string field);
    return $sformatf("%s.gen_bufs[%0d].u_rram_phy_rd_buffer.rd_buf_q.%s", RdPath, i, field);
  endfunction

  protected function bit [63:0] hdl_rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v[63:0];
  endfunction

  protected task corrupt_entry(bit [31:0] off, string kind);
    int entry = -1;
    bit [31:0] word_addr = off >> 4;
    // The entry the software's load filled, verified by its shadow read.
    for (int t = 0; t < 4000 && entry < 0; t++) begin
      for (int i = 0; i < NumRdBuf; i++) begin
        if (hdl_rd(buf_path(i, "attr")) == AttrVerified &&
            hdl_rd(buf_path(i, "addr")) == word_addr) begin
          entry = i;
        end
      end
      if (entry < 0) cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    end
    `DV_CHECK_FATAL(entry >= 0, $sformatf("no verified read-buffer entry holds RRAM word 0x%0h",
                                          word_addr))
    begin
      uvm_hdl_data_t data;
      int unsigned bit_idx = $urandom_range(0, 31);  // bus word 0, a data bit
      bit          told;  // R16, 2026-10-26
      if (!(uvm_hdl_read(buf_path(entry, "data"), data))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read(buf_path(entry, \"data\"), data)) %s", buf_path(entry, "data")))
      data[bit_idx] = ~data[bit_idx];
      // The fetch case: the routine's first instruction (bus word 0 of the entry, the row's first
      // word) fails its bus integrity check, which Ibex takes as an instruction access fault
      // (ibex_if_stage.sv instr_err = instr_intg_err | instr_bus_err_i). The model is told, on the
      // instruction the chip took the fetch error on (as chip_sw_eg2_fetch_intg_vseq and
      // _rram_uncorr_fetch_vseq tell it), and the instruction check is then not a target: a
      // mismatch there is a real one. Before, the model was not told: Spike ran the routine,
      // stayed diverged until the reset and the one-mismatch announcement covered only the first
      // mismatch (review R16, 2026-10-26). The data case needs nothing new: the load's integrity
      // error is an internal NMI, which reaches the model with the RVFI item (nmi_int).
      if (kind == "fetch") begin
        if (iss == null) begin
          uvm_component c = uvm_root::get().find("*.m_eg2_iss");
          if (c != null) void'($cast(iss, c));
        end
        told = iss != null &&
               iss.iside_error_event(top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR +
                                     {word_addr[27:0], 4'h0});
        if (told) begin
          `uvm_info(`gfn, $sformatf("EG2 RRAM host_intg model told: the fetch of RRAM word 0x%0h fails",
                                    word_addr), UVM_LOW)
        end
      end
      announce("chip_sw_eg2_rram_host_intg_vseq", told ? "rv_core_ibex,rvfi" : "rv_core_ibex,insn,rvfi",
               $sformatf("RRAM read buffer entry %0d (word 0x%0h, %s) data bit %0d flipped",
                         entry, word_addr, kind, bit_idx));
      if (!(uvm_hdl_deposit(buf_path(entry, "data"), data))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_deposit(buf_path(entry, \"data\"), data)) %s", buf_path(entry, "data")))
    end
  endtask

  virtual task body();
    string line;
    super.body();
    forever begin
      bit [31:0] off;
      wait_sw_line("EG2 RRAM host_intg ", line, 200_000_000, 1);
      if (line == "") return;
      if ($sscanf(line, "EG2 RRAM host_intg data off=0x%h", off) == 1) begin
        corrupt_entry(off, "data");
      end else if ($sscanf(line, "EG2 RRAM host_intg fetch off=0x%h", off) == 1) begin
        corrupt_entry(off, "fetch");
      end else if (line == "EG2 RRAM host_intg done") begin
        set_param("boot_phase", 1);
        `uvm_info(`gfn, "EG2 RRAM host_intg: second boot", UVM_LOW)
        apply_reset();
        return;
      end
    end
  endtask

endclass : chip_sw_eg2_rram_host_intg_vseq
