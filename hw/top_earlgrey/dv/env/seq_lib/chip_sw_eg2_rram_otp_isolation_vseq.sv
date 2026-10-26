// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_otp_isolation (sw/device/tests/eg2_rram/eg2_rram_otp_isolation.c; EG2 spec
// REQ_RRAM_16). The bench's half, the debugger's path: on
// `EG2 RRAM otp_isolation sba off=.. flag=..` an SBA read and an SBA write of that OTP-page host
// address must both end with an SBA error (the host window does not reach the OTP pages:
// programmers_guide.md "Memory-Mapped Reads via host_tl"; it is read-only besides), the RRAM row
// unchanged (backdoor), then 1 written over SBA to the software's flag.
class chip_sw_eg2_rram_otp_isolation_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_otp_isolation_vseq)

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  rand int unsigned n_accesses;
  rand int unsigned cfg_class;
  rand int unsigned w_path_host, w_path_read, w_path_write, w_path_sba;
  rand bit          sba;

  constraint knobs_c {
    n_accesses inside {[1:32]};
    cfg_class inside {[0:4]};
    w_path_host inside {[10:50]};
    w_path_read inside {[10:50]};
    w_path_write inside {[10:50]};
    w_path_sba inside {[5:30]};
    sba dist {1'b1 := 70, 1'b0 := 30};
  }

  virtual function void write_params();
    set_param("n_accesses", n_accesses);
    set_param("cfg_class", cfg_class);
    set_param("w_path_host", w_path_host);
    set_param("w_path_read", w_path_read);
    set_param("w_path_write", w_path_write);
    set_param("w_path_sba", w_path_sba);
    set_param("sba", sba);
  endfunction

  protected function bit [127:0] row(bit [31:0] off);
    return rram_ctrl_bkdr_util_pkg::rram_row_read(
        $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4));
  endfunction

  virtual task body();
    string line;
    bit dm_on;
    super.body();
    forever begin
      bit [31:0] addr, flag, data;
      bit [127:0] row_before;
      sba_access_err_e st;
      wait_sw_line("EG2 RRAM otp_isolation sba off=", line, 400_000_000, 1);
      if (line == "") return;
      if (!($sscanf(line, "EG2 RRAM otp_isolation sba off=0x%h flag=0x%h", addr, flag)
                      == 2)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 RRAM otp_isolation sba off=0x%%h flag=0x%%h\", addr, flag) == 2) %s", line))
      if (!dm_on) begin
        cfg.debugger.set_dmactive(1);
        dm_on = 1'b1;
      end
      row_before = row(addr - top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR);
      sba_rd32(addr, data, st);
      `DV_CHECK_NE(st, SbaErrNone, $sformatf("SBA read of OTP page 0x%0h succeeded (0x%0h)", addr,
                                             data))
      sba_wr32(addr, $urandom(), st);
      `DV_CHECK_NE(st, SbaErrNone, $sformatf("SBA write of OTP page 0x%0h succeeded", addr))
      `DV_CHECK_EQ(row(addr - top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR), row_before,
                   $sformatf("OTP page row at 0x%0h changed by SBA", addr))
      sba_wr32(flag, 1, st);
      `DV_CHECK_EQ(st, SbaErrNone, "SBA write of the software's flag")
    end
  endtask

endclass : chip_sw_eg2_rram_otp_isolation_vseq
