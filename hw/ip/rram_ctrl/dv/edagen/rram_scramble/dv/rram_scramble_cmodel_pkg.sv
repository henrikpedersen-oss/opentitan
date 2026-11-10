// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The scrambling unit bench's own part (edagen rram_scramble.yaml dv.files), on edagen's
// generated skeleton:
//   - rram_scramble_cmodel_vseq: constrained-random stimulus driving the unit's interface as
//     rram_phy's read and write paths do: two requesters, each with a mask (calc) and a cipher (op)
//     request held until acknowledged, random addresses, data, direction and cipher_switch, now and
//     then both at once; the key inputs: first a stretch on the reset keys, then keys_valid with new
//     keys, keys_disable (MuBi4True or an invalid value) switching to the random keys.
//   - rram_scramble_cmodel_scoreboard: every acknowledged mask and cipher result against the
//     scrambling reference model (eg2_rram_scramble_core.h through eg2_rram_dpi.cc), the model's key
//     registers stepped every cycle with the same inputs; an acknowledgement without a request, a
//     request waiting too long, arb_err; the functional coverage.
//   - the tests: rram_scramble_cmodel_random_test, and rram_scramble_cmodel_random_inject_fail_test,
//     which flips one bit of the first predicted mask and must fail.
// A request during which the keys change is not predicted (the documentation does not say which key
// a multi-cycle operation uses then); it is counted.
package rram_scramble_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import rram_ctrl_pkg::RndCnstAddrKeyDefault;
  import rram_ctrl_pkg::RndCnstDataKeyDefault;
  import rram_scramble_sc_agent_pkg::*;
  import rram_scramble_env_pkg::*;
  import rram_scramble_test_pkg::*;

  import "DPI-C" function chandle eg2_rram_scr_create(input bit [127:0] rst_addr_key,
                                                      input bit [127:0] rst_data_key);
  import "DPI-C" function void eg2_rram_scr_release(chandle h);
  import "DPI-C" function void eg2_rram_scr_step(chandle h, int unsigned keys_disable, int keys_valid,
      input bit [127:0] ak, input bit [127:0] dk, input bit [127:0] rak, input bit [127:0] rdk);
  import "DPI-C" function int unsigned eg2_rram_scr_epoch(chandle h);
  import "DPI-C" function void eg2_rram_scr_mask(chandle h, int unsigned addr, output bit [127:0] mask);
  import "DPI-C" function void eg2_rram_scr_op(chandle h, input bit [127:0] din, int decrypt,
                                               output bit [127:0] dout);

  localparam bit [3:0] MuBi4True = 4'h6, MuBi4False = 4'h9;
  localparam int unsigned MaxWait = 64;  // cycles a request may wait for its acknowledgement

  function automatic bit [127:0] rand128();
    case ($urandom_range(0, 19))
      0: return '0;
      1: return '1;
      default: return {$urandom(), $urandom(), $urandom(), $urandom()};
    endcase
  endfunction

  // ---------------------------------------------------------------------------------------------
  class rram_scramble_cmodel_vseq extends rram_scramble_base_vseq;
    `uvm_object_utils(rram_scramble_cmodel_vseq)

    int unsigned num_cycles = 20000;
    virtual rram_scramble_sc_if vif;

    bit [3:0]   keys_disable = MuBi4False;
    bit [127:0] addr_key, data_key, rand_addr_key, rand_data_key;
    bit [1:0]   calc_on, op_on, op_type, cswitch;
    bit [16:0]  addr[2];
    bit [127:0] data[2];
    bit [127:0] last_out[2];  // the last cipher result, to feed back the other way
    int unsigned busy_pct;    // how hard the requesters push, changes now and then

    function new(string name = "rram_scramble_cmodel_vseq");
      super.new(name);
    endfunction

    function automatic bit [16:0] rand_addr();
      case ($urandom_range(0, 9))
        0: return '0;
        1: return '1;
        2: return 17'(1 << $urandom_range(0, 16));
        default: return 17'($urandom());
      endcase
    endfunction

    task drive_keys(bit valid);
      vif.keys_disable <= keys_disable;
      vif.keys_valid <= valid;
      vif.addr_key <= addr_key;
      vif.data_key <= data_key;
      vif.rand_addr_key <= rand_addr_key;
      vif.rand_data_key <= rand_data_key;
    endtask

    task step(bit issue, bit allow_keys);
      bit valid = 0;
      @(posedge vif.clk);
      for (int k = 0; k < 2; k++) begin
        if (calc_on[k] && vif.calc_ack[k] === 1'b1) calc_on[k] = 0;
        if (op_on[k] && vif.op_ack[k] === 1'b1) begin
          op_on[k] = 0;
          last_out[k] = k ? vif.dout1 : vif.dout0;
        end
      end
      if ($urandom_range(0, 499) == 0) busy_pct = $urandom_range(5, 95);
      // Keys: mostly between operations, now and then in the middle of one.
      if (allow_keys && ($urandom_range(0, 399) == 0 ||
                         (calc_on == 0 && op_on == 0 && $urandom_range(0, 149) == 0))) begin
        case ($urandom_range(0, 9))
          0, 1, 2, 3: begin
            addr_key = rand128();
            data_key = rand128();
            valid = 1;
          end
          4: keys_disable = MuBi4True;
          5: do keys_disable = 4'($urandom()); while (keys_disable inside {MuBi4True, MuBi4False});
          6, 7: keys_disable = MuBi4False;
          default: begin
            rand_addr_key = rand128();
            rand_data_key = rand128();
          end
        endcase
      end
      for (int k = 0; k < 2; k++) begin
        if (issue && !calc_on[k] && $urandom_range(0, 99) < busy_pct) begin
          calc_on[k] = 1;
          addr[k] = rand_addr();
        end
        if (issue && !op_on[k] && $urandom_range(0, 99) < busy_pct) begin
          op_on[k] = 1;
          op_type[k] = $urandom_range(0, 1);
          cswitch[k] = $urandom_range(0, 1);
          // Half the time the last result back the other way (descramble(scramble(x))).
          data[k] = $urandom_range(0, 1) ? last_out[k] : rand128();
        end
      end
      drive_keys(valid);
      vif.calc_req <= calc_on;
      vif.op_req <= op_on;
      vif.op_type <= op_type;
      vif.cipher_switch <= cswitch;
      vif.addr0 <= addr[0];
      vif.addr1 <= addr[1];
      vif.data0 <= data[0];
      vif.data1 <= data[1];
    endtask

    virtual task body();
      int unsigned reset_stretch;
      vif = cfg.sc_cfg.vif;
      void'($value$plusargs("num_cycles=%d", num_cycles));
      rand_addr_key = rand128();
      rand_data_key = rand128();
      // Defined key inputs while the unit is still in reset (edagen's driver drives 0s, and
      // keys_disable 0 is not MuBi4False).
      @(posedge vif.clk);
      drive_keys(0);
      super.body();
      busy_pct = 50;
      // First the reset keys (RndCnstAddrKey / RndCnstDataKey), then everything.
      reset_stretch = $urandom_range(50, 300);
      repeat (reset_stretch) step(1, 0);
      repeat (num_cycles) step(1, 1);
      for (int i = 0; i < 1000 && (calc_on != 0 || op_on != 0); i++) step(0, 0);
      repeat (4) step(0, 0);
    endtask
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_scramble_cmodel_scoreboard extends rram_scramble_scoreboard;
    `uvm_component_utils(rram_scramble_cmodel_scoreboard)

    chandle h;
    bit inject_fail, injected;
    int unsigned calc_epoch[2], op_epoch[2], calc_wait[2], op_wait[2];
    bit calc_seen[2], op_seen[2], calc_back[2], op_back[2];
    int unsigned n_mask, n_op, n_unpred, n_err, n_checks, n_reset_key_ops, n_disabled_ops;
    int unsigned key_state;  // 0 reset keys, 1 loaded with keys_valid, 2 random keys (disable)

    // Coverage.
    int c_req, c_kind, c_dec, c_sw, c_both, c_lat, c_key, c_dis, c_back, c_pat;
    covergroup cg_op;
      cp_req: coverpoint c_req { bins r0 = {0}; bins r1 = {1}; }
      cp_kind: coverpoint c_kind { bins mask = {0}; bins cipher = {1}; }
      cp_dec: coverpoint c_dec iff (c_kind == 1) { bins scramble = {0}; bins descramble = {1}; }
      cp_sw: coverpoint c_sw iff (c_kind == 1) { bins straight = {0}; bins switched = {1}; }
      cp_both: coverpoint c_both { bins alone = {0}; bins contended = {1}; }
      cp_lat: coverpoint c_lat { bins l[] = {[1:4]}; bins l5_8 = {[5:8]}; bins l9_16 = {[9:16]};
                                 bins longer = {[17:$]}; }
      cp_key: coverpoint c_key { bins reset_keys = {0}; bins loaded = {1}; bins random_keys = {2}; }
      cp_back: coverpoint c_back { bins first = {0}; bins back_to_back = {1}; }
      x_req_kind_both: cross cp_req, cp_kind, cp_both;
      x_dec_sw_req: cross cp_dec, cp_sw, cp_req;
      x_kind_key: cross cp_kind, cp_key;
      x_kind_back: cross cp_kind, cp_back;
    endgroup
    covergroup cg_keys;  // key inputs each cycle
      cp_dis: coverpoint c_dis { bins f = {0}; bins t = {1}; bins invalid = {2}; }
      cp_valid: coverpoint c_pat { bins idle = {0}; bins load = {1}; }
      x_dis_valid: cross cp_dis, cp_valid;
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_op = new();
      cg_keys = new();
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      h = eg2_rram_scr_create(RndCnstAddrKeyDefault, RndCnstDataKeyDefault);
    endfunction

    function void expect_ok(bit ok, string what);
      n_checks++;
      if (!ok) begin
        n_err++;
        `uvm_error(get_type_name(), what)
      end
    endfunction

    function void sample(int k, int kind, int dec, int sw, int lat, bit both, bit back);
      c_req = k; c_kind = kind; c_dec = dec; c_sw = sw; c_lat = lat; c_both = both; c_key = key_state;
      c_back = back;
      if (cfg.en_cov) cg_op.sample();
    endfunction

    virtual function void predict_sc(rram_scramble_sc_item item);
      bit [127:0] exp, got;
      bit [16:0] a[2] = '{item.addr0, item.addr1};
      bit [127:0] d[2] = '{item.data0, item.data1};
      bit [127:0] m[2] = '{item.mask0, item.mask1};
      bit [127:0] o[2] = '{item.dout0, item.dout1};
      int unsigned ep = eg2_rram_scr_epoch(h);
      expect_ok(item.arb_err === 1'b0, "arb_err raised");
      for (int k = 0; k < 2; k++) begin
        // Mask requests.
        if (item.calc_req[k] === 1'b1) begin
          if (!calc_seen[k]) begin
            calc_seen[k] = 1;
            calc_epoch[k] = ep;
            calc_wait[k] = 0;
          end
          calc_wait[k]++;
          expect_ok(calc_wait[k] <= MaxWait, $sformatf("requester %0d: mask request waits more than %0d cycles", k, MaxWait));
        end
        if (item.calc_ack[k] === 1'b1) begin
          expect_ok(item.calc_req[k] === 1'b1, $sformatf("requester %0d: mask acknowledged without a request", k));
          if (item.calc_req[k] === 1'b1) begin
            if (calc_epoch[k] != ep) begin
              n_unpred++;
            end else begin
              eg2_rram_scr_mask(h, a[k], exp);
              if (inject_fail && !injected) begin
                exp[0] = !exp[0];
                injected = 1;
                `uvm_info(get_type_name(), "inject_fail: one bit of a predicted mask flipped", UVM_LOW)
              end
              n_mask++;
              if (key_state == 0) n_reset_key_ops++;
              expect_ok(m[k] === exp, $sformatf("requester %0d: mask of address %05h is %032h, the model's %032h",
                                                k, a[k], m[k], exp));
              sample(k, 0, 0, 0, calc_wait[k], item.calc_req == 2'b11, calc_back[k]);
            end
          end
          calc_seen[k] = 0;
          calc_back[k] = 1;
        end else if (item.calc_req[k] !== 1'b1) begin
          calc_back[k] = 0;
        end
        // Cipher requests.
        if (item.op_req[k] === 1'b1) begin
          if (!op_seen[k]) begin
            op_seen[k] = 1;
            op_epoch[k] = ep;
            op_wait[k] = 0;
          end
          op_wait[k]++;
          expect_ok(op_wait[k] <= MaxWait, $sformatf("requester %0d: cipher request waits more than %0d cycles", k, MaxWait));
        end
        if (item.op_ack[k] === 1'b1) begin
          expect_ok(item.op_req[k] === 1'b1, $sformatf("requester %0d: cipher acknowledged without a request", k));
          if (item.op_req[k] === 1'b1) begin
            if (op_epoch[k] != ep) begin
              n_unpred++;
            end else begin
              eg2_rram_scr_op(h, d[k], item.op_type[k], exp);
              n_op++;
              if (key_state == 0) n_reset_key_ops++;
              if (key_state == 2) n_disabled_ops++;
              expect_ok(o[k] === exp, $sformatf("requester %0d: %s of %032h (cipher_switch %b) is %032h, the model's %032h",
                                                k, item.op_type[k] ? "descramble" : "scramble", d[k],
                                                item.cipher_switch[k], o[k], exp));
              sample(k, 1, item.op_type[k], item.cipher_switch[k], op_wait[k], item.op_req == 2'b11, op_back[k]);
            end
          end
          op_seen[k] = 0;
          op_back[k] = 1;
        end else if (item.op_req[k] !== 1'b1) begin
          op_back[k] = 0;
        end
      end
      // The key registers take this cycle's inputs at the clock edge that ends it.
      c_dis = item.keys_disable == MuBi4False ? 0 : (item.keys_disable == MuBi4True ? 1 : 2);
      c_pat = item.keys_valid === 1'b1;
      if (cfg.en_cov) cg_keys.sample();
      if (item.keys_disable != MuBi4False) key_state = 2;
      else if (item.keys_valid === 1'b1) key_state = 1;
      eg2_rram_scr_step(h, item.keys_disable, item.keys_valid === 1'b1, item.addr_key, item.data_key,
                        item.rand_addr_key, item.rand_data_key);
    endfunction

    virtual function void check_phase(uvm_phase phase);
      super.check_phase(phase);
      if (n_mask < 100 || n_op < 100 || n_reset_key_ops < 5 || n_disabled_ops < 5)
        `uvm_error(get_type_name(), $sformatf("too little checked: %0d masks, %0d cipher results, %0d on the reset keys, %0d on the random keys",
                                              n_mask, n_op, n_reset_key_ops, n_disabled_ops))
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: nothing was flipped")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      super.report_phase(phase);
      `uvm_info(get_type_name(), $sformatf({"checked against the model: %0d masks, %0d cipher results ",
          "(%0d on the reset keys, %0d on the random keys), %0d not predicted (keys changed during the ",
          "operation), %0d checks, %0d failed"}, n_mask, n_op, n_reset_key_ops, n_disabled_ops, n_unpred,
          n_checks, n_err), UVM_LOW)
      if (h != null) eg2_rram_scr_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_scramble_cmodel_random_test extends rram_scramble_base_test;
    `uvm_component_utils(rram_scramble_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      rram_scramble_scoreboard::type_id::set_type_override(rram_scramble_cmodel_scoreboard::get_type());
      test_seq = "rram_scramble_cmodel_vseq";
      super.build_phase(phase);
    endfunction
  endclass

  // Must fail: the scoreboard flips one bit of the first predicted mask.
  class rram_scramble_cmodel_random_inject_fail_test extends rram_scramble_cmodel_random_test;
    `uvm_component_utils(rram_scramble_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass
endpackage
