// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_host_wr_reject (sw/device/tests/eg2_rram/eg2_rram_host_wr_reject.c; EG2 spec
// REQ_RRAM_03, "RRAM controller in RISC-V mode"). The knobs under the spec's distributions, and
// the debugger's half of the requirement: on `EG2 RRAM sba <addr> <flag>` an SBA write to that
// RRAM host-window word must end with an SBA error (host_tl is read-only: RFC RRAM §3.1 p6), the
// RRAM row must be unchanged (backdoor) and an SBA read of the word must return what the row
// holds; then 1 is written over SBA to the software's flag, and the software reads the word again.
class chip_sw_eg2_rram_host_wr_reject_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_host_wr_reject_vseq)

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  rand int unsigned n_writes;
  rand int unsigned w_width_byte, w_width_half, w_width_word;
  rand int unsigned w_addr_first, w_addr_last, w_addr_edge, w_addr_random;
  rand bit          in_image;
  rand bit          sba;

  constraint knobs_c {
    n_writes dist {[1:8] :/ 20, [9:64] :/ 60, [65:256] :/ 20};
    // Width uniform (spec: "access (fetch, load, host write) uniform"), +-50 % per seed.
    w_width_byte inside {[17:50]};
    w_width_half inside {[17:50]};
    w_width_word inside {[17:50]};
    // Address classes: the boundary classes at least 60 % of the weight, random at least 5 %.
    w_addr_first inside {[1:100]};
    w_addr_last inside {[1:100]};
    w_addr_edge inside {[1:100]};
    w_addr_random inside {[1:100]};
    (w_addr_first + w_addr_last + w_addr_edge) * 10 >=
        (w_addr_first + w_addr_last + w_addr_edge + w_addr_random) * 6;
    w_addr_random * 20 >= (w_addr_first + w_addr_last + w_addr_edge + w_addr_random);
    in_image dist {1'b1 := 50, 1'b0 := 50};
    sba dist {1'b1 := 80, 1'b0 := 20};
  }

  virtual function void write_params();
    set_param("n_writes", n_writes);
    set_param("w_width_byte", w_width_byte);
    set_param("w_width_half", w_width_half);
    set_param("w_width_word", w_width_word);
    set_param("w_addr_first", w_addr_first);
    set_param("w_addr_last", w_addr_last);
    set_param("w_addr_edge", w_addr_edge);
    set_param("w_addr_random", w_addr_random);
    set_param("in_image", in_image);
    set_param("sba", sba);
  endfunction

  protected function bit [127:0] row(bit [31:0] off);
    return rram_ctrl_bkdr_util_pkg::rram_row_read(
        $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4));
  endfunction

  virtual task body();
    string line;
    bit [31:0] addr, flag, data, off;
    bit [127:0] row_before;
    sba_access_err_e st;
    super.body();
    wait_sw_line("EG2 RRAM sba 0x", line, 400_000_000);
    if (line == "") return;
    if (!($sscanf(line, "EG2 RRAM sba 0x%h 0x%h", addr, flag) == 2)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 RRAM sba 0x%%h 0x%%h\", addr, flag) == 2) %s", line))
    off = addr - top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR;
    cfg.debugger.set_dmactive(1);
    row_before = row(off);
    sba_wr32(addr, ~row_before[32 * off[3:2] +: 32], st);
    `DV_CHECK_NE(st, SbaErrNone, $sformatf("SBA write to the RRAM host window 0x%0h succeeded",
                                           addr))
    `DV_CHECK_EQ(row(off), row_before, $sformatf("RRAM row at 0x%0h changed by an SBA write", addr))
    sba_rd32(addr, data, st);
    `DV_CHECK_EQ(st, SbaErrNone, $sformatf("SBA read of the RRAM host window 0x%0h", addr))
    sba_wr32(flag, 1, st);
    `DV_CHECK_EQ(st, SbaErrNone, "SBA write of the software's flag")
  endtask

endclass : chip_sw_eg2_rram_host_wr_reject_vseq
