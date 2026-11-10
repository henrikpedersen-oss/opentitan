// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The memory-protection unit bench's own part (edagen rram_ctrl_mp.yaml dv.files), on edagen's
// generated skeleton:
//   - rram_ctrl_mp_cmodel_vseq: constrained-random stimulus. It drives the unit's interface
//     directly, as rram_ctrl drives the unit: random register configurations (MP regions,
//     DEFAULT_REGION, info pages, emulated info windows and subregions with their lock chain,
//     HW_INFO_CFG_OVERRIDE), life-cycle enables, requester, phase and disables; a controller
//     requester that holds its request until done (rram_ctrl_rd / rram_ctrl_wr), a host requester
//     with at most two reads outstanding (tlul_adapter_sram, Outstanding = NumOutstandingRdReq), and
//     a PHY that grants, accepts and answers in order (theory_of_operation.md "RRAM Phy": rram_disable
//     suppresses new requests). The configuration changes only while no controller request is open.
//   - rram_ctrl_mp_cmodel_scoreboard: every cycle against the memory-protection reference model
//     (eg2_rram_mp_core.h through eg2_rram_dpi.cc), see predict_mp(); the functional coverage.
//   - the tests: rram_ctrl_mp_cmodel_random_test, and rram_ctrl_mp_cmodel_random_inject_fail_test,
//     which inverts the first definite forwarding prediction and must fail.
//
// Interface timing the bench assumes (the unit's port contract; the documentation describes the
// block, not this internal interface): the decision outputs (ctrl_rd/wr/req/scr/ecc/xor, host_req,
// host_scr/ecc) follow the request in the same cycle; a refused controller request's error and done
// come the next cycle; a refused host read is answered with an error in order with the PHY's
// answers. Life-cycle enables pass a synchroniser: a page they gate is not predicted until the
// enable has been steady for four cycles.
package rram_ctrl_mp_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import rram_ctrl_mp_mp_agent_pkg::*;
  import rram_ctrl_mp_env_pkg::*;
  import rram_ctrl_mp_test_pkg::*;

  import "DPI-C" function chandle eg2_rram_mp_create();
  import "DPI-C" function void eg2_rram_mp_release(chandle h);
  import "DPI-C" function void eg2_rram_mp_set(chandle h, int kind, int idx, int unsigned value);
  import "DPI-C" function void eg2_rram_mp_ctrl(chandle h, int unsigned sel, int unsigned phase,
      int unsigned part, int unsigned page, int write, int ovfl, int unsigned rram_disable,
      output int err, output int fwd, output int scr, output int ecc, output int xr,
      output int rule, output int idx);
  import "DPI-C" function void eg2_rram_mp_host(chandle h, int unsigned page,
      int unsigned sw_disable, int unsigned rram_disable, output int err, output int fwd,
      output int scr, output int ecc, output int rule, output int idx);

  localparam bit [3:0] MuBi4True = 4'h6, MuBi4False = 4'h9;
  localparam bit [3:0] LcOn = 4'h5, LcOff = 4'ha;
  localparam int No = 0, Yes = 1, Unpred = 2;
  // RramMp::Rule
  localparam int RuleNone = 0, RuleOtpExcl = 1, RuleSubregion = 2, RuleWindowDeny = 3,
                 RuleMpRegion = 4, RuleDefault = 5, RuleInfoPage = 6, RuleInfoLc = 7,
                 RuleLcMgr = 8, RuleOtpPlug = 9, RuleLoopBack = 10, RuleOutOfRange = 11,
                 RuleUnpred = 12;

  // A MuBi4 value: true, false, or (pinv percent) one of the 14 invalid encodings.
  function automatic bit [3:0] rand_mubi(int pinv = 10);
    int r = $urandom_range(0, 99);
    bit [3:0] v;
    if (r < pinv) begin
      do v = 4'($urandom()); while (v == MuBi4True || v == MuBi4False);
      return v;
    end
    return (r < pinv + (100 - pinv) / 2) ? MuBi4True : MuBi4False;
  endfunction

  function automatic bit [3:0] rand_lc();
    int r = $urandom_range(0, 99);
    bit [3:0] v;
    if (r < 5) begin
      do v = 4'($urandom()); while (v == LcOn || v == LcOff);
      return v;
    end
    return r < 55 ? LcOn : LcOff;
  endfunction

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_mp_cmodel_vseq extends rram_ctrl_mp_base_vseq;
    `uvm_object_utils(rram_ctrl_mp_cmodel_vseq)

    int unsigned num_cycles = 20000;
    virtual rram_ctrl_mp_mp_if vif;

    // The configuration being driven.
    bit [3:0]   lc[4];
    bit [239:0] region;
    bit [199:0] region_cfg;
    bit [15:0]  default_cfg;
    bit [159:0] info_cfg;
    bit [1:0]   emul_regwen;
    bit [55:0]  emul_region;
    bit [23:0]  sub_regwen;
    bit [191:0] sub_top;
    bit [383:0] sub_cfg;
    bit [3:0]   rram_disable, sw_disable, scr_dis, ecc_dis;
    bit [2:0]   if_sel;
    bit [1:0]   phase;
    bit         small_world;  // bases and pages in 0..127, so rules overlap and abut

    // Requesters and the PHY.
    bit          ctrl_busy, ctrl_wr, ctrl_part, ctrl_ovfl;
    bit [18:0]   ctrl_addr;
    bit          phy_ctrl_pending, phy_ctrl_wr;
    int unsigned phy_ctrl_cnt;
    bit          host_on;
    bit [18:0]   host_addr;
    int unsigned host_out;
    int unsigned phy_due[$];  // cycle numbers of the PHY's host answers, in order
    int unsigned cycle;

    function new(string name = "rram_ctrl_mp_cmodel_vseq");
      super.new(name);
    endfunction

    function automatic int unsigned rand_base();
      return small_world ? $urandom_range(0, 127) :
             ($urandom_range(0, 3) == 0 ? $urandom_range(4050, 4095) : $urandom_range(0, 4095));
    endfunction
    function automatic int unsigned rand_size();
      int r = $urandom_range(0, 9);
      return r < 2 ? 0 : (r < 8 ? $urandom_range(1, 31) : $urandom_range(0, 4095));
    endfunction

    // An address on a page the configuration makes interesting: region and window edges, just
    // outside them, the OTP boundary, the partition ends, random.
    function automatic int unsigned pick_page(bit part);
      int unsigned b, s, i;
      if (part) return $urandom_range(0, 99) < 90 ? $urandom_range(0, 7) : $urandom_range(8, 4095);
      case ($urandom_range(0, 11))
        0, 1: begin
          i = $urandom_range(0, 9);
          b = region[24*i +: 12];
          s = region[24*i+12 +: 12];
          case ($urandom_range(0, 3))
            0: return b;
            1: return (b + s) % 4096;
            2: return (b + 4095) % 4096;
            default: return (b + s + 1) % 4096;
          endcase
        end
        2, 3: begin
          i = $urandom_range(0, 1);
          b = emul_region[28*i +: 12];
          s = emul_region[28*i+12 +: 12];
          case ($urandom_range(0, 3))
            0: return (b + sub_top[8*(12*i + $urandom_range(0, 11)) +: 8]) % 4096;
            1: return (b + sub_top[8*(12*i + $urandom_range(0, 11)) +: 8] + 1) % 4096;
            2: return (b + $urandom_range(0, s > 40 ? 40 : s)) % 4096;
            default: return (b + s + $urandom_range(0, 1)) % 4096;
          endcase
        end
        4: return $urandom_range(4086, 4095);
        5: return $urandom_range(0, 1) ? 0 : 4095;
        6, 7, 8: return small_world ? $urandom_range(0, 160) : $urandom_range(0, 4095);
        default: return $urandom_range(0, 4095);
      endcase
    endfunction

    function automatic bit [19:0] rand_cfg20();  // EN, RD, WR, SCRAMBLE, ECC
      return {rand_mubi(), rand_mubi(), rand_mubi(), rand_mubi(), rand_mubi()};
    endfunction

    function void rand_regions();
      for (int i = 0; i < 10; i++) begin
        region[24*i +: 24] = {12'(rand_size()), 12'(rand_base())};
        region_cfg[20*i +: 20] = rand_cfg20();
      end
      default_cfg = {rand_mubi(), rand_mubi(), rand_mubi(), rand_mubi()};
    endfunction

    function void rand_info();
      for (int i = 0; i < 8; i++) info_cfg[20*i +: 20] = rand_cfg20();
      scr_dis = rand_mubi(15);
      ecc_dis = rand_mubi(15);
    endfunction

    // Windows and subregions: mostly configured as the programmer's guide says (increasing TOP,
    // locked in order from the first), sometimes not.
    function void rand_windows();
      for (int w = 0; w < 2; w++) begin
        int unsigned top = 0, nlock = $urandom_range(0, 1) ? 12 : $urandom_range(0, 12);
        emul_region[28*w +: 28] = {rand_mubi(10), 12'($urandom_range(0, 3) ? $urandom_range(8, 63) : rand_size() % 48),
                                   12'(rand_base())};
        emul_regwen[w] = $urandom_range(0, 3) == 0;
        for (int j = 0; j < 12; j++) begin
          int k = 12 * w + j;
          if ($urandom_range(0, 4) == 0) top = $urandom_range(0, 60);
          else top = top + $urandom_range(0, 4);
          sub_top[8*k +: 8] = 8'(top);
          sub_regwen[k] = !(j < nlock) ^ ($urandom_range(0, 19) == 0);
          sub_cfg[16*k +: 16] = {rand_mubi(), rand_mubi(), rand_mubi(), rand_mubi()};
        end
      end
      // Now and then the second window on top of the first.
      if ($urandom_range(0, 4) == 0) emul_region[28 +: 12] = emul_region[0 +: 12] + $urandom_range(0, 8);
    endfunction

    function void rand_requester();
      int r = $urandom_range(0, 99);
      if_sel = r < 50 ? 3'd1 : r < 65 ? 3'd2 : r < 82 ? 3'd3 : r < 92 ? 3'd4 :
               3'($urandom_range(0, 1) ? 0 : $urandom_range(5, 7));
      phase = if_sel == 3'd3 ? 2'($urandom()) : ($urandom_range(0, 49) == 0 ? 2'($urandom()) : 2'd3);
    endfunction

    function void rand_disables();
      int r = $urandom_range(0, 99);
      rram_disable = r < 90 ? MuBi4False : (r < 95 ? MuBi4True : rand_mubi(100));
      r = $urandom_range(0, 99);
      sw_disable = r < 85 ? MuBi4False : (r < 95 ? MuBi4True : rand_mubi(100));
    endfunction

    function void rand_all();
      small_world = $urandom_range(0, 2) != 0;
      rand_regions();
      rand_info();
      rand_windows();
      rand_requester();
      rand_disables();
      foreach (lc[i]) lc[i] = rand_lc();
    endfunction

    // One configuration change: a field group, or everything.
    function void mutate();
      case ($urandom_range(0, 9))
        0: rand_all();
        1, 2: rand_regions();
        3: rand_info();
        4, 5: rand_windows();
        6, 7: rand_requester();
        8: rand_disables();
        default: lc[$urandom_range(0, 3)] = rand_lc();
      endcase
    endfunction

    task drive_cfg();
      vif.lc_creator <= lc[0];
      vif.lc_owner <= lc[1];
      vif.lc_iso_wr <= lc[2];
      vif.lc_iso_rd <= lc[3];
      vif.region <= region;
      vif.region_cfg <= region_cfg;
      vif.default_cfg <= default_cfg;
      vif.info_cfg <= info_cfg;
      vif.emul_regwen <= emul_regwen;
      vif.emul_region <= emul_region;
      vif.sub_regwen <= sub_regwen;
      vif.sub_top <= sub_top;
      vif.sub_cfg <= sub_cfg;
      vif.rram_disable <= rram_disable;
      vif.sw_disable <= sw_disable;
      vif.if_sel <= if_sel;
      vif.hw_info_scr_dis <= scr_dis;
      vif.hw_info_ecc_dis <= ecc_dis;
      vif.phase <= phase;
    endtask

    // One clock edge: read what the DUT answered in the cycle that ends, decide the next cycle.
    task step(bit issue);
      bit phy_rd_done = 0, phy_wr_done = 0, phy_hdone = 0, phy_gnt;
      @(posedge vif.clk);
      cycle++;
      // Controller requester and PHY.
      if (ctrl_busy && (ctrl_wr ? vif.wr_done : vif.rd_done) === 1'b1) ctrl_busy = 0;
      if (phy_ctrl_pending) begin
        if (--phy_ctrl_cnt == 0) begin
          phy_ctrl_pending = 0;
          if (phy_ctrl_wr) phy_wr_done = 1;
          else phy_rd_done = 1;
        end
      end else if (vif.ctrl_req === 1'b1 && rram_disable == MuBi4False) begin
        phy_ctrl_pending = 1;
        phy_ctrl_wr = vif.ctrl_wr;
        phy_ctrl_cnt = $urandom_range(1, 4);
      end
      // Host requester and PHY.
      if (host_on && vif.host_gnt === 1'b1) begin
        host_on = 0;
        host_out++;
      end
      if (vif.host_rd_done === 1'b1 && host_out > 0) host_out--;
      if (vif.host_fwd === 1'b1 && vif.phy_host_gnt === 1'b1) begin
        int unsigned last = phy_due.size() ? phy_due[$] : cycle;
        int unsigned due = cycle + $urandom_range(1, 5);
        phy_due.push_back(due > last ? due : last + 1);
      end
      if (phy_due.size() && phy_due[0] <= cycle + 1) begin
        void'(phy_due.pop_front());
        phy_hdone = 1;
      end
      // New requests and configuration changes.
      if (issue && !ctrl_busy && !phy_ctrl_pending && !phy_rd_done && !phy_wr_done) begin
        if ($urandom_range(0, 59) == 0) begin
          mutate();
          drive_cfg();
        end else if ($urandom_range(0, 2) == 0) begin
          ctrl_busy = 1;
          ctrl_wr = $urandom_range(0, 1);
          ctrl_part = $urandom_range(0, 99) < 25;
          ctrl_ovfl = $urandom_range(0, 99) < 3;
          ctrl_addr = {12'(pick_page(ctrl_part)), 7'($urandom())};
        end
      end
      // The PHY's grant for the next cycle, with that cycle's rram_disable (after a configuration
      // change above): rram_phy suppresses new requests while RRAM is disabled.
      phy_gnt = phy_due.size() < 2 && rram_disable == MuBi4False && $urandom_range(0, 99) < 75;
      if (issue && !host_on && host_out < 2 && $urandom_range(0, 2) == 0) begin
        host_on = 1;
        host_addr = {12'(pick_page(0)), 7'($urandom())};
      end
      vif.rd_req <= ctrl_busy && !ctrl_wr;
      vif.wr_req <= ctrl_busy && ctrl_wr;
      vif.rd_addr <= ctrl_addr;
      vif.wr_addr <= ctrl_addr;
      vif.part <= ctrl_part;
      vif.rd_ovfl <= ctrl_busy && !ctrl_wr && ctrl_ovfl;
      vif.wr_ovfl <= ctrl_busy && ctrl_wr && ctrl_ovfl;
      vif.phy_rd_done <= phy_rd_done;
      vif.phy_wr_done <= phy_wr_done;
      vif.host_req <= host_on;
      vif.host_addr <= host_addr;
      vif.phy_host_gnt <= phy_gnt;
      vif.phy_host_rd_done <= phy_hdone;
      vif.phy_host_rd_err <= phy_hdone && $urandom_range(0, 19) == 0;
      vif.phy_host_rd_data <= phy_hdone ? {7'($urandom()), 32'($urandom())} : '0;
    endtask

    virtual task body();
      super.body();
      vif = cfg.mp_cfg.vif;
      void'($value$plusargs("num_cycles=%d", num_cycles));
      rand_all();
      drive_cfg();
      repeat (num_cycles) step(1);
      // Drain: no new requests until every open one has been answered.
      for (int i = 0; i < 1000 && (ctrl_busy || phy_ctrl_pending || host_on || host_out > 0 ||
                                   phy_due.size() > 0); i++) step(0);
      repeat (8) step(0);
      `uvm_info(get_type_name(), $sformatf("%0d cycles driven", cycle), UVM_LOW)
    endtask
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_mp_cmodel_scoreboard extends rram_ctrl_mp_scoreboard;
    `uvm_component_utils(rram_ctrl_mp_cmodel_scoreboard)

    chandle h;
    bit inject_fail, injected;
    // The configuration last given to the model.
    bit [1023:0] last_cfg;
    bit          have_cfg;
    bit [3:0]    lc_hist[4][$];
    // The previous cycle's controller request and its prediction.
    bit prev_req, prev_rd, prev_wr;
    int prev_err;
    // Expected answers of granted host reads, oldest first: No allowed, Yes refused, Unpred.
    int host_q[$];
    // Counts.
    int unsigned n_ctrl, n_ctrl_unpred, n_host, n_host_unpred, n_resp, n_resp_err, n_checks, n_err;
    int unsigned n_allowed, n_denied;

    // Coverage.
    int c_sel, c_part, c_wr, c_out, c_rule, c_idx, c_dis, c_swdis, c_page, c_lc, c_phase, c_depth;
    int c_edge, c_win_en, c_win_lock, c_ovl;

    covergroup cg_ctrl;
      cp_sel: coverpoint c_sel {
        bins none = {0}; bins sw = {1}; bins otp = {2}; bins lcmgr = {3}; bins loopback = {4};
        bins other = {[5:7]};
      }
      cp_part: coverpoint c_part { bins data = {0}; bins info = {1}; }
      cp_op: coverpoint c_wr { bins rd = {0}; bins wr = {1}; }
      cp_out: coverpoint c_out { bins denied = {No}; bins allowed = {Yes}; }
      cp_rule: coverpoint c_rule {
        bins otp_excl = {RuleOtpExcl}; bins subregion = {RuleSubregion};
        bins window_deny = {RuleWindowDeny}; bins mp_region = {RuleMpRegion};
        bins default_region = {RuleDefault}; bins info_page = {RuleInfoPage};
        bins info_lc = {RuleInfoLc}; bins lcmgr = {RuleLcMgr}; bins otp_plug = {RuleOtpPlug};
        bins out_of_range = {RuleOutOfRange}; bins none = {RuleNone};
      }
      cp_dis: coverpoint c_dis { bins f = {0}; bins t = {1}; bins invalid = {2}; }
      x_sel_part_op_out: cross cp_sel, cp_part, cp_op, cp_out {
        // No requester, or one the documentation does not list: never allowed. The loopback
        // requester (Rewrite) has no documented rule: predicted only out of range (denied).
        ignore_bins no_requester = binsof(cp_sel) intersect {0, [5:7]} && binsof(cp_out.allowed);
        ignore_bins loopback = binsof(cp_sel.loopback) && binsof(cp_out.allowed);
        // The OTP plug never reaches the info partition.
        ignore_bins otp_info = binsof(cp_sel.otp) && binsof(cp_part.info) && binsof(cp_out.allowed);
      }
      x_rule_op_out: cross cp_rule, cp_op, cp_out {
        ignore_bins deny_only = binsof(cp_rule) intersect {RuleOtpExcl, RuleWindowDeny,
            RuleOutOfRange, RuleNone} && binsof(cp_out.allowed);
      }
    endgroup

    covergroup cg_index;  // which MP region / subregion decided, and its outcome
      cp_region: coverpoint c_idx iff (c_rule == RuleMpRegion) { bins r[] = {[0:9]}; }
      cp_sub: coverpoint c_idx iff (c_rule == RuleSubregion) { bins s[] = {[0:23]}; }
      cp_window: coverpoint c_idx iff (c_rule == RuleWindowDeny) { bins w[] = {[0:1]}; }
      cp_out: coverpoint c_out { bins denied = {No}; bins allowed = {Yes}; }
      x_region_out: cross cp_region, cp_out;
      x_sub_out: cross cp_sub, cp_out;
      // The page at a rule's edge: 0 inside, 1 first page, 2 last page.
      cp_edge: coverpoint c_edge iff (c_rule == RuleMpRegion || c_rule == RuleSubregion) {
        bins mid_page = {0}; bins first_page = {1}; bins last_page = {2};
      }
      // Just outside an enabled MP region: 3 the page before, 4 the page after.
      cp_outside: coverpoint c_edge { bins page_before = {3}; bins page_after = {4}; }
    endgroup

    covergroup cg_window;  // window state at a software data request
      cp_en: coverpoint c_win_en { bins f = {0}; bins t = {1}; bins invalid = {2}; }
      cp_lock: coverpoint c_win_lock { bins unlocked = {0}; bins locked = {1}; }
      cp_ovl: coverpoint c_ovl { bins apart = {0}; bins overlap = {1}; }
      x_en_lock_ovl: cross cp_en, cp_lock, cp_ovl;
    endgroup

    covergroup cg_lc;  // info pages 5..7 under the life-cycle enables, software requests
      cp_page: coverpoint c_page { bins creator = {5}; bins owner = {6}; bins isolated = {7}; }
      cp_lc: coverpoint c_lc { bins on = {1}; bins off = {0}; }
      cp_op: coverpoint c_wr { bins rd = {0}; bins wr = {1}; }
      cp_out: coverpoint c_out { bins denied = {No}; bins allowed = {Yes}; }
      x_page_lc_op_out: cross cp_page, cp_lc, cp_op, cp_out {
        ignore_bins lc_off_allowed = binsof(cp_lc.off) && binsof(cp_out.allowed);
      }
    endgroup

    covergroup cg_lcmgr;  // lcmgr requests: phase x page class x op
      cp_phase: coverpoint c_phase { bins seed = {0}; bins rma = {1}; bins none = {2}; bins invalid = {3}; }
      // 0 creator/owner page, 1 isolated page, 2 other info page, 3 data page, 4 OTP page
      cp_page: coverpoint c_page { bins seed_pages = {0}; bins isolated = {1}; bins other_info = {2};
                                  bins data = {3}; bins otp = {4}; }
      cp_op: coverpoint c_wr { bins rd = {0}; bins wr = {1}; }
      x_phase_page_op: cross cp_phase, cp_page, cp_op;
    endgroup

    covergroup cg_host;
      cp_rule: coverpoint c_rule {
        bins otp_excl = {RuleOtpExcl}; bins window_deny = {RuleWindowDeny};
        bins mp_region = {RuleMpRegion}; bins default_region = {RuleDefault};
      }
      cp_out: coverpoint c_out { bins denied = {No}; bins allowed = {Yes}; }
      cp_swdis: coverpoint c_swdis { bins f = {0}; bins t = {1}; bins invalid = {2}; }
      cp_dis: coverpoint c_dis { bins f = {0}; bins t = {1}; bins invalid = {2}; }
      cp_depth: coverpoint c_depth { bins none = {0}; bins one = {1}; }
      x_rule_out: cross cp_rule, cp_out {
        ignore_bins deny_only = binsof(cp_rule) intersect {RuleOtpExcl, RuleWindowDeny} &&
                                binsof(cp_out.allowed);
      }
      x_depth_out: cross cp_depth, cp_out;
    endgroup

    covergroup cg_resp;  // host answers: refused (error from the unit) or the PHY's, with its error
      cp_kind: coverpoint c_out { bins phy = {No}; bins refused = {Yes}; }
      cp_err: coverpoint c_wr { bins ok = {0}; bins err = {1}; }
      x_kind_err: cross cp_kind, cp_err { ignore_bins refused_ok = binsof(cp_kind.refused) && binsof(cp_err.ok); }
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_ctrl = new();
      cg_index = new();
      cg_window = new();
      cg_lc = new();
      cg_lcmgr = new();
      cg_host = new();
      cg_resp = new();
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      h = eg2_rram_mp_create();
    endfunction

    static function int mubi_class(bit [3:0] v);
      return v == MuBi4False ? 0 : (v == MuBi4True ? 1 : 2);
    endfunction

    function void expect_ok(bit ok, string what, rram_ctrl_mp_mp_item it);
      n_checks++;
      if (!ok) begin
        n_err++;
        `uvm_error(get_type_name(), {what, "\n  ", it.convert2string()})
      end
    endfunction

    // The configuration of this cycle into the model (only when it changed).
    function void push_cfg(rram_ctrl_mp_mp_item it);
      bit [1023:0] c;
      c = {it.region, it.region_cfg, it.default_cfg, it.info_cfg, it.emul_regwen, it.emul_region,
           it.sub_regwen, it.hw_info_scr_dis, it.hw_info_ecc_dis};
      // sub_top and sub_cfg are compared on their own (they do not fit the key).
      if (have_cfg && c == last_cfg && !sub_changed(it)) return;
      have_cfg = 1;
      last_cfg = c;
      for (int i = 0; i < 10; i++) begin
        eg2_rram_mp_set(h, 0, i, it.region[24*i +: 24]);
        eg2_rram_mp_set(h, 1, i, it.region_cfg[20*i +: 20]);
      end
      eg2_rram_mp_set(h, 2, 0, it.default_cfg);
      for (int i = 0; i < 8; i++) eg2_rram_mp_set(h, 3, i, it.info_cfg[20*i +: 20]);
      for (int i = 0; i < 2; i++) begin
        eg2_rram_mp_set(h, 4, i, it.emul_regwen[i]);
        eg2_rram_mp_set(h, 5, i, it.emul_region[28*i +: 28]);
      end
      for (int i = 0; i < 24; i++) begin
        eg2_rram_mp_set(h, 6, i, it.sub_regwen[i]);
        eg2_rram_mp_set(h, 7, i, it.sub_top[8*i +: 8]);
        eg2_rram_mp_set(h, 8, i, {it.sub_cfg[16*i +: 16], 4'b0});  // register bits 19:4
      end
      eg2_rram_mp_set(h, 9, 0, {it.hw_info_ecc_dis, it.hw_info_scr_dis});
    endfunction

    bit [191:0] last_sub_top;
    bit [383:0] last_sub_cfg;
    function bit sub_changed(rram_ctrl_mp_mp_item it);
      bit ch = it.sub_top != last_sub_top || it.sub_cfg != last_sub_cfg;
      last_sub_top = it.sub_top;
      last_sub_cfg = it.sub_cfg;
      return ch;
    endfunction

    // A life-cycle enable as the model takes it: on, off, or settling (changed in the last four
    // cycles).
    function void push_lc(rram_ctrl_mp_mp_item it);
      bit [3:0] v[4] = '{it.lc_creator, it.lc_owner, it.lc_iso_rd, it.lc_iso_wr};
      for (int k = 0; k < 4; k++) begin
        int t;
        bit steady = 1;
        lc_hist[k].push_back(v[k]);
        if (lc_hist[k].size() > 4) void'(lc_hist[k].pop_front());
        for (int j = 0; j < lc_hist[k].size(); j++) steady &= lc_hist[k][j] == v[k];
        t = (!steady || lc_hist[k].size() < 4) ? Unpred : (v[k] == LcOn ? Yes : No);
        eg2_rram_mp_set(h, 10, k, t);
      end
    endfunction

    // Edge class of a page against the deciding MP region / subregion, or just outside a region.
    function int edge_of(rram_ctrl_mp_mp_item it, int unsigned page, int rule, int idx);
      int unsigned b, e;
      if (rule == RuleMpRegion) begin
        b = it.region[24*idx +: 12];
        e = b + it.region[24*idx+12 +: 12];
        return page == b ? 1 : (page == e ? 2 : 0);
      end
      if (rule == RuleSubregion) begin
        b = it.emul_region[28*(idx/12) +: 12];
        e = b + it.sub_top[8*idx +: 8];
        return page == b ? 1 : (page == e ? 2 : 0);
      end
      for (int i = 0; i < 10; i++) begin
        if (it.region_cfg[20*i +: 4] != MuBi4True) continue;
        b = it.region[24*i +: 12];
        e = b + it.region[24*i+12 +: 12];
        if (b > 0 && page == b - 1) return 3;
        if (page == e + 1) return 4;
      end
      return 0;
    endfunction

    virtual function void predict_mp(rram_ctrl_mp_mp_item item);
      rram_ctrl_mp_mp_item it = item;
      int err, fwd, scr, ecc, xr, rule, idx;
      bit rd = it.rd_req === 1'b1, wr = it.wr_req === 1'b1;
      int unsigned page;
      push_cfg(it);
      push_lc(it);

      // The previous cycle's controller request: its error and done now.
      if (prev_err != Unpred) begin
        bit e = prev_req && prev_err == Yes;
        expect_ok(it.mp_err === e, $sformatf("ctrl_mp_err %b, the model predicts %b for the previous cycle's request",
                                         it.mp_err, e), it);
        expect_ok(it.rd_done === (it.phy_rd_done | (e & prev_rd)),
              $sformatf("ctrl_rd_done %b: PHY done %b, refused read %b", it.rd_done, it.phy_rd_done, e & prev_rd), it);
        expect_ok(it.wr_done === (it.phy_wr_done | (e & prev_wr)),
              $sformatf("ctrl_wr_done %b: PHY done %b, refused write %b", it.wr_done, it.phy_wr_done, e & prev_wr), it);
      end

      // This cycle's controller request.
      prev_req = rd ^ wr;
      prev_rd = rd;
      prev_wr = wr;
      prev_err = No;
      if (rd && wr) begin
        prev_err = Unpred;  // the requesters never ask for both
      end else if (rd || wr) begin
        page = (rd ? it.rd_addr : it.wr_addr) >> 7;  // the page bits of the bus-word address
        eg2_rram_mp_ctrl(h, it.if_sel, it.phase, it.part, page, wr, rd ? it.rd_ovfl : it.wr_ovfl,
                         it.rram_disable, err, fwd, scr, ecc, xr, rule, idx);
        n_ctrl++;
        if (inject_fail && !injected && fwd != Unpred) begin
          fwd = fwd == Yes ? No : Yes;
          injected = 1;
          `uvm_info(get_type_name(), "inject_fail: one forwarding prediction inverted", UVM_LOW)
        end
        prev_err = err;
        if (fwd == Unpred || err == Unpred) n_ctrl_unpred++;
        if (fwd != Unpred) begin
          expect_ok(it.ctrl_req === (fwd == Yes), $sformatf("ctrl_req %b, the model says forwarded %0d (rule %0d.%0d)",
                                                        it.ctrl_req, fwd, rule, idx), it);
          expect_ok(it.ctrl_rd === (fwd == Yes && rd), $sformatf("ctrl_rd %b", it.ctrl_rd), it);
          expect_ok(it.ctrl_wr === (fwd == Yes && wr), $sformatf("ctrl_wr %b", it.ctrl_wr), it);
          if (fwd == Yes) begin
            expect_ok(it.ctrl_addr === (rd ? it.rd_addr : it.wr_addr), "ctrl_addr is not the request's", it);
            if (scr != Unpred) expect_ok(it.ctrl_scr === (scr == Yes), $sformatf("ctrl_scramble_en %b, model %0d (rule %0d.%0d)", it.ctrl_scr, scr, rule, idx), it);
            if (ecc != Unpred) expect_ok(it.ctrl_ecc === (ecc == Yes), $sformatf("ctrl_ecc_en %b, model %0d (rule %0d.%0d)", it.ctrl_ecc, ecc, rule, idx), it);
            if (xr != Unpred) expect_ok(it.ctrl_xor === (xr == Yes), $sformatf("ctrl_addr_xor_en %b, model %0d (rule %0d.%0d)", it.ctrl_xor, xr, rule, idx), it);
          end
        end
        // Coverage of every request whose answer (error or not) is predicted, disabled ones too.
        begin
          if (err != Unpred) begin
            if (err == Yes) n_denied++;
            else n_allowed++;
            c_sel = it.if_sel; c_part = it.part; c_wr = wr; c_out = err == No; c_rule = rule; c_idx = idx;
            c_dis = mubi_class(it.rram_disable);
            if (cfg.en_cov) cg_ctrl.sample();
            if (it.if_sel == 1 && it.part == 0 && it.rram_disable == MuBi4False) begin
              c_edge = edge_of(it, page, rule, idx);
              if (cfg.en_cov) cg_index.sample();
              for (int w = 0; w < 2; w++) begin
                c_win_en = mubi_class(it.emul_region[28*w+24 +: 4]) == 1 ? 1 :
                           (mubi_class(it.emul_region[28*w+24 +: 4]) == 0 ? 0 : 2);
                c_win_lock = !it.emul_regwen[w];
                c_ovl = it.emul_region[0 +: 12] <= it.emul_region[28 +: 12] + it.emul_region[40 +: 12] &&
                        it.emul_region[28 +: 12] <= it.emul_region[0 +: 12] + it.emul_region[12 +: 12];
                if (cfg.en_cov) cg_window.sample();
              end
            end
            if (it.if_sel == 1 && it.part == 1 && page inside {[5:7]} && it.rram_disable == MuBi4False) begin
              c_page = page;
              c_lc = page == 5 ? it.lc_creator == LcOn : page == 6 ? it.lc_owner == LcOn :
                     wr ? it.lc_iso_wr == LcOn : it.lc_iso_rd == LcOn;
              if (cfg.en_cov) cg_lc.sample();
            end
            if (it.if_sel == 3) begin
              c_phase = it.phase;
              c_page = it.part ? (page inside {5, 6} ? 0 : page == 7 ? 1 : 2) : (page >= 4090 ? 4 : 3);
              if (cfg.en_cov) cg_lcmgr.sample();
            end
          end
        end
      end else begin
        expect_ok(it.ctrl_req === 1'b0 && it.ctrl_rd === 1'b0 && it.ctrl_wr === 1'b0,
              "a PHY request without a controller request", it);
      end

      // Host answers first: a granted read is never answered in the cycle it is granted.
      if (it.host_rd_done === 1'b1) begin
        if (host_q.size() == 0) begin
          expect_ok(0, "a host answer without an outstanding read", it);
        end else begin
          int exp = host_q.pop_front();
          n_resp++;
          if (exp == Yes) begin
            n_resp_err++;
            expect_ok(it.host_rd_err === 1'b1, "a refused host read answered without an error", it);
            expect_ok(it.phy_host_rd_done === 1'b0, "the PHY's answer collides with a refused read's answer", it);
          end else if (exp == No) begin
            expect_ok(it.phy_host_rd_done === 1'b1, "a host answer for a forwarded read that the PHY has not given", it);
            expect_ok(it.host_rd_err === it.phy_host_rd_err && it.host_rd_data === it.phy_host_rd_data,
                  "a forwarded read's answer is not the PHY's", it);
          end
          if (exp != Unpred) begin
            c_out = exp; c_wr = it.host_rd_err;
            if (cfg.en_cov) cg_resp.sample();
          end
        end
      end else begin
        expect_ok(it.phy_host_rd_done !== 1'b1, "the PHY's host answer is dropped", it);
      end

      // This cycle's host read.
      if (it.host_req === 1'b1) begin
        page = it.host_addr >> 7;
        eg2_rram_mp_host(h, page, it.sw_disable, it.rram_disable, err, fwd, scr, ecc, rule, idx);
        n_host++;
        if (fwd == Unpred || err == Unpred) n_host_unpred++;
        if (fwd != Unpred) begin
          expect_ok(it.host_fwd === (fwd == Yes), $sformatf("host_req to the PHY %b, the model says forwarded %0d (rule %0d.%0d)",
                                                        it.host_fwd, fwd, rule, idx), it);
          if (fwd == Yes) begin
            if (scr != Unpred) expect_ok(it.host_scr === (scr == Yes), $sformatf("host_scramble_en %b, model %0d", it.host_scr, scr), it);
            if (ecc != Unpred) expect_ok(it.host_ecc === (ecc == Yes), $sformatf("host_ecc_en %b, model %0d", it.host_ecc, ecc), it);
            expect_ok(it.host_gnt === it.phy_host_gnt, "a forwarded host read's grant is not the PHY's", it);
          end
        end
        if (err == Yes && fwd != Yes) expect_ok(it.host_gnt === 1'b1, "a refused host read is not granted at once", it);
        if (it.host_gnt === 1'b1) host_q.push_back(err == Unpred || fwd == Unpred && err != Yes ? Unpred : err);
        if (err != Unpred) begin
          c_rule = rule; c_out = err == No; c_swdis = mubi_class(it.sw_disable); c_dis = mubi_class(it.rram_disable);
          c_depth = host_q.size() > (it.host_gnt === 1'b1);
          if (cfg.en_cov) cg_host.sample();
        end
      end else begin
        expect_ok(it.host_fwd === 1'b0 && it.host_gnt === 1'b0, "host_req or host_gnt without a host read", it);
      end
    endfunction

    virtual function void check_phase(uvm_phase phase);
      super.check_phase(phase);
      if (host_q.size() != 0)
        `uvm_error(get_type_name(), $sformatf("%0d granted host reads never answered", host_q.size()))
      if (n_ctrl < 100 || n_host < 100 || n_allowed < 10 || n_denied < 10)
        `uvm_error(get_type_name(), $sformatf("too little checked: %0d controller, %0d host requests, %0d allowed, %0d refused",
                                              n_ctrl, n_host, n_allowed, n_denied))
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: nothing was inverted")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      super.report_phase(phase);
      `uvm_info(get_type_name(), $sformatf({"checked against the model: %0d controller requests (%0d allowed, ",
          "%0d refused, %0d not predicted), %0d host reads (%0d not predicted), %0d host answers (%0d refused), ",
          "%0d checks, %0d failed"}, n_ctrl, n_allowed, n_denied, n_ctrl_unpred, n_host, n_host_unpred, n_resp,
          n_resp_err, n_checks, n_err), UVM_LOW)
      if (h != null) eg2_rram_mp_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_mp_cmodel_random_test extends rram_ctrl_mp_base_test;
    `uvm_component_utils(rram_ctrl_mp_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      rram_ctrl_mp_scoreboard::type_id::set_type_override(rram_ctrl_mp_cmodel_scoreboard::get_type());
      test_seq = "rram_ctrl_mp_cmodel_vseq";
      super.build_phase(phase);
    endfunction
  endclass

  // Must fail: the scoreboard inverts the first definite forwarding prediction.
  class rram_ctrl_mp_cmodel_random_inject_fail_test extends rram_ctrl_mp_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_mp_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass
endpackage
