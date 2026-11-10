// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of rstmgr's documented behaviour, sampled by rstmgr_cmodel_sb (with
// cfg.en_cov): the reset categories and requests pwrmgr gives, the RESET_INFO causes read back,
// the crash dump controls and their locks, RESET_REQ, and the software-controlled resets and their
// locks.
class rstmgr_reg_cov extends uvm_object;
  `uvm_object_utils(rstmgr_reg_cov)

  // A reset as pwrmgr requests it: its category and requests, lc / sys trees per domain.
  covergroup reset_req_cg with function sample(bit [1:0] cause, bit [4:0] rstreqs, bit [1:0] lc,
                                               bit [1:0] sys);
    cp_cause: coverpoint cause {
      bins none = {0};
      bins low_power = {1};
      bins hw_req = {2};
      bins undefined = {3};
    }
    cp_req: coverpoint rstreqs {
      bins sysrst_ctrl = {5'b00001};
      bins aon_timer = {5'b00010};
      bins main_pwr = {5'b00100};
      bins esc = {5'b01000};
      bins ndm = {5'b10000};
      bins several = {3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24,
                       25, 26, 27, 28, 29, 30, 31};  // two requests or more
      bins none = {0};
    }
    cp_lc: coverpoint lc {bins main_only = {2'b10}; bins both = {2'b11};}
    cp_sys: coverpoint sys {bins none = {2'b00}; bins main_only = {2'b10}; bins both = {2'b11};}
    cause_x_req: cross cp_cause, cp_req {
      ignore_bins no_req_bits = binsof(cp_cause.low_power) && !binsof(cp_req.none);
    }
    lc_x_sys: cross cp_lc, cp_sys;
  endgroup

  // RESET_INFO read back: each cause seen set, and several at once.
  covergroup reset_info_cg with function sample(bit [7:0] info);
    cp_por: coverpoint info[0];
    cp_low_power_exit: coverpoint info[1];
    cp_sw_reset: coverpoint info[2];
    cp_hw_req: coverpoint info[7:3] {
      wildcard bins b3 = {5'b????1};
      wildcard bins b4 = {5'b???1?};
      wildcard bins b5 = {5'b??1??};
      wildcard bins b6 = {5'b?1???};
      wildcard bins b7 = {5'b1????};
      bins none = {0};
    }
    cp_several: coverpoint $countones(info) {bins one = {1}; bins many = {[2:8]};}
  endgroup

  // Writes to the locked controls: crash dump controls, software resets; RESET_REQ values.
  covergroup ctrl_write_cg with function sample(int unsigned addr, bit [31:0] data, bit regwen);
    cp_reg: coverpoint addr {
      bins reset_req = {'h04};
      bins reset_info_clear = {'h08};
      bins alert_regwen = {'h0c};
      bins alert_ctrl = {'h10};
      bins cpu_regwen = {'h1c};
      bins cpu_ctrl = {'h20};
      bins sw_regwen = {['h2c:'h50]};
      bins sw_ctrl = {['h54:'h78]};
    }
    cp_regwen: coverpoint regwen;
    cp_en: coverpoint data[0];
    reg_x_regwen: cross cp_reg, cp_regwen {
      ignore_bins no_lock = binsof(cp_reg) intersect {'h04, 'h08, 'h0c, 'h1c, ['h2c:'h50]};
    }
    reg_x_en: cross cp_reg, cp_en;
  endgroup

  covergroup reset_req_val_cg with function sample(bit [3:0] v);
    cp_val: coverpoint v {
      bins true_ = {prim_mubi_pkg::MuBi4True};
      bins false_ = {prim_mubi_pkg::MuBi4False};
      bins other = default;
    }
  endgroup

  // The software-controllable resets: each held and released (read back while settled).
  covergroup sw_rst_cg with function sample(int unsigned idx, bit val, bit regwen);
    cp_idx: coverpoint idx {bins i[] = {[0:9]};}
    cp_val: coverpoint val;
    cp_regwen: coverpoint regwen;
    idx_x_val: cross cp_idx, cp_val;
    idx_x_regwen: cross cp_idx, cp_regwen;
  endgroup

  // Crash dump reads: each INDEX of each dump read back while settled.
  covergroup dump_read_cg with function sample(bit alert, int unsigned idx);
    cp_which: coverpoint alert;
    cp_idx: coverpoint idx {bins i[] = {[0:15]};}
    which_x_idx: cross cp_which, cp_idx;
  endgroup

  // A settled state: in reset or not, and the reset that last ended.
  covergroup settled_cg with function sample(bit por, bit in_reset, bit [1:0] cause);
    cp_por: coverpoint por;
    cp_in_reset: coverpoint in_reset;
    cp_cause: coverpoint cause;
  endgroup

  protected bit [1:0] cause_now;
  protected bit       sw_regwen[10];
  protected bit [3:0] alert_idx, cpu_idx;

  function new(string name = "");
    super.new(name);
    reset_req_cg = new();
    reset_info_cg = new();
    ctrl_write_cg = new();
    reset_req_val_cg = new();
    sw_rst_cg = new();
    dump_read_cg = new();
    settled_cg = new();
    reset();
  endfunction

  function void reset();
    foreach (sw_regwen[i]) sw_regwen[i] = 1'b1;
    alert_idx = 0;
    cpu_idx = 0;
  endfunction

  // in: rstmgr_cmodel_sb's input layout: por 0, scan 1, lc 3:2, sys 5:4, rstreqs 10:6, cause 12:11.
  function void sample_ports(bit [31:0] prev, bit [31:0] now);
    bit [1:0] lc = now[3:2], sys = now[5:4];
    if ((lc | sys) != 0 && (prev[3:2] | prev[5:4]) == 0 || now[10:6] != prev[10:6]) begin
      if ((lc | sys) != 0) reset_req_cg.sample(now[12:11], now[10:6], lc, sys);
    end
    if ((lc | sys) != 0) cause_now = now[12:11];
  endfunction

  function void sample_settled(bit [31:0] now);
    settled_cg.sample(now[0], (now[3:2] | now[5:4]) != 0, cause_now);
  endfunction

  function void sample_write(int unsigned addr, bit [31:0] data, int unsigned alert_regwen,
                             int unsigned cpu_regwen);
    bit regwen = 1'b1;
    if (addr == 'h10) regwen = alert_regwen[0];
    if (addr == 'h20) regwen = cpu_regwen[0];
    if (addr >= 'h54 && addr < 'h7c) regwen = sw_regwen[(addr - 'h54) / 4];
    if (addr >= 'h2c && addr < 'h54 && !data[0]) sw_regwen[(addr - 'h2c) / 4] = 1'b0;
    if (addr == 'h10 && regwen) alert_idx = data[7:4];
    if (addr == 'h20 && regwen) cpu_idx = data[7:4];
    ctrl_write_cg.sample(addr, data, regwen);
    if (addr == 'h04) reset_req_val_cg.sample(data[3:0]);
  endfunction

  function void sample_read(int unsigned addr, bit [31:0] data, bit quiet);
    if (!quiet) return;
    if (addr == 'h08) reset_info_cg.sample(data[7:0]);
    if (addr >= 'h54 && addr < 'h7c) begin
      sw_rst_cg.sample((addr - 'h54) / 4, data[0], sw_regwen[(addr - 'h54) / 4]);
    end
    if (addr == 'h18) dump_read_cg.sample(1'b1, alert_idx);
    if (addr == 'h28) dump_read_cg.sample(1'b0, cpu_idx);
  endfunction
endclass
