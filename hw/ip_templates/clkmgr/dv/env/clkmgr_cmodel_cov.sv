// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of what the clock manager reference model checked (clkmgr_cmodel_sb): the
// register writes with their access context, the input values the documents name and the others,
// the reads by how exactly the model predicted them, and each output the model predicted exactly.
class clkmgr_cmodel_cov extends uvm_object;
  `uvm_object_utils(clkmgr_cmodel_cov)

  // A mubi4 or lc_tx value: 0 true, 1 false, 2 any other.
  typedef enum bit [1:0] {VTrue = 0, VFalse = 1, VOther = 2} val_e;

  protected int unsigned w_addr, w_data;
  protected bit          w_regwen;  // the register's write enable, where it has one
  protected bit          w_sel;     // EXTCLK_CTRL.SEL selected before the write
  protected val_e        w_mubi;    // the written mubi4 field (SEL, VAL, EN)
  protected val_e        w_hispeed;
  protected val_e        i_idle[4], i_scan, i_debug, i_lcreq, i_ioack, i_allack, i_step, i_calib;
  protected bit [2:0]    i_en;
  protected int unsigned r_addr;
  protected bit [1:0]    r_kind;    // 0 exact, 1 with open bits, 2 all open
  protected int unsigned o_bit;
  protected bit          o_val;

  covergroup write_cg;
    addr_cp: coverpoint w_addr {
      bins regs[] = {'h04, 'h08, 'h10, 'h14, 'h18, 'h1c, 'h24, 'h28, 'h2c, 'h30, 'h34, 'h38,
                     'h3c, 'h40, 'h44, 'h48};
    }
    regwen_cp: coverpoint w_regwen;
    // The external clock selection: the documented transitions and "no impact" writes.
    extclk_cp: coverpoint {w_sel, w_mubi} iff (w_addr == 'h08) {
      // {sel before, written value}: 0 true, 1 false, 2 other.
      bins select   = {3'b000};
      bins deselect = {3'b101};
      bins keep_off = {3'b001, 3'b010};
      bins keep_on  = {3'b100, 3'b110};
    }
    hispeed_cp: coverpoint w_hispeed iff (w_addr == 'h08);
    jitter_cp: coverpoint w_mubi iff (w_addr == 'h14);
    meas_en_cp: coverpoint w_mubi iff (w_addr inside {'h28, 'h30, 'h38, 'h40});
    addr_x_regwen: cross addr_cp, regwen_cp {
      ignore_bins no_regwen = binsof(addr_cp) intersect {'h04, 'h10, 'h18, 'h1c, 'h24, 'h48};
    }
  endgroup

  covergroup inputs_cg;
    idle_aes_cp:  coverpoint i_idle[0];
    idle_hmac_cp: coverpoint i_idle[1];
    idle_kmac_cp: coverpoint i_idle[2];
    idle_otbn_cp: coverpoint i_idle[3];
    scan_cp:      coverpoint i_scan;
    debug_cp:     coverpoint i_debug;
    lcreq_cp:     coverpoint i_lcreq;
    ioack_cp:     coverpoint i_ioack;
    allack_cp:    coverpoint i_allack;
    step_cp:      coverpoint i_step;
    calib_cp:     coverpoint i_calib;
    en_cp:        coverpoint i_en;
    lc_switch:    cross lcreq_cp, ioack_cp;
  endgroup

  covergroup read_cg;
    addr_cp: coverpoint r_addr {
      bins regs[] = {'h00, 'h04, 'h08, 'h0c, 'h10, 'h14, 'h18, 'h1c, 'h20, 'h24, 'h28, 'h2c,
                     'h30, 'h34, 'h38, 'h3c, 'h40, 'h44, 'h48, 'h4c};
    }
    kind_cp: coverpoint r_kind {
      bins exact = {0};
      bins partly_open = {1};
      bins open = {2};
    }
    addr_x_kind: cross addr_cp, kind_cp;
  endgroup

  // Each output, predicted exactly to each value.
  covergroup output_cg;
    bit_cp: coverpoint o_bit {
      bins b[] = {[0 : ClkONumOutputs - 1]};
    }
    val_cp: coverpoint o_val;
    bit_x_val: cross bit_cp, val_cp {
      // hi_speed_sel_o is never predicted (not documented).
      ignore_bins hispeed = binsof(bit_cp) intersect {ClkOHiSpeedTrue, ClkOHiSpeedFalse};
    }
  endgroup

  function new(string name = "clkmgr_cmodel_cov");
    super.new(name);
    write_cg = new();
    inputs_cg = new();
    read_cg = new();
    output_cg = new();
  endfunction

  function void reset();
  endfunction

  protected static function val_e mubi_val(bit [3:0] v);
    return v == prim_mubi_pkg::MuBi4True ? VTrue : v == prim_mubi_pkg::MuBi4False ? VFalse : VOther;
  endfunction
  protected static function val_e lc_val(bit [3:0] v);
    return v == lc_ctrl_pkg::On ? VTrue : v == lc_ctrl_pkg::Off ? VFalse : VOther;
  endfunction

  function void sample_write(int unsigned addr, int unsigned data, int unsigned extclk_regwen,
                             int unsigned jitter_regwen, int unsigned meas_regwen,
                             int unsigned sel);
    w_addr = addr;
    w_data = data;
    w_sel = sel[0];
    w_mubi = mubi_val(data[3:0]);
    w_hispeed = mubi_val(data[7:4]);
    case (addr)
      'h08: w_regwen = extclk_regwen[0];
      'h14: w_regwen = jitter_regwen[0];
      'h28, 'h2c, 'h30, 'h34, 'h38, 'h3c, 'h40, 'h44: w_regwen = meas_regwen[0];
      default: w_regwen = 1'b1;
    endcase
    write_cg.sample();
  endfunction

  function void sample_inputs(bit [95:0] in);
    for (int i = 0; i < 4; i++) i_idle[i] = mubi_val(in[4 * i +: 4]);
    i_scan   = mubi_val(in[19:16]);
    i_debug  = lc_val(in[23:20]);
    i_lcreq  = lc_val(in[27:24]);
    i_ioack  = mubi_val(in[31:28]);
    i_allack = mubi_val(in[35:32]);
    i_step   = mubi_val(in[39:36]);
    i_calib  = mubi_val(in[43:40]);
    i_en     = in[46:44];
    inputs_cg.sample();
  endfunction

  function void sample_read(int unsigned addr, int unsigned data, int unsigned lo,
                            int unsigned hi);
    r_addr = addr;
    r_kind = lo == hi ? 0 : (lo == 0 && hi == '1) ? 2 : 1;
    read_cg.sample();
  endfunction

  function void sample_outputs(int unsigned lo, int unsigned hi, int unsigned obs,
                               int unsigned mask);
    for (int i = 0; i < ClkONumOutputs; i++) begin
      if (!mask[i] || (hi[i] && !lo[i])) continue;
      o_bit = i;
      o_val = lo[i];
      output_cg.sample();
    end
  endfunction
endclass
