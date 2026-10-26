// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_eg2_fetch_intg[_cheriot] (EG2 spec REQ_CIB_07; software
// sw/device/tests/eg2_cib/eg2_fetch_intg.c, whose header holds the checks and the documents).
//
// The bench's part, on the software's lines:
//   - `EG2 FETCH place <slot> <w0> <w1> <w2> <w3>` (ROM source): the slot's words written into the
//     ROM by backdoor, scrambled with the ROM's key and with their integrity (rom_ctrl_bkdr_util),
//     and given to Spike (RISC-V mode), whose fetches see no backdoor write;
//   - `EG2 FETCH arm <addr>`: the next instruction fetch of the word at <addr> at the core's
//     instruction port (rv_core_ibex corei_tl_h_o; any of the fetches the prefetcher or the
//     instruction cache makes) is remembered by its a_source, and the response with that d_source
//     (corei_tl_h_i, before rv_core_ibex checks it) is corrupted for its one cycle the way
//     chip_sw_cheriot_bus_intg corrupts the data port (chip_sw_eg2_rand_vseq corrupt_next_d): one
//     bit of d_user.data_intg (field 0), of d_user.rsp_intg (field 1), or one immediate bit of
//     d_data with the integrity left as it was (field 2). The fault is given to the model as a
//     failed fetch at <addr> (Spike in RISC-V mode; CHERIoT-Sail in CHERIoT mode since 2026-10-25),
//     on the instruction the chip took the fetch error on, so the instruction check expects an
//     instruction access fault there; it is announced (the injection registry) to alert_handler,
//     and to insn only when no model takes it. The
//     bench then checks that rv_core_ibex_fatal_hw_err reached the alert handler (ALERT_CAUSE by
//     backdoor).
class chip_sw_eg2_fetch_intg_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_fetch_intg_vseq)

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string CoreiA = {PdMain, ".u_rv_core_ibex.corei_tl_h_o"};
  localparam string CoreiD = {PdMain, ".u_rv_core_ibex.corei_tl_h_i"};
  localparam bit [31:0] RomBase = top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR;

  typedef enum int unsigned {SrcRom, SrcSec, SrcMain} source_e;
  typedef enum int unsigned {FieldDataIntg, FieldRspIntg, FieldData} field_e;

  rand source_e     source;
  rand field_e      field;
  rand int unsigned bit_idx;
  rand bit [10:0]   imm;
  bit               cheriot_image;

  constraint knobs_c {
    source dist {SrcRom := 40, SrcSec := 30, SrcMain := 30};
    field dist {FieldDataIntg := 35, FieldRspIntg := 35, FieldData := 30};
    (field == FieldData) -> bit_idx inside {[20:31]};
    (field != FieldData) -> bit_idx inside {[0:6]};
    imm != 0;
  }

  `uvm_object_new

  function void pre_randomize();
    string images;
    super.pre_randomize();
    cheriot_image = $value$plusargs("sw_images=%s", images) && has_cheriot(images);
  endfunction

  protected function bit has_cheriot(string s);
    for (int i = 0; i + 8 <= s.len(); i++) begin
      if (s.substr(i, i + 7) == "_cheriot") return 1'b1;
    end
    return 1'b0;
  endfunction

  virtual function void write_params();
    set_param("source", source);
    set_param("field", field);
    set_param("bit", bit_idx);
    set_param("imm", imm);
  endfunction

  protected eg2_iss_wrapper iss;

  protected function void find_iss();
    uvm_component c = uvm_root::get().find("*.m_eg2_iss");
    if (c != null) void'($cast(iss, c));
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  // The slot's words into the ROM (and Spike).
  protected function void place_rom(bit [31:0] slot, bit [31:0] w[4]);
    rom_ctrl_bkdr_util rom;
    `downcast(rom, cfg.mem_bkdr_util_h[Rom])
    for (int i = 0; i < 8; i++) begin
      bit [31:0] data = i < 4 ? w[i] : '0;
      rom.rom_encrypt_write32_integ(slot + 4 * i - RomBase, {7'd0, data}, 1'b1);
      // Both modes since 2026-10-25: the ISS wrapper gives the word to whichever model runs.
      if (iss != null) iss.mem_write_event(slot + 4 * i, data);
    end
    `uvm_info(`gfn, $sformatf("EG2 FETCH slot written into the ROM at 0x%08x", slot), UVM_LOW)
  endfunction

  // Corrupts the response to the next fetch of `addr`.
  protected task corrupt_fetch(bit [31:0] addr);
    tlul_pkg::tl_h2d_t h;
    tlul_pkg::tl_d2h_t d;
    uvm_hdl_data_t v;
    bit [top_pkg::TL_AIW-1:0] src;
    bit hit;
    bit told;
    // The request: the fetch of `addr`, accepted (a_valid with a_ready).
    for (int cyc = 0; cyc < 2_000_000 && !hit; cyc++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      v = rd(CoreiA);
      h = tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
      v = rd(CoreiD);
      d = tlul_pkg::tl_d2h_t'(v[$bits(tlul_pkg::tl_d2h_t)-1:0]);
      if (h.a_valid && d.a_ready && h.a_address == addr) begin
        src = h.a_source;
        hit = 1'b1;
      end
    end
    `DV_CHECK_FATAL(hit, $sformatf("no instruction fetch of 0x%08x", addr))
    // Its response.
    hit = 1'b0;
    for (int cyc = 0; cyc < 200_000 && !hit; cyc++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      v = rd(CoreiD);
      d = tlul_pkg::tl_d2h_t'(v[$bits(tlul_pkg::tl_d2h_t)-1:0]);
      if (d.d_valid && d.d_source == src) hit = 1'b1;
    end
    `DV_CHECK_FATAL(hit, $sformatf("no response to the fetch of 0x%08x", addr))
    case (field)
      FieldDataIntg: d.d_user.data_intg[bit_idx % 7] ^= 1'b1;
      FieldRspIntg:  d.d_user.rsp_intg[bit_idx % 7] ^= 1'b1;
      default:       d.d_data[bit_idx % 32] ^= 1'b1;
    endcase
    // The model (Spike, or CHERIoT-Sail in CHERIoT mode) takes the fault on the instruction the chip
    // took a fetch error on (eg2_iss_wrapper, reviews R11/R12 2026-10-25). When it does, the
    // instruction check is not a target: the chip must fault exactly where the model does, and a
    // mismatch is a real one (before, CHERIoT-Sail was not told, and the open-ended announcement
    // made every later mismatch of the run EXPECTED).
    told = iss != null && iss.iside_error_event(addr);
    announce("chip_sw_eg2_fetch_intg_vseq", told ? "alert_handler" : "insn,alert_handler",
             $sformatf("fetch of 0x%08x: %s bit %0d flipped", addr, field.name(), bit_idx));
    if (told) begin
      `uvm_info(`gfn, $sformatf("EG2 FETCH model told: the fetch of 0x%08x fails", addr), UVM_LOW)
    end
    `DV_CHECK_FATAL(uvm_hdl_force(CoreiD, uvm_hdl_data_t'(d)), CoreiD)
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    #1ps;
    `DV_CHECK_FATAL(uvm_hdl_release(CoreiD), CoreiD)
  endtask

  virtual task body();
    string line;
    super.body();
    find_iss();
    `uvm_info(`gfn, $sformatf("EG2 FETCH: %s image, source %s, field %s, bit %0d",
                              cheriot_image ? "CHERIoT" : "RISC-V", source.name(), field.name(),
                              bit_idx), UVM_LOW)
    forever begin
      bit [31:0] slot, addr;
      bit [31:0] w[4];
      wait_sw_line("EG2 FETCH ", line, 200_000_000, 1);
      if (line == "") return;
      if ($sscanf(line, "EG2 FETCH place 0x%h 0x%h 0x%h 0x%h 0x%h", slot, w[0], w[1], w[2],
                  w[3]) == 5) begin
        place_rom(slot, w);
      end else if ($sscanf(line, "EG2 FETCH arm 0x%h", addr) == 1) begin
        uvm_reg_data_t cause;
        int unsigned alert = TopEarlgreyAlertIdRvCoreIbexFatalHwErr;
        corrupt_fetch(addr);
        for (int i = 0; i < 2000; i++) begin
          csr_rd(.ptr(ral.alert_handler.alert_cause[alert]), .value(cause), .backdoor(1));
          if (cause[0]) break;
          cfg.chip_vif.cpu_clk_rst_if.wait_clks(10);
        end
        `DV_CHECK_EQ(cause[0], 1, "rv_core_ibex_fatal_hw_err not at the alert handler")
      end
    end
  endtask

endclass : chip_sw_eg2_fetch_intg_vseq
