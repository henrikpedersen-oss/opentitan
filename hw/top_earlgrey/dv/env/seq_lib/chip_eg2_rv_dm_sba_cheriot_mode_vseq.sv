// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rv_dm_sba_cheriot_mode (sw/device/tests/eg2_debug/eg2_rv_dm_sba_cheriot_mode.c; EG2 spec
// REQ_CDM_06, REQ_CMS_16), CHERIoT mode. In a debug-enabled life-cycle state (the base class's
// draw) the bench makes `n_ops` system bus accesses over the capabilities the software stored:
//
// | knob      | values                                                   | distribution |
// |-----------|----------------------------------------------------------|--------------|
// | sba_width | 8, 16, 32 bits                                           | uniform      |
// | target    | a capability's first word, its second, both, a plain    | uniform      |
// |           |   word of a granule without a capability                 |              |
// | op        | read, write                                              | 50 : 50      |
//
// Every access must succeed (sberror 0: SBA is not capability-checked, REQ_CDM_06) and every read
// must return the bench's model of the memory: the software's line `EG2 DEBUG caps` gives the
// capabilities' first words (&plain_[i]) and their common second word, the plain words follow
// the software's formula, and each write updates the model. After the accesses every modelled
// word is read back once more, the mask of the capabilities written is written to the software's
// scratch word, and the loop is ended; the software then checks the tags (clear exactly where
// written, REQ_CMS_16). Sub-word data is right-aligned in sbdata0 (RISC-V Debug Specification
// 0.13.2, 3.12.23 sbdata0).
//
// Expected chip failure until ticket F23 is decided: SBA reaches main SRAM around cheriot_mem_sys,
// so a written capability keeps its tag and the software's tag check fails.
class chip_eg2_rv_dm_sba_cheriot_mode_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_sba_cheriot_mode_vseq)
  `uvm_object_new

  typedef enum int unsigned {TgtCapLo, TgtCapHi, TgtCapBoth, TgtPlain} sba_target_e;

  localparam int unsigned PlainWords = 32;
  localparam int unsigned MaxCaps = 16;

  rand int unsigned      n_caps;
  rand int unsigned      n_ops;
  rand sba_target_e      targets[];
  rand bit               writes[];
  rand sba_access_size_e widths[];
  rand bit [31:0]        wdata[];
  rand int unsigned      picks[];

  constraint ops_c {
    n_caps inside {[1:MaxCaps]};
    n_ops inside {[8:64]};
    targets.size() == n_ops;
    writes.size() == n_ops;
    widths.size() == n_ops;
    wdata.size() == n_ops;
    picks.size() == n_ops;
    foreach (targets[i]) {
      targets[i] dist {TgtCapLo := 25, TgtCapHi := 25, TgtCapBoth := 25, TgtPlain := 25};
      writes[i] dist {1 := 50, 0 := 50};
      widths[i] dist {SbaAccessSize8b := 1, SbaAccessSize16b := 1, SbaAccessSize32b := 1};
      picks[i] < 4096;
    }
  }

  // From the software's line.
  bit [31:0] caps_addr, plain_addr, meta;
  int unsigned n_caps_sw;
  // The model: the capabilities' two words each, the plain words; the capabilities written.
  bit [31:0] cap_words[2 * MaxCaps];
  bit [31:0] plain_words[PlainWords];
  bit [31:0] written_mask;

  virtual function void write_params();
    super.write_params();
    set_param("n_caps", n_caps);
  endfunction

  protected task sba_one(bit write, bit [31:0] addr, sba_access_size_e size, bit [31:0] data,
                         output bit [31:0] rdata);
    sba_access_item item = sba_access_item::type_id::create("item");
    item.bus_op = write ? BusOpWrite : BusOpRead;
    item.addr = addr;
    item.size = size;
    item.readonaddr = !write;
    item.readondata = 1'b0;
    item.autoincrement = 0;
    item.wdata.push_back(data);
    cfg.debugger.sba_access(item);
    if (item.timed_out) `uvm_fatal(`gfn, $sformatf("SBA access to 0x%08x timed out", addr))
    if (item.is_err != SbaErrNone) begin
      `uvm_error(`gfn, $sformatf({"SBA %s of 0x%08x (%s) answered sberror %s: SBA must not be ",
                                  "capability-checked (REQ_CDM_06)"}, write ? "write" : "read",
                                 addr, size.name(), item.is_err.name()))
    end
    rdata = item.rdata.size() > 0 ? item.rdata[0] : '0;
  endtask

  // Bytes of an access: 1, 2 or 4.
  protected function int unsigned nbytes(sba_access_size_e size);
    return size == SbaAccessSize8b ? 1 : size == SbaAccessSize16b ? 2 : 4;
  endfunction

  // One access to the modelled word `word` (by reference) at bus address `waddr`, at byte offset
  // `off` (aligned to the size).
  protected task access_word(string what, bit write, bit [31:0] waddr, int unsigned off,
                             sba_access_size_e size, bit [31:0] data, ref bit [31:0] word);
    int unsigned nb = nbytes(size);
    bit [31:0] mask = (nb == 4) ? 32'hffff_ffff : ((32'h1 << (8 * nb)) - 1);
    bit [31:0] rdata;
    sba_one(write, waddr + off, size, data & mask, rdata);
    if (write) begin
      word = (word & ~(mask << (8 * off))) | ((data & mask) << (8 * off));
      `uvm_info(`gfn, $sformatf("EG2 SBA cheriot: write %s 0x%08x %s 0x%0h", what, waddr + off,
                                size.name(), data & mask), UVM_MEDIUM)
    end else begin
      bit [31:0] exp = (word >> (8 * off)) & mask;
      if ((rdata & mask) != exp) begin
        `uvm_error(`gfn, $sformatf("SBA read of %s at 0x%08x (%s): 0x%0h, expected 0x%0h",
                                   what, waddr + off, size.name(), rdata & mask, exp))
      end
    end
  endtask

  virtual task body();
    string line;
    bit [31:0] n_sw, rdata;
    sba_access_err_e st;
    super.body();
    wait_sw_line("EG2 DEBUG caps", line);
    if (line == "") return;  // the software ended first (an _inject_fail build)
    if (!($sscanf(line, "EG2 DEBUG caps 0x%h 0x%h 0x%h 0x%h", caps_addr, n_sw, plain_addr, meta)
          == 4)) begin
      `uvm_fatal(`gfn, $sformatf("cannot read the capabilities' line '%s'", line))
    end
    if (n_sw != n_caps) begin
      `uvm_fatal(`gfn, $sformatf("software stored %0d capabilities, bench drew %0d", n_sw,
                                 n_caps))
    end
    for (int unsigned i = 0; i < n_caps; i++) begin
      cap_words[2 * i] = plain_addr + 4 * i;
      cap_words[2 * i + 1] = meta;
    end
    for (int unsigned j = 0; j < PlainWords; j++) begin
      plain_words[j] = (j * 32'h0101_0101) ^ 32'h5a5a_5a5a;
    end
    written_mask = '0;
    wait_ready();

    foreach (targets[i]) begin
      int unsigned nb = nbytes(widths[i]);
      int unsigned off = ((picks[i] >> 8) % (4 / nb)) * nb;
      int unsigned ci = picks[i] % n_caps;
      case (targets[i])
        TgtCapLo, TgtCapHi, TgtCapBoth: begin
          for (int h = 0; h < 2; h++) begin
            if (targets[i] == TgtCapLo && h == 1) continue;
            if (targets[i] == TgtCapHi && h == 0) continue;
            access_word($sformatf("capability %0d word %0d", ci, h), writes[i],
                        caps_addr + 8 * ci + 4 * h, off, widths[i], wdata[i] ^ h,
                        cap_words[2 * ci + h]);
          end
          if (writes[i]) written_mask[ci] = 1'b1;
        end
        default: begin
          int unsigned pj = picks[i] % PlainWords;
          access_word($sformatf("plain word %0d", pj), writes[i], plain_addr + 4 * pj, off,
                      widths[i], wdata[i], plain_words[pj]);
        end
      endcase
    end
    // Every modelled word read back once more.
    for (int unsigned w = 0; w < 2 * n_caps; w++) begin
      access_word($sformatf("capability %0d word %0d (final)", w / 2, w % 2), 1'b0,
                  caps_addr + 4 * w, 0, SbaAccessSize32b, '0, cap_words[w]);
    end
    for (int unsigned j = 0; j < PlainWords; j++) begin
      access_word($sformatf("plain word %0d (final)", j), 1'b0, plain_addr + 4 * j, 0,
                  SbaAccessSize32b, '0, plain_words[j]);
    end
    `uvm_info(`gfn, $sformatf("EG2 SBA cheriot: %0d accesses, capabilities written 0x%0h",
                              n_ops, written_mask), UVM_LOW)
    sba_wr32(scratch_addr, written_mask, st);
    `DV_CHECK_EQ(st, SbaErrNone, "the SBA write of the written-capability mask")
    sba_rd32(scratch_addr, rdata, st);
    `DV_CHECK_EQ(rdata, written_mask, "the written-capability mask read back")
    finish(5);
  endtask

endclass : chip_eg2_rv_dm_sba_cheriot_mode_vseq
