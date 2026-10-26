// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_eg2_illegal_sweep[_cheriot] (EG2 spec REQ_CIB_07; software
// sw/device/tests/eg2_cib/eg2_illegal_sweep.c, decode table and code slots in eg2_isa_decode.h).
//
// The knobs: the index range (first, count; the dvsim list pins them to split the 98,304
// encodings into short runs), the 32-bit part's operand fields (fixed a5, a4, a3 unless drawn
// otherwise), the weights of the batch placements.
//
// The bench's part, on each software line `EG2 ISA batch <place> <slots> <first> <n> <ops>`:
//   - ROM (place 3): the batch's 64 code slots (eg2_isa_decode.h: probe, encoding, CSR restore,
//     ebreak; unused slots 0) written into the ROM by backdoor, scrambled with the ROM's key and
//     its integrity (rom_ctrl_bkdr_util), and the first time the 4 KiB of zeros on each side; the
//     software reads every word back and checks it against its own slots before it runs them;
//   - RISC-V mode, ROM and RRAM (place 2): the same words to Spike (eg2_iss_wrapper
//     mem_write_event), whose fetches see neither a backdoor write nor rram_ctrl's controller
//     writes. In CHERIoT mode CHERIoT-Sail executes the RVFI word and needs none.
// Every instruction and trap is checked by the scoreboard's instruction check (en_eg2_cosim): the
// reference model's legal/illegal split against the chip's, encoding by encoding; the software
// checks the chip against the documents' decode table. Each `EG2 ISA cov` line samples
// eg2_illegal_cg (encoding class x placement x outcome, per mode).
class chip_sw_eg2_illegal_sweep_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_illegal_sweep_vseq)

  localparam int unsigned IsaCount = 65536 + 32768;
  localparam int unsigned SlotWords = 8;
  localparam int unsigned SlotBytes = 32;
  localparam int unsigned MaxSlots = 64;
  localparam int unsigned PadBytes = 'h1000;
  localparam int unsigned PlaceRram = 2;
  localparam int unsigned PlaceRom = 3;
  localparam bit [31:0] Nop = 32'h0000_0013;
  localparam bit [31:0] Ebreak = 32'h0010_0073;
  localparam bit [31:0] RomBase = top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR;

  rand bit [31:0] first, count;
  rand bit        fixed_ops;
  rand bit [4:0]  rd_f, rs1_f, rs2_f;
  rand int unsigned w_place_main, w_place_sec, w_place_rram, w_place_rom;
  bit cheriot_image;

  constraint range_c {
    count dist {1024 :/ 80, [64:2048] :/ 20};
    count % 64 == 0;
    first % 64 == 0;
    first + count <= IsaCount;
  }
  // The spec's sweep has fixed operands; a run in five draws other fields, so the CSR numbers and
  // register forms the fixed fields never reach are swept too. CHERIoT mode has x0-x15: most of
  // its draws stay there, so the register forms are not all illegal.
  constraint ops_c {
    fixed_ops dist {1'b1 := 80, 1'b0 := 20};
    fixed_ops -> (rd_f == 15 && rs1_f == 14 && rs2_f == 13);
    (!fixed_ops && cheriot_image) -> (rd_f < 16 && rs1_f < 16);
    (!fixed_ops && cheriot_image) -> rs2_f dist {[0:15] :/ 80, [16:31] :/ 20};
  }
  constraint place_c {
    w_place_main inside {[0:4]};
    w_place_sec inside {[1:4]};
    w_place_rram inside {[0:4]};
    w_place_rom inside {[0:4]};
    soft w_place_main == 1;
    soft w_place_sec == 1;
    soft w_place_rram == 1;
    soft w_place_rom == 1;
  }

  covergroup eg2_illegal_cg with function sample(bit cheriot, int unsigned cls, int unsigned place,
                                                 bit illegal);
    cp_mode: coverpoint cheriot;
    cp_class: coverpoint cls {bins cls[] = {[0:16]};}
    cp_place: coverpoint place {bins place[] = {[0:3]};}
    cp_outcome: coverpoint illegal {bins executed = {0}; bins illegal_trap = {1};}
    cross_all: cross cp_mode, cp_class, cp_place, cp_outcome {
      // An absent opcode (class 16) is never executed; 0x5B and AUICGP (14, 15) only run in
      // CHERIoT mode.
      ignore_bins absent_runs = binsof(cp_class) intersect {16} && binsof(cp_outcome.executed);
      ignore_bins cheri_in_rv = binsof(cp_mode) intersect {0} &&
                                binsof(cp_class) intersect {14, 15} &&
                                binsof(cp_outcome.executed);
    }
  endgroup

  function new(string name = "");
    super.new(name);
    eg2_illegal_cg = new();
  endfunction

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
    set_param("first", first);
    set_param("count", count);
    set_param("rd_f", rd_f);
    set_param("rs1_f", rs1_f);
    set_param("rs2_f", rs2_f);
    set_param("w_place_main", w_place_main);
    set_param("w_place_sec", w_place_sec);
    set_param("w_place_rram", w_place_rram);
    set_param("w_place_rom", w_place_rom);
  endfunction

  // ---- eg2_isa_decode.c, repeated: the encoding of an index and its code slot ----

  protected function bit [31:0] encoding(bit [31:0] idx, bit [31:0] ops);
    bit [31:0] j;
    if (idx < 65536) return idx;
    j = idx - 65536;
    return {j[14:8], ops[14:10], ops[9:5], j[7:5], ops[4:0], j[4:0], 2'b11};
  endfunction

  protected function void slot_words(bit [31:0] insn, output bit [31:0] w[SlotWords]);
    bit [31:0] probe0 = Nop;
    bit [31:0] probe1 = Nop;
    bit [31:0] restore = '0;
    bit        is16 = insn[1:0] != 2'b11;
    bit [15:0] h[12];
    int unsigned n;
    foreach (w[i]) w[i] = '0;
    foreach (h[i]) h[i] = '0;
    if (!is16) begin
      bit [6:0] op = insn[6:0];
      bit [4:0] rd = insn[11:7];
      bit [2:0] f3 = insn[14:12];
      bit [4:0] rs1 = insn[19:15];
      bit [6:0] f7 = insn[31:25];
      bit [4:0] r = 5'd5;
      bit       rs1_ok;
      while (r == rd || r == rs1) r++;
      rs1_ok = rs1 != 0 && !(cheriot_image && rs1 >= 16);
      if (op == 7'h73 && f3 != 0 && f3 != 4) begin
        bit [11:0] csr = insn[31:20];
        probe0 = {csr, 5'd0, 3'd2, r, 7'h73};                    // csrrs R, csr, x0
        if (f3 == 1 && rs1_ok) probe1 = {12'd0, r, 3'd0, rs1, 7'h13};  // addi rs1, R, 0
        else if ((f3 == 2 || f3 == 3) && rs1_ok) probe1 = {20'd0, rs1, 7'h13};  // addi rs1, x0, 0
        if ((f3 == 1 && rs1 == 0) || f3 == 5 || ((f3 == 6 || f3 == 7) && rs1 != 0)) begin
          restore = {csr, r, 3'd1, 5'd0, 7'h73};                 // csrrw x0, csr, R
        end
      end else if (cheriot_image && op == 7'h5b && f3 == 0 && f7 == 7'h01) begin
        bit [4:0] scr = insn[24:20];
        probe0 = {7'h01, scr, 5'd0, 3'd0, r, 7'h5b};               // cspecialrw cR, scr, c0
        if (rs1_ok) probe1 = {7'h7f, 5'h0a, r, 3'd0, rs1, 7'h5b};  // cmove cs1, cR
      end
    end
    w[0] = probe0;
    w[1] = probe1;
    h[n++] = insn[15:0];
    if (!is16) h[n++] = insn[31:16];
    if (restore != '0) begin
      h[n++] = restore[15:0];
      h[n++] = restore[31:16];
    end
    h[n++] = Ebreak[15:0];
    h[n++] = Ebreak[31:16];
    for (int i = 0; i < 12; i++) w[2 + i / 2] |= {16'd0, h[i]} << (16 * (i % 2));
  endfunction

  // ---- the bench's writes ----

  protected eg2_iss_wrapper iss;
  protected bit             rom_pads_done;

  protected function void find_iss();
    uvm_component c = uvm_root::get().find("*.m_eg2_iss");
    if (c != null) void'($cast(iss, c));
  endfunction

  // One word: into the ROM (backdoor, scrambled, with its integrity) and, in RISC-V mode, Spike.
  protected function void put_word(bit to_rom, bit [31:0] addr, bit [31:0] data);
    if (to_rom) begin
      rom_ctrl_bkdr_util rom;
      `downcast(rom, cfg.mem_bkdr_util_h[Rom])
      rom.rom_encrypt_write32_integ(addr - RomBase, {7'd0, data}, 1'b1);
    end
    if (!cheriot_image && iss != null) iss.mem_write_event(addr, data);
  endfunction

  protected function void write_batch(bit to_rom, bit [31:0] slots, bit [31:0] b_first,
                                      bit [31:0] n, bit [31:0] ops);
    for (int unsigned k = 0; k < MaxSlots; k++) begin
      bit [31:0] w[SlotWords];
      if (k < n) slot_words(encoding(b_first + k, ops), w);
      else foreach (w[i]) w[i] = '0;
      foreach (w[i]) put_word(to_rom, slots + SlotBytes * k + 4 * i, w[i]);
    end
  endfunction

  protected function void write_zeros(bit to_rom, bit [31:0] lo, bit [31:0] hi);
    for (bit [31:0] a = lo; a < hi; a += 4) put_word(to_rom, a, '0);
  endfunction

  virtual task body();
    string line;
    super.body();
    find_iss();
    `uvm_info(`gfn, $sformatf("EG2 ISA sweep: %s image, ISS %s", cheriot_image ? "CHERIoT" : "RISC-V",
                              iss == null ? "absent" : "present"), UVM_LOW)
    forever begin
      bit [31:0] place, slots, b_first, n, ops, cls, cnt_ill, cnt_exec;
      wait_sw_line("EG2 ISA ", line, 400_000_000, 1);
      if (line == "") return;
      if ($sscanf(line, "EG2 ISA batch 0x%h 0x%h 0x%h 0x%h 0x%h", place, slots, b_first, n,
                  ops) == 5) begin
        if (place == PlaceRom) begin
          // Placed code, not a fault: nothing to announce.
          if (!rom_pads_done) begin
            write_zeros(1'b1, slots - PadBytes, slots);
            write_zeros(1'b1, slots + MaxSlots * SlotBytes, slots + MaxSlots * SlotBytes + PadBytes);
            rom_pads_done = 1'b1;
          end
          write_batch(1'b1, slots, b_first, n, ops);
          `uvm_info(`gfn, $sformatf("EG2 ISA ROM batch at 0x%08x: indices 0x%0x + %0d", slots,
                                    b_first, n), UVM_MEDIUM)
        end else if (place == PlaceRram && !cheriot_image) begin
          write_batch(1'b0, slots, b_first, n, ops);  // Spike only: the software wrote the RRAM
        end
      end else if ($sscanf(line, "EG2 ISA cov 0x%h 0x%h 0x%h 0x%h", cls, place, cnt_ill,
                           cnt_exec) == 4) begin
        if (cnt_ill != 0) eg2_illegal_cg.sample(cheriot_image, cls, place, 1'b1);
        if (cnt_exec != 0) eg2_illegal_cg.sample(cheriot_image, cls, place, 1'b0);
      end
    end
  endtask

endclass : chip_sw_eg2_illegal_sweep_vseq
