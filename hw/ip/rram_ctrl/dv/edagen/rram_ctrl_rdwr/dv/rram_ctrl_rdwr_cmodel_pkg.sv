// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The controller-path unit bench's own part (edagen rram_ctrl_rdwr.yaml dv.files), on edagen's
// generated skeleton:
//   - rram_ctrl_rdwr_cmodel_vseq: the three requesters and the units behind the path, each by its
//     documented interface. Software (CONTROL / ADDR with START held until done, as the register
//     clears START on done; the write FIFO window: words pushed after START; the read FIFO: 16
//     entries, drained at random), the OTP plug (requests of one RRAM word, read or write, in the
//     OTP pages, req held over a few commands, "back to back operations"), the lcmgr plug (seed
//     reads of 8 words in the info partition with PhaseSeed, wipe writes and reads with PhaseRma),
//     memory protection and the PHY behind the read and write engines (one request at a time,
//     answered after 1 to 4 cycles, read words with the address XOR kept and the integrity of the
//     clean word as rram_phy gives them, mp_err / rd_err / wr_intg_err at a mode's rate, a memory
//     image the writes update), phy_init_done, init_done & keys_valid, the RMA software disable and
//     rram_disable per mode.
//   - rram_ctrl_rdwr_cmodel_scoreboard: every request, pushed word, RRAM request and answer,
//     delivered word and completion against the controller-path reference model
//     (eg2_rram_ctrl_rdwr_core.h through eg2_rram_ctrl_rdwr_dpi.cc); the functional coverage.
//   - the tests: random, err (high error rates), contend (heavy plug traffic), gate (requests
//     before init, the RMA software disable, undefined and unaligned operations), long (up to 1024
//     words), disable (rram_disable mid-run), random_inject_fail (one delivered word flipped: must
//     fail); the finding twins, which must fail while their finding is open: swpush_race (software
//     pushes its first write word while a plug holds the arbiter), disable_strict (a request left
//     unanswered after rram_disable is an error), errearly_strict (an error code visible before
//     the completion is an error). The strict twins are plusargs: +rdwr_strict_disable=1,
//     +rdwr_strict_err_early=1, +rdwr_strict_erraddr=1 (a stale ERR_ADDR when the last word of the
//     operation fails is an error; without it the bench counts it as the finding).
package rram_ctrl_rdwr_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import prim_mubi_pkg::*;
  import eg2_rram_ctrl_rdwr_cmodel_pkg::*;
  import rram_ctrl_rdwr_cp_agent_pkg::*;
  import rram_ctrl_rdwr_env_pkg::*;
  import rram_ctrl_rdwr_test_pkg::*;

  typedef enum int {ModeRandom, ModeErr, ModeContend, ModeGate, ModeLong, ModeDisable, ModeRace} mode_e;

  // Who: 0 software, 1 OTP plug, 2 lcmgr plug (the model's numbering).
  localparam int SW = 0;
  localparam int OT = 1;
  localparam int LC = 2;

  class rdwr_ctx;
    static mode_e mode = ModeRandom;
    static bit ready;                  // inputs driven, reset released: the scoreboard starts
    static longint dis_cycle = -1;     // scoreboard cycle of rram_disable
    static int unsigned rd_err_pct = 2, mp_err_pct = 2, wr_err_pct = 2;
    static bit sw_push_safe = 1;       // software pushes only once no plug holds the arbiter
    // memory image: {part, bus-word address} -> clean word
    static bit [31:0] image[int unsigned];
  endclass

  function automatic bit [38:0] intg39(bit [31:0] w);
    return 39'(eg2_rdwr_with_intg(int'(w)));
  endfunction

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_rdwr_cmodel_vseq extends rram_ctrl_rdwr_base_vseq;
    `uvm_object_utils(rram_ctrl_rdwr_cmodel_vseq)

    virtual rram_ctrl_rdwr_cp_if vif;
    int unsigned num_ops = 300;
    longint cyc;

    // software
    bit sw_on, sw_pushing, sw_go;  // sw_go: the operation may take its words (safe mode)
    int unsigned sw_op, sw_num, sw_addr, sw_part, sw_push_left, sw_gap, sw_ops, sw_wait;
    bit [38:0] sw_wd;
    int unsigned rdq;                  // read FIFO level
    int unsigned plug_quiet;           // cycles without a plug request
    int unsigned stray_left;
    // plugs: 1 OTP, 2 lcmgr
    bit p_req[3], p_on[3];
    int unsigned p_cmds[3], p_gap[3], p_op[3], p_num[3], p_addr[3], p_part[3], p_phase[3];
    int unsigned p_push_left[3], p_rate[3];
    bit p_wv[3];
    bit [38:0] p_wd[3];
    // responders
    bit rd_busy, wr_busy;
    int unsigned rd_lat, wr_lat;
    bit disabled;

    function new(string name = "rram_ctrl_rdwr_cmodel_vseq");
      super.new(name);
    endfunction

    function bit [38:0] rand_word();
      return intg39($urandom());
    endfunction

    function bit [38:0] phy_word(bit part, int unsigned a);
      int unsigned k = {part, 19'(a)};
      bit [31:0] clean;
      bit [38:0] w;
      if (!rdwr_ctx::image.exists(k)) rdwr_ctx::image[k] = $urandom();
      clean = rdwr_ctx::image[k];
      w = intg39(clean);
      return {w[38:32], clean ^ 32'(a)};
    endfunction

    // ---- software ----
    task sw_cycle(bit issue);
      // what happened in the cycle that just ended
      bit accepted = vif.sw_wvalid === 1'b1 && vif.sw_wready === 1'b1;
      if (vif.ot_req === 1'b1 || vif.lc_req === 1'b1) plug_quiet = 0;
      else plug_quiet++;
      if (accepted) begin
        if (stray_left > 0) stray_left--;
        else if (sw_push_left > 0) sw_push_left--;
        sw_wd = rand_word();
      end
      if (sw_on && vif.sw_done === 1'b1) begin
        sw_on = 0;
        sw_pushing = 0;
        vif.sw_start <= 1'b0;
        vif.sw_wvalid <= 1'b0;
        sw_gap = $urandom_range(1, 6);
        sw_ops++;
        accepted = 1;  // a word still offered is withdrawn: the operation is over
      end
      if (vif.sw_rvalid === 1'b1 && vif.sw_rready === 1'b1) rdq++;
      if (rdq > 0 && $urandom_range(0, 99) < (rdwr_ctx::mode == ModeLong ? 70 : 45)) rdq--;
      vif.sw_rready <= rdq < 16;
      if (!sw_on) begin
        if (stray_left == 0 && $urandom_range(0, 199) == 0) begin
          stray_left = $urandom_range(1, 3);   // stray words with no operation: must never be written
          sw_wd = rand_word();
        end
        vif.sw_wvalid <= stray_left > 0;
        vif.sw_wdata <= sw_wd;
        if (sw_gap > 0) sw_gap--;
        else if (issue && stray_left == 0) begin
          new_sw_op();
          sw_on = 1;
          sw_wait = 0;
          sw_go = 0;
          vif.sw_start <= 1'b1;
          vif.sw_op <= sw_op;
          vif.sw_num <= sw_num;
          vif.sw_addr <= sw_addr;
          vif.sw_part <= sw_part;
          vif.sw_wvalid <= 1'b0;
        end
      end else begin
        // Pushes only after START was sent (the TL-UL write holds its word until it is taken). In
        // the safe mode only once the arbiter must have taken the operation: no plug request for 3
        // cycles with START high (the finding: a word offered while a plug holds the arbiter is
        // taken and dropped when the arbiter passes through idle).
        sw_wait++;
        if (sw_pushing && !accepted) begin
          // the offered word stays
        end else if (sw_op == 1 && sw_push_left > 0 && sw_wait > 1 &&
                     (sw_go || !rdwr_ctx::sw_push_safe ||
                      (plug_quiet > 3 && sw_wait > 4 && vif.if_sel === 3'd1))) begin  // SwSel: past phy_init_done
          sw_go = 1;
          sw_pushing = $urandom_range(0, 99) < 80;
        end else begin
          sw_pushing = 0;
        end
        vif.sw_wvalid <= sw_pushing;
        vif.sw_wdata <= sw_wd;
      end
    endtask

    function void new_sw_op();
      int unsigned r = $urandom_range(0, 99);
      int unsigned bad = rdwr_ctx::mode == ModeGate ? 25 : 4;
      int unsigned maxw = rdwr_ctx::mode == ModeLong ? 1024 : 40;
      sw_part = $urandom_range(0, 9) == 0;
      if (r < 45) sw_op = 0;
      else if (r < 85) sw_op = 1;
      else if (r < 97) sw_op = 2;
      else sw_op = 3;  // reserved
      if ($urandom_range(0, 99) < 10) sw_num = maxw - 1;
      else sw_num = $urandom_range(0, maxw > 64 ? 300 : maxw - 1);
      if (rdwr_ctx::mode == ModeLong && $urandom_range(0, 3) == 0) sw_num = 1023;
      sw_addr = $urandom_range(0, 21'h1fffff);
      if ($urandom_range(0, 19) == 0) sw_addr = 21'h1fffff - $urandom_range(0, 64);  // wrap the partition
      if (sw_op == 1) begin
        sw_num = (sw_num | 3);
        if (sw_num > 1023) sw_num = 1023;
        sw_addr &= ~32'hf;
        if ($urandom_range(0, 99) < bad) sw_addr |= $urandom_range(1, 15);
        if ($urandom_range(0, 99) < bad) sw_num = sw_num ^ $urandom_range(1, 3);
        sw_push_left = sw_num + 1;
      end else begin
        sw_push_left = 0;
      end
      if (sw_op == 2) begin
        sw_addr &= ~32'hf;
        if ($urandom_range(0, 99) < bad) sw_addr |= $urandom_range(1, 15);
      end
      sw_wd = rand_word();
    endfunction

    // ---- plugs: req held over a few commands, start held until done ----
    task plug_cycle(int p, bit issue);
      if (p_wv[p] && (p == OT ? vif.ot_wready : vif.lc_wready) === 1'b1) begin
        p_push_left[p]--;
        p_wd[p] = rand_word();
      end
      if (p_on[p] && (p == OT ? vif.ot_done : vif.lc_done) === 1'b1) begin
        p_on[p] = 0;
        p_wv[p] = 0;
        p_cmds[p]--;
        p_gap[p] = $urandom_range(1, 3);  // start low for a cycle at least: a new command
      end
      // Safe mode: no new plug request while a software write waits to push its first word (the
      // finding: that word is lost when the arbiter passes through idle).
      if (!p_req[p] && issue && !disabled && $urandom_range(0, 999) < p_rate[p] &&
          !(rdwr_ctx::sw_push_safe && sw_on && sw_op == 1 && !sw_go)) begin
        p_req[p] = 1;
        p_cmds[p] = $urandom_range(1, p == OT ? 3 : 6);
        p_gap[p] = 0;
      end
      if (p_req[p] && !p_on[p]) begin
        if (p_cmds[p] == 0) p_req[p] = 0;
        else if (p_gap[p] > 0) p_gap[p]--;
        else begin
          p_on[p] = 1;
          if (p == OT) begin
            p_op[p] = $urandom_range(0, 1);
            p_num[p] = 3;
            p_part[p] = 0;
            p_phase[p] = 2;
            p_addr[p] = ((4090 + $urandom_range(0, 5)) * 128 + 4 * $urandom_range(0, 31)) * 4;
          end else begin
            p_phase[p] = $urandom_range(0, 1);       // PhaseSeed, PhaseRma
            if (p_phase[p] == 0) begin
              p_op[p] = 0;
              p_num[p] = 7;
              p_part[p] = 1;
              p_addr[p] = ((5 + $urandom_range(0, 1)) * 128) * 4;
            end else begin
              p_op[p] = $urandom_range(0, 1);
              p_num[p] = 4 * $urandom_range(1, 8) - 1;
              p_part[p] = $urandom_range(0, 1);
              p_addr[p] = (p_part[p] ? (5 + $urandom_range(0, 2)) * 128 : $urandom_range(0, 4089) * 128) * 4 +
                          16 * $urandom_range(0, 7);
            end
          end
          p_push_left[p] = p_op[p] == 1 ? p_num[p] + 1 : 0;
          p_wd[p] = rand_word();
        end
      end
      p_wv[p] = p_on[p] && p_push_left[p] > 0 && $urandom_range(0, 9) < 8;
      if (p == OT) begin
        vif.ot_req <= p_req[p];
        vif.ot_start <= p_on[p];
        vif.ot_op <= p_op[p];
        vif.ot_num <= p_num[p];
        vif.ot_part <= p_part[p];
        vif.ot_addr <= p_addr[p];
        vif.ot_wvalid <= p_wv[p];
        vif.ot_wdata <= p_wd[p];
      end else begin
        vif.lc_req <= p_req[p];
        vif.lc_start <= p_on[p];
        vif.lc_op <= p_op[p];
        vif.lc_num <= p_num[p];
        vif.lc_part <= p_part[p];
        vif.lc_addr <= p_addr[p];
        vif.lc_phase <= p_phase[p];
        vif.lc_wvalid <= p_wv[p];
        vif.lc_wdata <= p_wd[p];
      end
    endtask

    // ---- memory protection and the PHY: one request at a time per engine ----
    task resp_cycle();
      bit [38:0] d;
      vif.rd_done <= 1'b0;
      vif.rd_err <= 1'b0;
      vif.rd_mp_err <= 1'b0;
      vif.wr_done <= 1'b0;
      vif.wr_mp_err <= 1'b0;
      vif.wr_intg_err <= 1'b0;
      if (rd_busy && vif.rd_done === 1'b1) rd_busy = 0;  // answered in the cycle that ended
      if (wr_busy && vif.wr_done === 1'b1) begin
        wr_busy = 0;
        if (vif.wr_req === 1'b1 && vif.wr_mp_err !== 1'b1 && vif.wr_intg_err !== 1'b1)
          rdwr_ctx::image[{vif.part, 19'(vif.wr_addr)}] = vif.wr_data[31:0];
      end
      if (!rd_busy && vif.rd_req === 1'b1 && vif.rd_done !== 1'b1 && !disabled) begin
        rd_busy = 1;
        rd_lat = $urandom_range(0, 3);
      end
      if (rd_busy) begin
        if (rd_lat > 0) rd_lat--;
        else begin
          vif.rd_done <= 1'b1;
          vif.rd_data <= phy_word(vif.part, vif.rd_addr);
          if ($urandom_range(0, 999) < 10 * rdwr_ctx::rd_err_pct) vif.rd_err <= 1'b1;
          if ($urandom_range(0, 999) < 10 * rdwr_ctx::mp_err_pct || vif.rd_ovfl === 1'b1) vif.rd_mp_err <= 1'b1;
        end
      end
      if (!wr_busy && vif.wr_req === 1'b1 && vif.wr_done !== 1'b1 && !disabled) begin
        wr_busy = 1;
        wr_lat = $urandom_range(0, 3);
      end
      if (wr_busy) begin
        if (wr_lat > 0) wr_lat--;
        else begin
          vif.wr_done <= 1'b1;
          if ($urandom_range(0, 999) < 10 * rdwr_ctx::wr_err_pct) vif.wr_intg_err <= 1'b1;
          if ($urandom_range(0, 999) < 10 * rdwr_ctx::mp_err_pct || vif.wr_ovfl === 1'b1) vif.wr_mp_err <= 1'b1;
        end
      end
    endtask

    task step(bit issue);
      @(posedge vif.clk);
      cyc++;
      resp_cycle();
      sw_cycle(issue && sw_ops < num_ops);
      plug_cycle(OT, issue);
      plug_cycle(LC, issue);
    endtask

    virtual task body();
      int unsigned phy_delay = $urandom_range(3, 40);
      int unsigned init_delay = rdwr_ctx::mode == ModeGate ? $urandom_range(200, 800) : $urandom_range(0, 100);
      longint last_cyc;
      int unsigned last_ops;
      vif = cfg.cp_cfg.vif;
      void'($value$plusargs("num_ops=%d", num_ops));
      case (rdwr_ctx::mode)
        ModeErr: begin rdwr_ctx::rd_err_pct = 12; rdwr_ctx::mp_err_pct = 8; rdwr_ctx::wr_err_pct = 10; end
        ModeContend, ModeRace: begin p_rate[OT] = 40; p_rate[LC] = 30; end
        ModeLong: num_ops = num_ops / 4;
        default: ;
      endcase
      if (p_rate[OT] == 0) begin p_rate[OT] = 6; p_rate[LC] = 4; end
      if (rdwr_ctx::mode == ModeRace) rdwr_ctx::sw_push_safe = 0;
      @(posedge vif.clk);
      vif.rram_dis <= MuBi4False;
      vif.sw_dis <= MuBi4False;
      vif.phy_init_done <= 1'b0;
      vif.ctrl_init_done <= 1'b0;
      vif.sw_start <= 1'b0;
      vif.sw_op <= '0;
      vif.sw_part <= '0;
      vif.sw_num <= '0;
      vif.sw_addr <= '0;
      vif.sw_wvalid <= 1'b0;
      vif.sw_wdata <= '0;
      vif.sw_rready <= 1'b1;
      vif.lc_req <= 1'b0;
      vif.lc_start <= 1'b0;
      vif.lc_op <= '0;
      vif.lc_part <= '0;
      vif.lc_num <= '0;
      vif.lc_phase <= '0;
      vif.lc_addr <= '0;
      vif.lc_wvalid <= 1'b0;
      vif.lc_wdata <= '0;
      vif.lc_rready <= 1'b1;
      vif.ot_req <= 1'b0;
      vif.ot_start <= 1'b0;
      vif.ot_op <= '0;
      vif.ot_part <= '0;
      vif.ot_num <= '0;
      vif.ot_addr <= '0;
      vif.ot_wvalid <= 1'b0;
      vif.ot_wdata <= '0;
      vif.ot_rready <= 1'b1;
      vif.fifo_clr <= 1'b0;
      vif.rd_data <= '0;
      vif.rd_done <= 1'b0;
      vif.rd_err <= 1'b0;
      vif.rd_mp_err <= 1'b0;
      vif.wr_done <= 1'b0;
      vif.wr_mp_err <= 1'b0;
      vif.wr_intg_err <= 1'b0;
      super.body();
      rdwr_ctx::ready = 1;
      // Requests (plugs and software) start before phy_init_done: nothing may happen until then.
      repeat (phy_delay) step(1);
      vif.phy_init_done <= 1'b1;
      repeat (init_delay) step(1);
      vif.ctrl_init_done <= 1'b1;
      last_cyc = cyc;
      last_ops = sw_ops;
      while (sw_ops < num_ops) begin
        step(1);
        if (sw_ops != last_ops) begin
          last_ops = sw_ops;
          last_cyc = cyc;
        end else if (cyc > last_cyc + 30000) begin
          `uvm_info(get_type_name(), $sformatf({"no software operation completed for 30000 cycles: stopping. software: on %0d op %0d num %0d addr 0x%0h push_left %0d; OTP: req %0d on %0d op %0d push_left %0d; lcmgr: req %0d on %0d op %0d num %0d push_left %0d; if_sel %0d fifo %0d rd_req %0b wr_req %0b sw_wready %0b lc_wready %0b ot_wready %0b"}, sw_on, sw_op, sw_num, sw_addr, sw_push_left, p_req[OT], p_on[OT], p_op[OT], p_push_left[OT], p_req[LC], p_on[LC], p_op[LC], p_num[LC], p_push_left[LC], vif.if_sel, vif.fifo_depth, vif.rd_req, vif.wr_req, vif.sw_wready, vif.lc_wready, vif.ot_wready), UVM_LOW)
          break;
        end
        if (rdwr_ctx::mode == ModeGate && sw_ops >= num_ops / 2 && vif.sw_dis == MuBi4False && $urandom_range(0, 199) == 0)
          vif.sw_dis <= $urandom_range(0, 3) == 0 ? 4'h0 : MuBi4True;  // RMA entry: latched
        if (rdwr_ctx::mode == ModeDisable && !disabled && sw_ops > num_ops / 3 && $urandom_range(0, 99) == 0) begin
          disabled = 1;
          vif.rram_dis <= $urandom_range(0, 1) ? MuBi4True : 4'h0;
          repeat (2000) step(1);
          break;
        end
      end
      // drain: no new requests
      for (int i = 0; i < 20000 && (sw_on || p_req[OT] || p_req[LC]); i++) step(0);
      repeat (50) step(0);
    endtask
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_rdwr_cmodel_scoreboard extends rram_ctrl_rdwr_scoreboard;
    `uvm_component_utils(rram_ctrl_rdwr_cmodel_scoreboard)

    chandle h;
    longint cyc;
    bit inject_fail, injected, strict_dis, strict_early, strict_erraddr;
    longint trace_from = -1, trace_to = -1;  // +rdwr_trace_from=, +rdwr_trace_to=: a per-cycle trace
    bit pv_rd_req, pv_rd_done, pv_wr_req, pv_wr_done, pv_sw, pv_lc, pv_ot;  // the previous cycle
    int unsigned n_err, n_checks, n_err_early, n_stray, n_stray_stall, n_rdq_stall, n_wrap, n_erraddr_last;
    // per requester: what its request was (for coverage at its completion)
    int unsigned rq_op[3], rq_num[3], rq_reason[3];
    bit rq_contended[3];
    int unsigned last_sel;

    // coverage
    int unsigned cv_who, cv_op, cv_err, cv_num, cv_reason, cv_sel, cv_contended;
    covergroup cg_done;
      option.per_instance = 1;
      cp_who: coverpoint cv_who { bins sw = {0}; bins otp = {1}; bins lc = {2}; }
      cp_op: coverpoint cv_op { bins rd = {0}; bins wr = {1}; bins rewrite = {2}; bins reserved = {3}; }
      cp_err: coverpoint cv_err {
        bins ok = {0}; bins op_err = {1}; bins mp_err = {2}; bins rd_err = {4}; bins wr_err = {8};
        bins rd_mp = {6}; bins wr_mp = {10};
      }
      cp_num: coverpoint cv_num {
        bins one = {0}; bins few = {[1:3]}; bins some = {[4:15]}; bins fifo = {[16:63]};
        bins many = {[64:1022]}; bins max = {1023};
      }
      x_who_op: cross cp_who, cp_op {
        ignore_bins hw_rewrite = binsof(cp_who) intersect {1, 2} && binsof(cp_op) intersect {2, 3};
      }
      x_op_err: cross cp_op, cp_err {
        ignore_bins rd_wr = binsof(cp_op) intersect {0} && binsof(cp_err) intersect {8, 10};
        ignore_bins wr_rd = binsof(cp_op) intersect {1} && binsof(cp_err) intersect {4, 6};
        ignore_bins res = binsof(cp_op) intersect {3} && !binsof(cp_err) intersect {1};
        ignore_bins dbl = binsof(cp_op) intersect {2} && binsof(cp_err) intersect {6, 10};
      }
      x_op_num: cross cp_op, cp_num { ignore_bins o = binsof(cp_op) intersect {2, 3}; }
    endgroup
    covergroup cg_reject;
      option.per_instance = 1;
      cp_reason: coverpoint cv_reason {
        bins not_init = {1}; bins undefined_op = {2}; bins wr_unaligned = {3}; bins wr_num = {4};
        bins rewrite_unaligned = {5}; bins rma_sw_dis = {6};
      }
    endgroup
    covergroup cg_arb;
      option.per_instance = 1;
      cp_sel: coverpoint cv_sel { bins sw = {1}; bins otp = {2}; bins lc = {3}; bins loop = {4}; }
      cp_contended: coverpoint cv_contended { bins alone = {0}; bins sw_waiting = {1}; bins both_hw = {2}; }
      x_sel_cont: cross cp_sel, cp_contended {
        ignore_bins sw_waits_for_sw = binsof(cp_sel) intersect {1, 4} && binsof(cp_contended) intersect {1};
      }
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_done = new();
      cg_reject = new();
      cg_arb = new();
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      void'($value$plusargs("rdwr_strict_disable=%d", strict_dis));
      void'($value$plusargs("rdwr_strict_err_early=%d", strict_early));
      void'($value$plusargs("rdwr_strict_erraddr=%d", strict_erraddr));
      void'($value$plusargs("rdwr_trace_from=%d", trace_from));
      void'($value$plusargs("rdwr_trace_to=%d", trace_to));
      h = eg2_rdwr_create();
    endfunction

    function void judge(int r, string what);
      string why;
      n_checks++;
      if (r != 0) begin
        n_err++;
        // r == 2: a check of the bench itself (no model reason)
        if (r != 2) why = {": ", eg2_rdwr_error(h)};
        if (n_err <= 30) `uvm_error(get_type_name(), $sformatf("cycle %0d: %s%s", cyc, what, why))
      end
    endfunction

    function int unsigned reject_reason(rram_ctrl_rdwr_cp_item it);
      if (it.ctrl_init_done !== 1'b1) return 1;
      if (it.sw_op == 3) return 2;
      if (it.sw_op == 1 && it.sw_addr[3:0] != 0) return 3;
      if (it.sw_op == 1 && it.sw_num[1:0] != 2'b11) return 4;
      if (it.sw_op == 2 && it.sw_addr[3:0] != 0) return 5;
      if (it.sw_dis != MuBi4False) return 6;
      return 0;
    endfunction

    virtual function void predict_cp(rram_ctrl_rdwr_cp_item item);
      rram_ctrl_rdwr_cp_item it = item;
      bit p_rd_req, p_rd_done, p_wr_req, p_wr_done, p_sw, p_lc, p_ot;
      bit [38:0] d;
      if (!rdwr_ctx::ready) return;
      cyc++;
      p_rd_req = pv_rd_req; p_rd_done = pv_rd_done; p_wr_req = pv_wr_req; p_wr_done = pv_wr_done;
      p_sw = pv_sw; p_lc = pv_lc; p_ot = pv_ot;
      pv_rd_req = it.rd_req === 1'b1; pv_rd_done = it.rd_done === 1'b1;
      pv_wr_req = it.wr_req === 1'b1; pv_wr_done = it.wr_done === 1'b1;
      pv_sw = it.sw_start === 1'b1;
      pv_lc = it.lc_req === 1'b1 && it.lc_start === 1'b1;
      pv_ot = it.ot_req === 1'b1 && it.ot_start === 1'b1;
      if (it.sw_wvalid === 1'b1 && it.sw_wready !== 1'b1 && !eg2_rdwr_pending(h, SW)) n_stray_stall++;
      if (cyc >= trace_from && cyc <= trace_to)
        `uvm_info(get_type_name(), $sformatf("trace %0d: sw start %b op %0d wv %b wr %b | sel %0d fifo %0d | ot req %b st %b | lc req %b st %b | rd %b wr %b done %b", cyc, it.sw_start, it.sw_op, it.sw_wvalid, it.sw_wready, it.if_sel, it.fifo_depth, it.ot_req, it.ot_start, it.lc_req, it.lc_start, it.rd_req, it.wr_req, it.sw_done), UVM_LOW)
      if (it.rram_dis != MuBi4False && rdwr_ctx::dis_cycle < 0) rdwr_ctx::dis_cycle = cyc;
      eg2_rdwr_cycle(h, it.phy_init_done === 1'b1, it.ctrl_init_done === 1'b1, it.rram_dis != MuBi4False,
                     it.sw_dis != MuBi4False);
      // 1. new requests
      if (it.sw_start === 1'b1 && !p_sw) begin
        rq_op[SW] = it.sw_op; rq_num[SW] = it.sw_num; rq_reason[SW] = reject_reason(it);
        rq_contended[SW] = 0;
        judge(eg2_rdwr_request(h, SW, it.sw_op, it.sw_part, it.sw_num, it.sw_addr, 0), "software request");
      end
      if (it.ot_req === 1'b1 && it.ot_start === 1'b1 && !p_ot) begin
        rq_op[OT] = it.ot_op; rq_num[OT] = it.ot_num; rq_reason[OT] = 0;
        judge(eg2_rdwr_request(h, OT, it.ot_op, it.ot_part, it.ot_num, it.ot_addr, 2), "OTP request");
      end
      if (it.lc_req === 1'b1 && it.lc_start === 1'b1 && !p_lc) begin
        rq_op[LC] = it.lc_op; rq_num[LC] = it.lc_num; rq_reason[LC] = 0;
        judge(eg2_rdwr_request(h, LC, it.lc_op, it.lc_part, it.lc_num, it.lc_addr, it.lc_phase), "lcmgr request");
      end
      if (eg2_rdwr_pending(h, SW) && (eg2_rdwr_pending(h, OT) || eg2_rdwr_pending(h, LC))) rq_contended[SW] = 1;
      // 2. pushes (before the write requests: the FIFO passes a word through when empty)
      if (it.sw_wvalid === 1'b1 && it.sw_wready === 1'b1) begin
        if (!eg2_rdwr_pending(h, SW)) n_stray++;
        eg2_rdwr_push(h, SW, it.sw_wdata);
      end
      if (it.ot_wvalid === 1'b1 && it.ot_wready === 1'b1) eg2_rdwr_push(h, OT, it.ot_wdata);
      if (it.lc_wvalid === 1'b1 && it.lc_wready === 1'b1) eg2_rdwr_push(h, LC, it.lc_wdata);
      // 3. reads: a new request (raised, or still high after an answer), then its answer
      if (it.rd_req === 1'b1 && (!p_rd_req || p_rd_done)) begin
        if (eg2_rdwr_on(h) == 0) sample_arb(it);
        if (it.rd_ovfl === 1'b1) n_wrap++;
        judge(eg2_rdwr_rd_req(h, it.rd_addr, it.rd_ovfl, it.if_sel, it.part, it.phase), "RRAM read request");
      end else if (p_rd_req && !p_rd_done && it.rd_req !== 1'b1) begin
        judge(2, "RRAM read request withdrawn before its answer");
      end
      if (it.rd_req === 1'b1 && it.rd_done === 1'b1)
        judge(eg2_rdwr_rd_rsp(h, it.rd_data, it.rd_err === 1'b1, it.rd_mp_err === 1'b1), "RRAM read answer");
      // 4. words delivered
      if (it.sw_rvalid === 1'b1 && it.sw_rready === 1'b1) begin
        d = it.sw_rdata;
        if (inject_fail && !injected) begin
          d[0] = ~d[0];
          injected = 1;
        end
        judge(eg2_rdwr_deliver(h, SW, d, 1), "word to the software read FIFO");
      end
      if (it.sw_rvalid === 1'b1 && it.sw_rready !== 1'b1) n_rdq_stall++;
      if (it.lc_rvalid === 1'b1 && it.lc_rready === 1'b1)
        judge(eg2_rdwr_deliver(h, LC, it.lc_rdata, 1), "word to lcmgr");
      if (it.ot_rvalid === 1'b1 && it.ot_rready === 1'b1)
        judge(eg2_rdwr_deliver(h, OT, 0, 0), "word to the OTP plug");
      // 5. writes
      if (it.wr_req === 1'b1 && (!p_wr_req || p_wr_done)) begin
        if (eg2_rdwr_on(h) == 0) sample_arb(it);
        if (it.wr_ovfl === 1'b1) n_wrap++;
        judge(eg2_rdwr_wr_req(h, it.wr_addr, it.wr_ovfl, it.wr_data, it.wr_last, it.if_sel, it.part, it.phase),
              "RRAM write request");
      end else if (p_wr_req && !p_wr_done && it.wr_req !== 1'b1) begin
        judge(2, "RRAM write request withdrawn before its answer");
      end
      if (it.wr_req === 1'b1 && it.wr_done === 1'b1)
        judge(eg2_rdwr_wr_rsp(h, it.wr_mp_err === 1'b1, it.wr_intg_err === 1'b1), "RRAM write answer");
      // 6. completions
      if (it.sw_done === 1'b1) completion(SW, it.sw_err, it.err_addr);
      if (it.ot_done === 1'b1) completion(OT, it.ot_err, 0);
      if (it.lc_done === 1'b1) completion(LC, it.lc_err, 0);
      // an error code before the completion ("set when the operation completes")
      if ((it.sw_err != 0 && it.sw_done !== 1'b1) || (it.lc_err != 0 && it.lc_done !== 1'b1) ||
          (it.ot_err != 0 && it.ot_done !== 1'b1)) begin
        n_err_early++;
        if (strict_early) judge(2, "an error code without the completion");
      end
      // faults: none may come from regular operation
      if (it.fsm_err === 1'b1 || it.cnt_err === 1'b1 || it.fifo_err === 1'b1)
        judge(2, $sformatf("fault flagged: fsm %b cnt %b fifo %b", it.fsm_err, it.cnt_err, it.fifo_err));
      if (!(it.rd_req === 1'b1 || it.rd_req === 1'b0) && it.phy_init_done === 1'b1) judge(2, "rd_req unknown");

    endfunction

    function void sample_arb(rram_ctrl_rdwr_cp_item it);
      cv_sel = it.if_sel;
      cv_contended = 0;
      if ((it.if_sel == 2 || it.if_sel == 3) && eg2_rdwr_pending(h, SW)) cv_contended = 1;
      if (eg2_rdwr_pending(h, OT) && eg2_rdwr_pending(h, LC)) cv_contended = 2;
      cg_arb.sample();
    endfunction

    function void completion(int who, bit [3:0] err, bit [18:0] err_addr);
      bit was_rejected = who == SW && eg2_rdwr_who(h) != SW;
      int r = eg2_rdwr_done(h, who, err, err_addr);
      if (r != 0 && eg2_rdwr_known_erraddr(h) && !strict_erraddr) n_erraddr_last++;
      else judge(r, $sformatf("completion to requester %0d", who));
      cv_who = who; cv_op = rq_op[who]; cv_err = err; cv_num = rq_num[who];
      cg_done.sample();
      if (was_rejected) begin
        cv_reason = rq_reason[who];
        cg_reject.sample();
      end
    endfunction

    virtual function void check_phase(uvm_phase phase);
      string e;
      super.check_phase(phase);
      e = eg2_rdwr_end(h);
      if (e != "") judge(2, {"at the end: ", e});
      if (eg2_rdwr_stat(h, 7) > 0) begin
        `uvm_info(get_type_name(), $sformatf("%0d request(s) never answered after rram_disable (finding)", eg2_rdwr_stat(h, 7)), UVM_LOW)
        if (strict_dis) judge(2, "requests never answered after rram_disable");
      end
      if (rdwr_ctx::mode == ModeDisable && rdwr_ctx::dis_cycle < 0) `uvm_error(get_type_name(), "rram_disable never driven")
      if (rdwr_ctx::mode != ModeDisable && eg2_rdwr_stat(h, 1) < (rdwr_ctx::mode == ModeLong ? 8 : 40))
        `uvm_error(get_type_name(), $sformatf("too little checked: %0d completions", eg2_rdwr_stat(h, 1)))
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: nothing was flipped")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      super.report_phase(phase);
      `uvm_info(get_type_name(), $sformatf({"checked against the model: %0d completions (%0d rejected, ",
          "%0d with an error), %0d RRAM reads, %0d RRAM writes; begun: software %0d/%0d/%0d, OTP %0d/%0d, ",
          "lcmgr %0d/%0d (read/write/Rewrite); %0d cycles with software waiting behind a plug, %0d with ",
          "both plugs waiting; %0d stray words (%0d cycles a stray word waited), %0d read-FIFO stalls, %0d wrapped addresses; %0d cycles with ",
          "an error code before the completion (finding); %0d completions with a stale ERR_ADDR, the failing word the last (finding); %0d checks, %0d failed"},
          eg2_rdwr_stat(h, 1), eg2_rdwr_stat(h, 0), eg2_rdwr_stat(h, 4), eg2_rdwr_stat(h, 2), eg2_rdwr_stat(h, 3),
          eg2_rdwr_stat(h, 8), eg2_rdwr_stat(h, 9), eg2_rdwr_stat(h, 10), eg2_rdwr_stat(h, 11), eg2_rdwr_stat(h, 12),
          eg2_rdwr_stat(h, 14), eg2_rdwr_stat(h, 15), eg2_rdwr_stat(h, 5), eg2_rdwr_stat(h, 6), n_stray, n_stray_stall,
          n_rdq_stall, n_wrap, n_err_early, n_erraddr_last, n_checks, n_err), UVM_LOW)
      if (h != null) eg2_rdwr_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_rdwr_cmodel_random_test extends rram_ctrl_rdwr_base_test;
    `uvm_component_utils(rram_ctrl_rdwr_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rdwr_ctx::mode = ModeRandom;
    endfunction
    virtual function void build_phase(uvm_phase phase);
      set_mode();
      rram_ctrl_rdwr_scoreboard::type_id::set_type_override(rram_ctrl_rdwr_cmodel_scoreboard::get_type());
      test_seq = "rram_ctrl_rdwr_cmodel_vseq";
      super.build_phase(phase);
    endfunction
  endclass

`define RDWR_MODE_TEST(NAME, MODE) \
  class rram_ctrl_rdwr_cmodel_``NAME``_test extends rram_ctrl_rdwr_cmodel_random_test; \
    `uvm_component_utils(rram_ctrl_rdwr_cmodel_``NAME``_test) \
    function new(string name, uvm_component parent); \
      super.new(name, parent); \
    endfunction \
    virtual function void set_mode(); \
      rdwr_ctx::mode = MODE; \
    endfunction \
  endclass

  `RDWR_MODE_TEST(err, ModeErr)
  `RDWR_MODE_TEST(contend, ModeContend)
  `RDWR_MODE_TEST(gate, ModeGate)
  `RDWR_MODE_TEST(long, ModeLong)
  `RDWR_MODE_TEST(disable, ModeDisable)
  // Must fail while the finding is open: software pushes its write words as soon as START is sent,
  // while the plugs hold the arbiter.
  `RDWR_MODE_TEST(swpush_race, ModeRace)

  // Must fail: the scoreboard flips one bit of the first word delivered to software.
  class rram_ctrl_rdwr_cmodel_random_inject_fail_test extends rram_ctrl_rdwr_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_rdwr_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass
endpackage
