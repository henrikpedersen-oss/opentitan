// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The memory tests after boot (eg2_top_verification spec REQ_PBF_19: chip_sw_eg2_pbf_memory_sb
// and chip_sw_eg2_pbf_memory_fault; software sw/device/tests/eg2_pbf/eg2_pbf_memory_*.c).
//
// The bench side of the tests' contract (eg2_pbf_mem.h): a bench step is the DV status word
// written with 16'h5000 | step; the bench acts in the simulation step it sees it. The words under
// test are at fixed places both sides compute: main SRAM base + 0x3000 + 4 word, retention SRAM
// base + 0x1000 + 4 word, the OTTF non-volatile scratch area (`_non_volatile_scratch_start` of the
// image) + 16 word.

// Helpers the PBF memory, debug, ROM_EXT and CHERIoT vseqs share.
class chip_sw_eg2_pbf_mem_base_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_mem_base_vseq)
  `uvm_object_new

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam bit [15:0] BenchCode = 16'h5000;

  // Waits for the software's bench step `step` (eg2_pbf_bench_step).
  protected task wait_bench_step(int step);
    wait (cfg.sw_test_status_vif.sw_test_status == sw_test_status_e'(BenchCode | 16'(step)));
    `uvm_info(`gfn, $sformatf("EG2_PBF bench step %0d", step), UVM_LOW)
  endtask

  // The life cycle state of the run enables debug (lc_ctrl's signal table: TEST_UNLOCKED*, DEV,
  // RMA).
  protected function bit lc_debug_enabled();
    return cfg.use_otp_image inside {OtpTypeLcStDev, OtpTypeLcStRma, OtpTypeLcStTestUnlocked0,
                                     OtpTypeLcStTestUnlocked1, OtpTypeLcStTestUnlocked2};
  endfunction

  // A symbol's address in the test image.
  protected function bit [31:0] image_symbol(string symbol);
    longint unsigned addr, size;
    string elf = $sformatf("%0s.elf", cfg.sw_images[SwTypeTestSlotA]);
    `DV_CHECK_FATAL(dv_utils_pkg::sw_symbol_get_addr_size(elf, symbol, 0, addr, size),
                    $sformatf("no symbol %s in %s", symbol, elf))
    return addr[31:0];
  endfunction

  // Writes `data` to an SRAM word by backdoor through the scrambling, with `flips` applied to its
  // 39-bit integrity code word before it is scrambled: exactly those bits wrong at the reader's
  // integrity check. The default (test ROM) keys: no boot stage renewed them.
  protected function void sram_write_flipped(bit is_main, bit [31:0] addr, bit [31:0] data,
                                             bit [38:0] flips);
    sram_ctrl_bkdr_util sram;
    chip_mem_e mem = is_main ? RamMain0 : RamRet0;
    int num_tiles = is_main ? cfg.num_ram_main_tiles : cfg.num_ram_ret_tiles;
    bit [sram_scrambler_pkg::SRAM_KEY_WIDTH-1:0] key =
        is_main ? RndCnstSramCtrlMainSramKey : RndCnstSramCtrlRetSramKey;
    bit [sram_scrambler_pkg::SRAM_BLOCK_WIDTH-1:0] nonce =
        is_main ? RndCnstSramCtrlMainSramNonce : RndCnstSramCtrlRetSramNonce;
    bit [31:0] addr_scr;
    bit [38:0] data_scr;
    int size_bytes, tile;
    `downcast(sram, cfg.mem_bkdr_util_h[mem])
    size_bytes = sram.get_size_bytes();
    addr_scr = sram.get_sram_encrypt_addr(.addr(addr), .nonce(nonce),
                                          .extra_addr_bits($clog2(num_tiles)));
    tile = addr_scr / size_bytes;
    data_scr = sram.get_sram_encrypt32_intg_data(.addr(addr), .data(data), .key(key),
                                                 .nonce(nonce),
                                                 .extra_addr_bits($clog2(num_tiles)),
                                                 .flip_bits(flips));
    cfg.mem_bkdr_util_h[chip_mem_e'(mem + tile)].write39integ(addr_scr & (size_bytes - 1),
                                                              data_scr);
    // The word the bench wrote reaches the model (its integrity flips are the injected fault,
    // announced): review R19, 2026-10-26 (no hook, the model kept the old word).
    eg2_iss_wrapper::backdoor_write32(addr, data);
    `uvm_info(`gfn, $sformatf("EG2_PBF SRAM word 0x%08x = 0x%08x, integrity bits flipped 0x%010x",
                              addr, data, flips), UVM_LOW)
  endfunction

  // Flips `bit_idx` bits of the stored RRAM word (data and ECC as the macro keeps them: {ecc1,
  // ecc0, data1, data0}) that holds bus address `addr`.
  protected task rram_flip(bit [31:0] addr, int unsigned bit_idx[$]);
    bit [31:0] row = (addr - top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR) >> 4;
    bit [31:0] baddr = row << cfg.mem_bkdr_util_h[RramData].get_addr_lsb();
    mem_bkdr_util_pkg::row_data_t raw;
    wait_rram_rd_buf_rdy();
    raw = cfg.mem_bkdr_util_h[RramData].read(baddr);
    foreach (bit_idx[i]) raw[bit_idx[i]] = ~raw[bit_idx[i]];
    cfg.mem_bkdr_util_h[RramData].write(baddr, raw);
    `uvm_info(`gfn, $sformatf("EG2_PBF RRAM word 0x%08x (row %0d): bits %p flipped", addr, row,
                              bit_idx), UVM_LOW)
  endtask
endclass

// chip_sw_eg2_pbf_memory_sb: SRAM INIT, key renewal, RRAM program and rewrite.
class chip_sw_eg2_pbf_memory_sb_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_memory_sb_vseq)
  `uvm_object_new

  rand bit [3:0] ops;
  rand bit [1:0] range_class;
  rand bit       scramble;

  constraint knobs_c {
    ops != 0;
    ops dist {4'hf :/ 50, [4'h1 : 4'he] :/ 50};
  }

  virtual function void write_params();
    set_param("ops", ops);
    set_param("range_class", range_class);
    set_param("scramble", scramble);
  endfunction

  virtual task body();
    // Behind ROM_EXT the signed image keeps its defaults: every operation.
    bit [31:0] o = cfg.rom_ext_assembled ? 32'hf : pinned("ops", ops);
    if (o[0]) add_activity("sram_ctrl_sec INIT", {PdMain, ".u_sram_ctrl_sec.init_req"});
    if (o[1]) add_activity("sram_ctrl_sec key request", {PdMain, ".u_sram_ctrl_sec.key_req"});
    if (o[3:2] != 0) add_activity("RRAM programming", {PdMain, ".u_rram_ctrl.phy_wr_busy"});
    super.body();
  endtask
endclass

// chip_sw_eg2_pbf_memory_fault: an ECC / integrity error in the word the software uses.
class chip_sw_eg2_pbf_memory_fault_vseq extends chip_sw_eg2_pbf_mem_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_memory_fault_vseq)
  `uvm_object_new

  rand bit [1:0]  target;   // 0 main SRAM, 1 retention SRAM, 2 RRAM
  rand bit [1:0]  bits;     // 1, 2 (SRAM and RRAM), 3 (RRAM: uncorrectable)
  rand bit [5:0]  word;
  rand bit [31:0] data;

  constraint knobs_c {
    target inside {[0 : 2]};
    bits inside {[1 : 3]};
    target != 2 -> bits inside {[1 : 2]};
  }

  virtual function void write_params();
    set_param("target", target);
    set_param("bits", bits);
    set_param("word", word);
    set_param("data", data);
  endfunction

  virtual task body();
    bit [31:0] t = pinned("target", target), b = pinned("bits", bits), w = pinned("word", word),
               d = pinned("data", data);
    if (t != 2) add_activity("core bus integrity error", {PdMain, ".u_rv_core_ibex.fatal_intg_event"});
    super.body();
    fork inject(t, b, w, d); join_none
  endtask

  protected task inject(bit [31:0] t, bit [31:0] b, bit [31:0] w, bit [31:0] d);
    wait_bench_step(1);
    if (t inside {0, 1}) begin
      bit [38:0] flips;
      int unsigned p0 = $urandom_range(0, 38), p1;
      do p1 = $urandom_range(0, 38); while (p1 == p0);
      flips = (39'(1) << p0) | (b == 2 ? (39'(1) << p1) : '0);
      void'(announce_fault("chip_sw_eg2_pbf_memory_fault_vseq", "insn",
                           $sformatf("%0d integrity bits flipped", b)));
      sram_write_flipped(
          .is_main(t == 0),
          .addr(t == 0 ? top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR + 'h3000 + 4 * w
                       : top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR + 'h1000 + 4 * w),
          .data(d), .flips(flips));
    end else begin
      int unsigned idx[$];
      int unsigned a = $urandom_range(0, 63), c;
      do c = $urandom_range(0, 63); while (c == a);
      idx.push_back(a);                      // one bit of the low half
      if (b == 2) idx.push_back(64 + c);     // and one of the high half: corrected double
      if (b == 3) idx.push_back(c);          // two in the low half: uncorrectable
      if (b == 3) begin
        void'(announce_fault("chip_sw_eg2_pbf_memory_fault_vseq", "insn",
                             "uncorrectable RRAM ECC error"));
      end
      rram_flip(image_symbol("_non_volatile_scratch_start") + 16 * w, idx);
    end
  endtask
endclass
