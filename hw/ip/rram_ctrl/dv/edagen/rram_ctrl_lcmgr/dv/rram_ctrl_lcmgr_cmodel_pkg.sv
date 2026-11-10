// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The life-cycle manager unit bench's own part (edagen rram_ctrl_lcmgr.yaml dv.files), on edagen's
// generated skeleton (agent ok on clk_otp_i, agent ct on clk_i). One scenario per run (the bench
// resets the unit once, at time 0):
//   - rram_ctrl_lcmgr_cmodel_vseq, three threads:
//       software / life cycle (ct): lc_seed_hw_rd_en On, Off or a glitched multibit value; INIT.VAL;
//         rma_req On (and glitched values that must not count), rma_seed; rram_disable at a random
//         point; at the end the life-cycle inputs go back to "earlier" values, which must change
//         nothing (sticky decisions);
//       otp_ctrl's key side (ok): answers each key request (req held until ack, a one-cycle ack)
//         with a random key and rand_key that stay until the next ack;
//       the controller path (ct): rram_ctrl_arb / rram_ctrl_rd / rram_ctrl_wr as rram_ctrl's
//         documents describe them, on a bench memory of the RRAM (the seed pages filled with seeds,
//         RMA writes stored, reads returned with TL-UL integrity), with the scenario's faults: a
//         seed read error, a discrepancy between the two seed reads, a seed word with broken
//         integrity, an RMA write or read error, a corrupted RMA read-back, an mp_err.
//   - rram_ctrl_lcmgr_cmodel_scoreboard: every controller request, every seed word, every RMA word
//     and every output change against the lcmgr reference model (eg2_rram_lcmgr_core.h through
//     eg2_rram_lcmgr_dpi.cc): keys, init_done, seeds and seeds_valid, seed_err (bounds), intg_err,
//     rma_sw_dis / rma_dis_access / rma_ack (multibit values only On or Off, and sticky once On),
//     fatal_err; the end-of-run state; the functional coverage.
//   - tests: rram_ctrl_lcmgr_cmodel_random_test (any scenario; an RMA there gets a fault early, so it
//     is short); rram_ctrl_lcmgr_cmodel_rma_test (a full RMA wipe, with or without a late fault);
//     rram_ctrl_lcmgr_cmodel_random_inject_fail_test (one prediction corrupted: must fail).
package rram_ctrl_lcmgr_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import rram_ctrl_lcmgr_ok_agent_pkg::*;
  import rram_ctrl_lcmgr_ct_agent_pkg::*;
  import rram_ctrl_lcmgr_env_pkg::*;
  import rram_ctrl_lcmgr_test_pkg::*;

  import "DPI-C" function chandle eg2_lcmgr_create();
  import "DPI-C" function void eg2_lcmgr_release(chandle h);
  import "DPI-C" function string eg2_lcmgr_last_error(chandle h);
  import "DPI-C" function void eg2_lcmgr_init(chandle h);
  import "DPI-C" function void eg2_lcmgr_lc_seed(chandle h, int v);
  import "DPI-C" function void eg2_lcmgr_rma_req(chandle h, int v);
  import "DPI-C" function void eg2_lcmgr_disable(chandle h, int v);
  import "DPI-C" function int eg2_lcmgr_key_req(chandle h, int kind);
  import "DPI-C" function void eg2_lcmgr_key_ack(chandle h, int kind, int k0, int k1, int k2,
      int k3, int r0, int r1, int r2, int r3);
  import "DPI-C" function void eg2_lcmgr_provisioned_was(chandle h, int p);
  import "DPI-C" function int eg2_lcmgr_rma_started(chandle h);
  import "DPI-C" function int eg2_lcmgr_request(chandle h, int op, int part, int byte_addr,
      int num_words, int phase);
  import "DPI-C" function void eg2_lcmgr_seed_word(chandle h, int idx, int data, int intg_ok);
  import "DPI-C" function void eg2_lcmgr_seed_done(chandle h, int err);
  import "DPI-C" function void eg2_lcmgr_seed_err_now(chandle h, int err);
  import "DPI-C" function void eg2_lcmgr_rma_write(chandle h, int part, int word_addr);
  import "DPI-C" function void eg2_lcmgr_rma_read(chandle h, int part, int word_addr, int same);
  import "DPI-C" function void eg2_lcmgr_rma_error(chandle h);
  import "DPI-C" function int eg2_lcmgr_rma_ack_seen(chandle h);
  import "DPI-C" function void eg2_lcmgr_rma_fail_seen(chandle h);
  import "DPI-C" function string eg2_lcmgr_wipe_incomplete(chandle h);
  import "DPI-C" function int eg2_lcmgr_state(chandle h);
  import "DPI-C" function string eg2_lcmgr_state_name(chandle h);
  import "DPI-C" function int eg2_lcmgr_keys_acked(chandle h);
  import "DPI-C" function int eg2_lcmgr_key_word(chandle h, int which, int i);
  import "DPI-C" function int eg2_lcmgr_provisioned(chandle h);
  import "DPI-C" function int eg2_lcmgr_seed_evaluated(chandle h, int s);
  import "DPI-C" function int eg2_lcmgr_seed_valid(chandle h, int s);
  import "DPI-C" function int eg2_lcmgr_seed_word_of(chandle h, int s, int i);
  import "DPI-C" function int eg2_lcmgr_seed_err_lo(chandle h);
  import "DPI-C" function int eg2_lcmgr_seed_err_hi(chandle h);
  import "DPI-C" function int eg2_lcmgr_intg_err(chandle h);
  import "DPI-C" function int eg2_lcmgr_init_done_allowed(chandle h);
  import "DPI-C" function int eg2_lcmgr_rma_ever(chandle h);
  import "DPI-C" function int eg2_lcmgr_rma_failed(chandle h);
  import "DPI-C" function int eg2_lcmgr_disabled(chandle h);
  import "DPI-C" function int eg2_lcmgr_rma_requested(chandle h);

  localparam bit [3:0] LcOn = 4'h5, LcOff = 4'ha;
  localparam bit [3:0] MuBi4True = 4'h6, MuBi4False = 4'h9;
  localparam int unsigned PageWords = 128, PageBytes = 512;
  localparam int unsigned CreatorPage = 5, OwnerPage = 6, IsolatedPage = 7;
  localparam int unsigned WipeDataPages = 4090;
  // Model states (Eg2RramLcmgrCore::State).
  localparam int StIdle = 0, StKeyAddr = 1, StKeyData = 2, StSeeds = 3, StWait = 4, StRma = 5,
                 StRmaRsp = 6, StRmaFail = 7, StDisabled = 8;
  // Inputs need this many cycles before the unit's reaction counts as late or early (the lc_tx
  // synchronizers and the one-cycle FSM steps; theory_of_operation.md gives no cycle timing).
  localparam int unsigned Settle = 8;

  typedef enum int {
    SeedOk, SeedMismatch, SeedRdErr, SeedIntg
  } seed_fault_e;
  typedef enum int {
    RmaNone, RmaWrErr, RmaRdErr, RmaReadback, RmaMpErr
  } rma_fault_e;
  typedef enum int {
    ScnInit, ScnInitFault, ScnRmaIdle, ScnRmaWait, ScnRmaKeys, ScnRmaDuringSeeds, ScnGlitchOnly,
    ScnDisable
  } scn_e;
  typedef enum int {
    DisNone, DisIdle, DisKeys, DisSeeds, DisWait, DisRma, DisAfterAck
  } dis_e;

  function automatic bit [38:0] bus_word(bit [31:0] d);
    return prim_secded_pkg::prim_secded_inv_39_32_enc(d);
  endfunction

  function automatic bit [3:0] glitch4(bit [3:0] a, bit [3:0] b);
    bit [3:0] v;
    do v = 4'($urandom()); while (v == a || v == b);
    return v;
  endfunction

  // ---------------------------------------------------------------------------------------------
  // What the stimulus, the controller-path model and the scoreboard share.
  class lcb;
    static bit [31:0] mem[int unsigned];   // {part, word address} -> data
    static scn_e scn;
    static bit full_rma;                   // the RMA test: a full wipe
    static bit [3:0] seed_en;              // lc_seed_hw_rd_en as driven at the key decision
    static seed_fault_e seed_fault[2];
    static int unsigned seed_fault_pass[2], seed_fault_word[2];
    static rma_fault_e rma_fault;
    static int unsigned rma_fault_op;      // the n-th RMA operation gets the fault
    static dis_e dis_at;
    // Progress the threads report.
    static int unsigned n_ops, n_rma_ops, n_seed_ops, n_reads, n_writes, n_words_rd, n_words_wr;
    static int unsigned seed_pass_cnt[2];
    static bit rma_fault_applied, seed_fault_applied[2];
    static bit [127:0] key[2], rand_key[2];   // the last key answered per kind (0 addr, 1 data)
    static int unsigned n_key_acks;
    static string trace[$];

    static function int unsigned wa(bit part, int unsigned byte_addr);
      return {part, 31'(byte_addr / 4)};
    endfunction

    static function bit [31:0] rd(bit part, int unsigned byte_addr);
      int unsigned k = wa(part, byte_addr);
      if (!mem.exists(k)) mem[k] = $urandom();
      return mem[k];
    endfunction

    static function void log(string s);
      trace.push_back($sformatf("%0t %s", $time, s));
      if (trace.size() > 16) void'(trace.pop_front());
    endfunction

    static function string last_ops();
      string s = "";
      foreach (trace[i]) s = {s, "\n    ", trace[i]};
      return s;
    endfunction
  endclass

  typedef class rram_ctrl_lcmgr_cmodel_scoreboard;

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_lcmgr_cmodel_vseq extends rram_ctrl_lcmgr_base_vseq;
    `uvm_object_utils(rram_ctrl_lcmgr_cmodel_vseq)

    virtual rram_ctrl_lcmgr_ok_if ov;
    virtual rram_ctrl_lcmgr_ct_if cv;
    rram_ctrl_lcmgr_cmodel_scoreboard sb;
    bit stop;

    function new(string name = "rram_ctrl_lcmgr_cmodel_vseq");
      super.new(name);
    endfunction

    // ---- otp_ctrl's key side ----
    task ok_thread();
      ov.addr_ack <= 0;
      ov.data_ack <= 0;
      ov.key <= '0;
      ov.rand_key <= '0;
      ov.seed_valid <= 0;
      forever begin
        int kind;
        @(posedge ov.clk);
        if (stop) return;
        if (ov.addr_req === 1'b1) kind = 0;
        else if (ov.data_req === 1'b1) kind = 1;
        else continue;
        // The scenario may hold one key handshake to raise rma_req in that key state.
        while (sb.hold_keys && kind == sb.hold_kind) @(posedge ov.clk);
        repeat ($urandom_range(0, 12)) @(posedge ov.clk);
        lcb::key[kind] = {$urandom(), $urandom(), $urandom(), $urandom()};
        lcb::rand_key[kind] = {$urandom(), $urandom(), $urandom(), $urandom()};
        ov.key <= lcb::key[kind];
        ov.rand_key <= lcb::rand_key[kind];
        ov.seed_valid <= $urandom_range(0, 3) != 0;
        if (kind == 0) ov.addr_ack <= 1; else ov.data_ack <= 1;
        lcb::n_key_acks++;
        @(posedge ov.clk);
        ov.addr_ack <= 0;
        ov.data_ack <= 0;
        // key and rand_key stay as they are until the next acknowledgement.
      end
    endtask

    // ---- the controller path ----
    task ct_idle();
      cv.done <= 0;
      cv.err <= '0;
      cv.rvalid <= 0;
      cv.rdata <= '0;
      cv.wready <= 0;
    endtask

    task ct_thread();
      ct_idle();
      forever begin
        bit [1:0] op;
        bit part;
        int unsigned base, nw, page, seed_s, pass;
        bit [3:0] err;
        bit seed_op, rma_op, fault_here;
        int unsigned fail_at;
        @(posedge cv.clk);
        if (stop) return;
        if (!(cv.req === 1'b1 && cv.start === 1'b1)) continue;
        op = cv.op;
        part = cv.part;
        base = cv.addr;
        nw = cv.num + 1;
        page = base / PageBytes;
        lcb::n_ops++;
        err = '0;
        seed_op = cv.phase == 2'd0;
        rma_op = cv.phase == 2'd1;
        seed_s = page == OwnerPage ? 1 : 0;
        pass = seed_op ? lcb::seed_pass_cnt[seed_s] : 0;
        fault_here = 0;
        if (rma_op) begin
          lcb::n_rma_ops++;
          fault_here = lcb::rma_fault != RmaNone && !lcb::rma_fault_applied &&
                       lcb::n_rma_ops >= lcb::rma_fault_op &&
                       ((lcb::rma_fault == RmaWrErr && op == 2'd1) ||
                        ((lcb::rma_fault == RmaRdErr || lcb::rma_fault == RmaReadback) && op == 2'd0) ||
                        lcb::rma_fault == RmaMpErr);
          if (fault_here) lcb::rma_fault_applied = 1;
          if (fault_here && lcb::rma_fault == RmaMpErr) err[2] = 1;
        end
        if (seed_op) begin
          lcb::n_seed_ops++;
          fault_here = lcb::seed_fault[seed_s] != SeedOk && pass == lcb::seed_fault_pass[seed_s];
          if (fault_here) lcb::seed_fault_applied[seed_s] = 1;
          lcb::seed_pass_cnt[seed_s]++;
        end
        lcb::log($sformatf("op %0d part %0d addr %06h words %0d phase %0d%s", op, part, base, nw,
                           cv.phase, fault_here ? " (fault)" : ""));
        // An mp_err still runs the operation's words (lcmgr waits for every word and for done; the
        // read data is all ones, as for a read error), with the error on done.
        case (op)
          2'd0: begin  // read: NUM + 1 bus words, done with the last
            lcb::n_reads++;
            fail_at = err[2] ? 0 : nw;
            if (fault_here && ((seed_op && lcb::seed_fault[seed_s] == SeedRdErr) ||
                               (rma_op && lcb::rma_fault == RmaRdErr))) begin
              fail_at = seed_op ? lcb::seed_fault_word[seed_s] % nw : $urandom_range(0, nw - 1);
              err[1] = 1;
            end
            for (int unsigned i = 0; i < nw; i++) begin
              bit [31:0] d;
              bit [38:0] w;
              while ($urandom_range(0, 3) == 0) begin
                cv.rvalid <= 0;
                @(posedge cv.clk);
              end
              d = i >= fail_at ? '1 : lcb::rd(part, base + 4 * i);
              if (fault_here && seed_op && lcb::seed_fault[seed_s] == SeedMismatch &&
                  i == lcb::seed_fault_word[seed_s] % nw)
                d[$urandom_range(0, 31)] ^= 1'b1;
              if (fault_here && rma_op && lcb::rma_fault == RmaReadback && i == nw / 2)
                d[$urandom_range(0, 31)] ^= 1'b1;
              w = bus_word(d);
              if (fault_here && seed_op && lcb::seed_fault[seed_s] == SeedIntg &&
                  i == lcb::seed_fault_word[seed_s] % nw)
                w[32 + $urandom_range(0, 6)] ^= 1'b1;
              cv.rvalid <= 1;
              cv.rdata <= w;
              if (i == nw - 1) begin
                cv.done <= 1;
                cv.err <= err;
              end
              @(posedge cv.clk);
              while (cv.rready !== 1'b1) @(posedge cv.clk);
              lcb::n_words_rd++;
            end
            ct_idle();
          end
          2'd1: begin  // write: words taken with wvalid and wready, done after the last
            int unsigned taken = 0;
            lcb::n_writes++;
            if (fault_here && lcb::rma_fault == RmaWrErr) err[0] = 1;
            while (taken < nw) begin
              cv.wready <= $urandom_range(0, 4) != 0;
              @(posedge cv.clk);
              if (stop) return;
              if (cv.wready === 1'b1 && cv.wvalid === 1'b1) begin
                if (cv.wdata !== bus_word(cv.wdata[31:0]))
                  `uvm_error("lcmgr_ct", $sformatf("write word %0d with broken bus integrity: %010h",
                                                   taken, cv.wdata))
                if (err == 0) lcb::mem[lcb::wa(part, base + 4 * taken)] = cv.wdata[31:0];
                taken++;
                lcb::n_words_wr++;
              end
            end
            cv.wready <= 0;
            cv.done <= 1;
            cv.err <= err;
            @(posedge cv.clk);
            ct_idle();
          end
          default: begin  // not an operation lcmgr uses: done with op_err
            cv.done <= 1;
            cv.err <= 4'b1000;
            @(posedge cv.clk);
            ct_idle();
          end
        endcase
      end
    endtask

    // ---- software and life cycle ----
    task wait_state(int st, int unsigned max_cycles);
      for (int unsigned i = 0; i < max_cycles && sb.mstate() != st; i++) @(posedge cv.clk);
    endtask

    task raise_rma();
      // Now and then glitched values first: none of them may start an RMA.
      repeat ($urandom_range(0, 2)) begin
        cv.rma_req <= glitch4(LcOn, LcOff);
        repeat ($urandom_range(5, 40)) @(posedge cv.clk);
      end
      cv.rma_req <= LcOn;
    endtask

    task disable_now();
      cv.rram_dis <= $urandom_range(0, 3) == 0 ? glitch4(MuBi4True, MuBi4False) : MuBi4True;
    endtask

    virtual task body();
      int unsigned max_wait;
      ov = cfg.ok_cfg.vif;
      cv = cfg.ct_cfg.vif;
      if (!$cast(sb, uvm_root::get().find("uvm_test_top.env.scoreboard")))
        `uvm_fatal("lcmgr_vseq", "scoreboard is not rram_ctrl_lcmgr_cmodel_scoreboard")
      choose_scenario();
      // The bench memory: the seed pages hold seeds, the isolated page random data.
      for (int s = 0; s < 2; s++)
        for (int unsigned i = 0; i < PageWords; i++)
          lcb::mem[lcb::wa(1, (s == 0 ? CreatorPage : OwnerPage) * PageBytes + 4 * i)] = $urandom();
      // Inputs defined while in reset.
      cv.init <= 0;
      cv.seed_hw_rd_en <= lcb::seed_en;
      cv.rma_req <= LcOff;
      cv.rma_seed <= {$urandom(), $urandom()};
      cv.rram_dis <= MuBi4False;
      ct_idle();
      super.body();
      wait (cv.rst_n === 1'b1 && ov.rst_n === 1'b1);
      fork
        ct_thread();
        ok_thread();
      join_none
      repeat ($urandom_range(2, 200)) @(posedge cv.clk);
      max_wait = lcb::full_rma ? 40_000_000 : 400_000;
      case (lcb::scn)
        ScnRmaIdle: begin
          raise_rma();
        end
        ScnGlitchOnly: begin
          repeat (4) begin
            cv.rma_req <= glitch4(LcOn, LcOff);
            repeat ($urandom_range(5, 60)) @(posedge cv.clk);
          end
          cv.init <= 1;
        end
        ScnRmaKeys: begin
          sb.hold_kind = $urandom_range(0, 1);
          sb.hold_keys = 1;
          cv.init <= 1;
          wait_state(sb.hold_kind == 0 ? StKeyAddr : StKeyData, 4000);
          repeat ($urandom_range(20, 60)) @(posedge cv.clk);
          raise_rma();
          repeat (Settle * 4) @(posedge cv.clk);
          sb.hold_keys = 0;
        end
        default: cv.init <= 1;
      endcase
      // Mid-run events.
      if (lcb::scn == ScnRmaDuringSeeds) begin
        wait_state(StSeeds, 4000);
        repeat ($urandom_range(0, 30)) @(posedge cv.clk);
        raise_rma();
      end
      if (lcb::scn == ScnRmaWait) begin
        wait_state(StWait, 20000);
        repeat ($urandom_range(5, 100)) @(posedge cv.clk);
        raise_rma();
      end
      if (lcb::dis_at != DisNone) begin
        case (lcb::dis_at)
          DisKeys: wait_state(StKeyData, 4000);
          DisSeeds: wait_state(StSeeds, 4000);
          DisWait: wait_state(StWait, 20000);
          DisRma: begin wait_state(StRma, 40000); repeat ($urandom_range(10, 3000)) @(posedge cv.clk); end
          DisAfterAck: begin  // after the RMA's end: the ack, or the error state
            for (int unsigned i = 0; i < max_wait && !(sb.mstate() inside {StRmaRsp, StRmaFail}) &&
                 !sb.dis_access_seen; i++) @(posedge cv.clk);
          end
          default: ;
        endcase
        repeat ($urandom_range(0, 40)) @(posedge cv.clk);
        disable_now();
      end
      // Run to the end state.
      for (int unsigned i = 0; i < max_wait; i++) begin
        @(posedge cv.clk);
        if (sb.settled_end()) break;
      end
      repeat (100) @(posedge cv.clk);
      // "Earlier" life-cycle values: they must change nothing (sticky decisions).
      cv.rma_req <= $urandom_range(0, 1) ? LcOff : glitch4(LcOn, LcOff);
      cv.seed_hw_rd_en <= $urandom_range(0, 1) ? LcOff : glitch4(LcOn, LcOff);
      if (lcb::dis_at == DisNone && $urandom_range(0, 3) == 0) cv.rram_dis <= MuBi4False;
      repeat (300) @(posedge cv.clk);
      cv.seed_hw_rd_en <= LcOn;
      repeat (300) @(posedge cv.clk);
      stop = 1;
      repeat (5) @(posedge cv.clk);
      sb.end_of_run();
    endtask

    function void choose_scenario();
      int r;
      string s;
      lcb::full_rma = sb.full_rma;
      if (lcb::full_rma) begin
        lcb::scn = $urandom_range(0, 1) ? ScnRmaIdle : ScnRmaWait;
      end else begin
        r = $urandom_range(0, 99);
        lcb::scn = r < 25 ? ScnInit : r < 45 ? ScnInitFault : r < 55 ? ScnRmaIdle :
                   r < 65 ? ScnRmaWait : r < 72 ? ScnRmaKeys : r < 80 ? ScnRmaDuringSeeds :
                   r < 87 ? ScnGlitchOnly : ScnDisable;
      end
      if (sb.inject_fail) begin
        lcb::scn = ScnInit;  // valid seeds to corrupt
      end
      if ($value$plusargs("scn=%s", s))
        foreach (scn_names[i]) if (scn_names[i] == s) lcb::scn = scn_e'(i);
      r = $urandom_range(0, 99);
      lcb::seed_en = r < 65 ? LcOn : r < 85 ? LcOff : glitch4(LcOn, LcOff);
      if (lcb::scn == ScnRmaDuringSeeds || sb.inject_fail) lcb::seed_en = LcOn;
      begin
        int e;
        if ($value$plusargs("seed_en=%h", e)) lcb::seed_en = 4'(e);
      end
      foreach (lcb::seed_fault[i]) begin
        lcb::seed_fault[i] = SeedOk;
        lcb::seed_fault_pass[i] = $urandom_range(0, 1);
        lcb::seed_fault_word[i] = $urandom_range(0, 7);
      end
      if (lcb::scn == ScnInitFault) begin
        lcb::seed_en = LcOn;
        lcb::seed_fault[$urandom_range(0, 1)] = seed_fault_e'($urandom_range(1, 3));
        if ($urandom_range(0, 2) == 0) lcb::seed_fault[$urandom_range(0, 1)] = seed_fault_e'($urandom_range(1, 3));
      end
      // An RMA in the random test gets a fault within its first operations (short runs); in the
      // RMA test half of the runs wipe everything, the other half fail late.
      lcb::rma_fault = RmaNone;
      if (lcb::scn inside {ScnRmaIdle, ScnRmaWait, ScnRmaKeys, ScnRmaDuringSeeds}) begin
        // Only the RMA test judges an RMA fault: the error state comes after the full wipe.
        if (lcb::full_rma && $urandom_range(0, 1)) begin
          lcb::rma_fault = rma_fault_e'($urandom_range(1, 4));
          lcb::rma_fault_op = $urandom_range(2, 8000);  // a full wipe has 8186 operations
        end
      end
      lcb::dis_at = DisNone;
      if (lcb::scn == ScnDisable) lcb::dis_at = dis_e'($urandom_range(1, 3));
      else if (!lcb::full_rma && !sb.inject_fail && $urandom_range(0, 9) == 0) lcb::dis_at = dis_e'($urandom_range(1, 4));
      if (lcb::full_rma && lcb::rma_fault == RmaNone && $urandom_range(0, 2) == 0) lcb::dis_at = DisAfterAck;
      begin
        int d;
        if ($value$plusargs("dis=%d", d)) lcb::dis_at = dis_e'(d);
        if ($value$plusargs("rma_fault=%d", d)) begin
          lcb::rma_fault = rma_fault_e'(d);
          if (d == 0) lcb::dis_at = lcb::dis_at;
        end
      end
      `uvm_info("lcmgr_vseq", $sformatf("scenario %s, lc_seed_hw_rd_en %h, seed faults %s/%s, RMA fault %s at op %0d, disable %s",
                                        lcb::scn.name(), lcb::seed_en, lcb::seed_fault[0].name(),
                                        lcb::seed_fault[1].name(), lcb::rma_fault.name(), lcb::rma_fault_op,
                                        lcb::dis_at.name()), UVM_LOW)
    endfunction

    string scn_names[] = '{"init", "init_fault", "rma_idle", "rma_wait", "rma_keys",
                           "rma_during_seeds", "glitch_only", "disable"};
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_lcmgr_cmodel_scoreboard extends rram_ctrl_lcmgr_scoreboard;
    `uvm_component_utils(rram_ctrl_lcmgr_cmodel_scoreboard)

    chandle h;
    bit inject_fail, injected, full_rma;
    bit hold_keys;
    int hold_kind;
    int unsigned cyc, n_checks, n_err, n_req, n_seed_words, n_rma_wr, n_rma_rd, n_out_checks;
    // The last values seen, and when the inputs changed.
    rram_ctrl_lcmgr_ct_item prev;
    bit have_prev;
    int unsigned t_seed_en, t_rma_req, t_dis, t_init;
    bit init_seen, dis_seen, ok_prev_addr_req, ok_prev_data_req;
    // The operation in flight.
    bit op_on, op_seed, op_rma;
    bit [1:0] op_op;
    bit op_part;
    int unsigned op_base, op_nw, op_words;
    // Once-On outputs (sticky) and what was seen.
    bit sw_dis_seen, dis_access_seen, ack_seen, rma_started_seen, init_done_seen, intg_seen_dut;
    bit decision_checked;
    int unsigned t_rma_start;
    int unsigned t_ack;

    // Coverage.
    int c_scn, c_seed_en, c_seed_out0, c_seed_out1, c_rma_out, c_dis, c_glitch;
    covergroup cg_run;
      option.per_instance = 1;
      cp_scn: coverpoint c_scn { bins s[] = {[0:7]}; }
      cp_seed_en: coverpoint c_seed_en { bins on = {0}; bins off = {1}; bins glitch = {2}; }
      cp_seed0: coverpoint c_seed_out0 { bins not_read = {0}; bins valid = {1}; bins mismatch = {2};
                                         bins rd_err = {3}; bins intg = {4}; }
      cp_seed1: coverpoint c_seed_out1 { bins not_read = {0}; bins valid = {1}; bins mismatch = {2};
                                         bins rd_err = {3}; bins intg = {4}; }
      cp_rma: coverpoint c_rma_out { bins none = {0}; bins ack = {1}; bins wr_err = {2}; bins rd_err = {3};
                                     bins readback = {4}; bins mp_err = {5}; }
      cp_dis: coverpoint c_dis { bins d[] = {[0:6]}; }
      x_scn_dis: cross cp_scn, cp_dis { ignore_bins rare = binsof(cp_dis) intersect {[5:6]}; }
      x_en_seed0: cross cp_seed_en, cp_seed0;
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_run = new();
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      void'(uvm_config_db#(bit)::get(null, "", "full_rma", full_rma));
      h = eg2_lcmgr_create();
    endfunction

    function int mstate();
      return eg2_lcmgr_state(h);
    endfunction

    int unsigned n_msg[string];  // each distinct mismatch is reported three times, then counted
    function void fail(string what);
      n_err++;
      n_msg[what]++;
      if (n_msg[what] <= 3)
        `uvm_error(get_type_name(), $sformatf("%s [model state %s]%s", what, eg2_lcmgr_state_name(h),
                                              lcb::last_ops()))
    endfunction

    function void judge(int r, string what);
      n_checks++;
      if (r != 0) fail({what, ": ", eg2_lcmgr_last_error(h)});
    endfunction

    // ---- the OTP key side ----
    virtual function void predict_ok(rram_ctrl_lcmgr_ok_item item);
      rram_ctrl_lcmgr_ok_item it = item;
      // A request raised before rram_disable or the RMA entry still reaches the OTP side through the
      // req/ack synchronizer: within the settle window it is not judged.
      bit recent = (dis_seen && cyc <= t_dis + 3 * Settle) || (rma_started_seen && cyc <= t_rma_start + 3 * Settle);
      if (it.addr_req === 1'b1 && !ok_prev_addr_req && !recent) judge(eg2_lcmgr_key_req(h, 0), "address key request");
      if (it.data_req === 1'b1 && !ok_prev_data_req && !recent) judge(eg2_lcmgr_key_req(h, 1), "data key request");
      if (it.addr_ack === 1'b1 || it.data_ack === 1'b1) begin
        int kind = it.addr_ack === 1'b1 ? 0 : 1;
        eg2_lcmgr_key_ack(h, kind, it.key[31:0], it.key[63:32], it.key[95:64], it.key[127:96],
                          it.rand_key[31:0], it.rand_key[63:32], it.rand_key[95:64], it.rand_key[127:96]);
      end
      ok_prev_addr_req = it.addr_req === 1'b1;
      ok_prev_data_req = it.data_req === 1'b1;
    endfunction

    function bit [127:0] mkey(int which);
      return {eg2_lcmgr_key_word(h, which, 3), eg2_lcmgr_key_word(h, which, 2),
              eg2_lcmgr_key_word(h, which, 1), eg2_lcmgr_key_word(h, which, 0)};
    endfunction

    function bit [255:0] mseed(int s);
      bit [255:0] v;
      for (int i = 7; i >= 0; i--) v = {v[223:0], 32'(eg2_lcmgr_seed_word_of(h, s, i))};
      return v;
    endfunction

    // ---- the clk_i side, every cycle ----
    virtual function void predict_ct(rram_ctrl_lcmgr_ct_item item);
      rram_ctrl_lcmgr_ct_item it = item;
      cyc++;
      // Inputs to the model.
      if (it.init === 1'b1 && !init_seen) begin
        init_seen = 1;
        t_init = cyc;
        eg2_lcmgr_init(h);
      end
      if (!have_prev || it.seed_hw_rd_en !== prev.seed_hw_rd_en) begin
        eg2_lcmgr_lc_seed(h, it.seed_hw_rd_en);
        t_seed_en = cyc;
      end
      if (!have_prev || it.rma_req !== prev.rma_req) begin
        eg2_lcmgr_rma_req(h, it.rma_req);
        t_rma_req = cyc;
      end
      if (!have_prev || it.rram_dis !== prev.rram_dis) begin
        if (it.rram_dis !== MuBi4False && !dis_seen) begin
          dis_seen = 1;
          t_dis = cyc;
        end
        eg2_lcmgr_disable(h, it.rram_dis);
      end
      check_multibit(it);
      track_ops(it);
      check_outputs(it);
      prev = it;
      have_prev = 1;
    endfunction

    function void check_multibit(rram_ctrl_lcmgr_ct_item it);
      if (!(it.rma_ack inside {LcOn, LcOff})) fail($sformatf("rma_ack %h is neither On nor Off", it.rma_ack));
      if (!(it.rma_sw_dis inside {LcOn, LcOff})) fail($sformatf("rma_sw_dis %h is neither On nor Off", it.rma_sw_dis));
      if (!(it.rma_dis_access inside {LcOn, LcOff})) fail($sformatf("rma_dis_access %h is neither On nor Off", it.rma_dis_access));
      if (!(it.phase inside {2'd0, 2'd1, 2'd2, 2'd3})) fail("phase is X");
    endfunction

    function void rma_start_seen(string how);
      if (rma_started_seen) return;
      rma_started_seen = 1;
      t_rma_start = cyc;
      judge(eg2_lcmgr_rma_started(h), {"RMA entry (", how, ")"});
    endfunction

    function void track_ops(rram_ctrl_lcmgr_ct_item it);
      // A new operation.
      if (it.req === 1'b1 && it.start === 1'b1 && !op_on) begin
        n_req++;
        if (dis_seen && cyc > t_dis + Settle) fail("controller request after rram_disable");
        if (it.phase == 2'd1) rma_start_seen("first wipe request");
        // The seed/no-seed decision: lc_seed_hw_rd_en changed within the settle window before it.
        if (!decision_checked && it.phase == 2'd0) begin
          decision_checked = 1;
          if (!eg2_lcmgr_provisioned(h) && cyc - t_seed_en <= 2 * Settle) eg2_lcmgr_provisioned_was(h, 1);
        end
        judge(eg2_lcmgr_request(h, it.op, it.part, it.addr, it.num + 1, it.phase),
              $sformatf("request op %0d part %0d addr %06h words %0d phase %0d", it.op, it.part,
                        it.addr, it.num + 1, it.phase));
        op_on = 1;
        op_seed = it.phase == 2'd0;
        op_rma = it.phase == 2'd1;
        op_op = it.op;
        op_part = it.part;
        op_base = it.addr;
        op_nw = it.num + 1;
        op_words = 0;
      end
      if (!op_on) return;
      // Read words.
      if (it.rvalid === 1'b1 && it.rready === 1'b1 && op_op == 2'd0) begin
        bit [31:0] d = it.rdata[31:0];
        bit intg_ok = it.rdata === bus_word(d);
        if (op_seed) begin
          n_seed_words++;
          eg2_lcmgr_seed_word(h, op_words, d, intg_ok);
        end else if (op_rma) begin
          int unsigned k = lcb::wa(op_part, op_base + 4 * op_words);
          bit same = lcb::mem.exists(k) && lcb::mem[k] == d && intg_ok;
          n_rma_rd++;
          eg2_lcmgr_rma_read(h, op_part, (op_base + 4 * op_words) / 4, same);
        end
        op_words++;
      end
      // Write words.
      if (it.wvalid === 1'b1 && it.wready === 1'b1 && op_op == 2'd1) begin
        if (op_rma) begin
          n_rma_wr++;
          eg2_lcmgr_rma_write(h, op_part, (op_base + 4 * op_words) / 4);
        end
        op_words++;
      end
      if (it.done === 1'b1) begin
        if (op_seed) begin
          eg2_lcmgr_seed_err_now(h, it.err);
          eg2_lcmgr_seed_done(h, it.err);
        end
        if (op_rma && it.err != 0) eg2_lcmgr_rma_error(h);
        op_on = 0;
      end
    endfunction

    function void check_outputs(rram_ctrl_lcmgr_ct_item it);
      int st = mstate();
      bit late_dis = dis_seen && cyc > t_dis + Settle;
      n_out_checks++;
      // keys_valid and the keys.
      if (it.keys_valid === 1'b1) begin
        if (!eg2_lcmgr_keys_acked(h)) fail("keys_valid before both keys were acknowledged");
        else begin
          n_checks++;
          if (it.addr_key !== mkey(0)) fail($sformatf("addr_key %032h, OTP gave %032h", it.addr_key, mkey(0)));
          if (it.data_key !== mkey(1)) fail($sformatf("data_key %032h, OTP gave %032h", it.data_key, mkey(1)));
          if (it.rand_addr_key !== mkey(2)) fail("rand_addr_key differs from OTP's rand_key of the address key");
          if (it.rand_data_key !== mkey(3)) fail("rand_data_key differs from OTP's rand_key of the data key");
        end
      end
      // init_done.
      if (it.init_done === 1'b1 && !init_done_seen) begin
        init_done_seen = 1;
        // Not provisioned at the decision and no seed read: lc_seed_hw_rd_en may have changed
        // within the synchronizer window.
        if (eg2_lcmgr_provisioned(h) && !eg2_lcmgr_seed_evaluated(h, 0) && cyc - t_seed_en <= 4 * Settle &&
            lcb::n_seed_ops == 0)
          eg2_lcmgr_provisioned_was(h, 0);
        if (!eg2_lcmgr_init_done_allowed(h)) fail("init_done before the keys and the seed reads completed");
        if (it.keys_valid !== 1'b1 && !dis_seen) fail("init_done without keys_valid");
      end
      if (have_prev && prev.init_done === 1'b1 && it.init_done !== 1'b1) fail("init_done dropped before reset");
      // seeds_valid and the seeds (after the model has evaluated them).
      for (int s = 0; s < 2; s++) begin
        bit v = it.seeds_valid[s];
        bit exp_v = eg2_lcmgr_seed_valid(h, s);
        bit [255:0] seed = s == 0 ? it.creator_seed : it.owner_seed;
        if (v === 1'b1) begin
          if (!exp_v && !(dis_seen && !late_dis)) fail($sformatf("seeds_valid[%0d] although the model says not valid", s));
          else if (exp_v) begin
            bit [255:0] e = mseed(s);
            n_checks++;
            if (inject_fail && !injected) begin
              e[0] = !e[0];
              injected = 1;
            end
            if (seed !== e) fail($sformatf("seed %0d is %064h, the first read gave %064h", s, seed, e));
          end
        end
        if (it.init_done === 1'b1 && !late_dis && eg2_lcmgr_seed_evaluated(h, s) && st == StWait &&
            cyc > t_init + Settle && v !== exp_v)
          fail($sformatf("seeds_valid[%0d] is %b at init_done, the model says %b", s, v, exp_v));
        if (late_dis && v === 1'b1) fail($sformatf("seeds_valid[%0d] still set after rram_disable", s));
      end
      // seed_err (bounds), intg_err (sticky, only for a broken seed word).
      if (it.seed_err === 1'b1 && !eg2_lcmgr_seed_err_hi(h)) fail("seed_err without a seed read error, discrepancy or integrity error");
      if (it.init_done === 1'b1 && st == StWait && eg2_lcmgr_seed_err_lo(h) && it.seed_err !== 1'b1)
        fail("seed_err not set although a seed read failed or the two reads differed");
      if (it.intg_err === 1'b1) intg_seen_dut = 1;
      if (it.intg_err === 1'b1 && !eg2_lcmgr_intg_err(h)) fail("intg_err without a seed word with broken integrity");
      if (intg_seen_dut && it.intg_err !== 1'b1) fail("intg_err dropped before reset");
      // RMA outputs.
      if (it.rma_sw_dis == LcOn) begin
        rma_start_seen("rma_sw_dis On");
        sw_dis_seen = 1;
      end
      if (sw_dis_seen && it.rma_sw_dis != LcOn && it.rma_dis_access != LcOn)
        fail("rma_sw_dis went back to Off without rma_dis_access On (software access re-enabled before reset)");
      if (it.rma_dis_access == LcOn) begin
        if (!eg2_lcmgr_disabled(h) && !eg2_lcmgr_rma_failed(h)) fail("rma_dis_access On without rram_disable or an RMA error");
        dis_access_seen = 1;
        if (eg2_lcmgr_rma_failed(h)) eg2_lcmgr_rma_fail_seen(h);
      end
      if (dis_access_seen && it.rma_dis_access != LcOn) fail("rma_dis_access went back to Off before reset");
      if (it.rma_ack == LcOn && !ack_seen) begin
        ack_seen = 1;
        t_ack = cyc;
        judge(eg2_lcmgr_rma_ack_seen(h), "rma_ack On");
      end
      if (ack_seen && it.rma_ack != LcOn) fail("rma_ack went back to Off before reset");
      if (ack_seen && it.rma_sw_dis != LcOn) fail("rma_sw_dis not On together with rma_ack (StRmaRsp keeps it On)");
      if (late_dis && st != StRmaRsp && !ack_seen && it.rma_dis_access != LcOn)
        fail("rma_dis_access not On after rram_disable");
      if (it.rma_ack == LcOn && eg2_lcmgr_rma_failed(h)) fail("rma_ack On after an RMA error");
      // fatal_err: only in the RMA error state (documents: the invalid terminal state).
      if (it.fatal_err === 1'b1 && !eg2_lcmgr_rma_failed(h)) fail("fatal_err without an FSM, counter or RMA error");
      // Life-cycle values that are not On must not start an RMA.
      if (!eg2_lcmgr_rma_requested(h) && (it.rma_sw_dis == LcOn || (it.req === 1'b1 && it.phase == 2'd1)))
        fail("RMA activity without rma_req On");
    endfunction

    // The end condition the stimulus waits for.
    function bit settled_end();
      int st = mstate();
      if (ack_seen && cyc > t_ack + 200) return 1;
      if (!full_rma && rma_started_seen && n_rma_wr + n_rma_rd > 6000) return 1;
      if (dis_access_seen && (dis_seen || eg2_lcmgr_rma_failed(h))) return 1;
      if (init_done_seen && st == StWait && lcb::scn inside {ScnInit, ScnInitFault, ScnGlitchOnly, ScnDisable}
          && lcb::dis_at == DisNone) return 1;
      return 0;
    endfunction

    function void end_of_run();
      int st = mstate();
      n_checks++;
      // Whatever the scenario: the end state the documents give for it.
      if (eg2_lcmgr_rma_failed(h) && !dis_access_seen && (full_rma || !rma_started_seen))
        fail("RMA error, but rma_dis_access never went On");
      if (eg2_lcmgr_rma_failed(h) && ack_seen) fail("RMA error, but rma_ack On");
      if (lcb::full_rma && lcb::rma_fault == RmaNone && lcb::dis_at inside {DisNone, DisAfterAck} && !ack_seen)
        fail({"full RMA without a fault ended without rma_ack On: ", eg2_lcmgr_wipe_incomplete(h)});
      if (lcb::scn inside {ScnInit, ScnInitFault, ScnGlitchOnly} && lcb::dis_at == DisNone && !init_done_seen)
        fail("init_done never came");
      if (lcb::scn == ScnGlitchOnly && rma_started_seen) fail("an RMA started on glitched rma_req values");
      // Coverage of this run.
      c_scn = int'(lcb::scn);
      c_seed_en = lcb::seed_en == LcOn ? 0 : lcb::seed_en == LcOff ? 1 : 2;
      c_seed_out0 = seed_outcome(0);
      c_seed_out1 = seed_outcome(1);
      c_rma_out = ack_seen ? 1 : !lcb::rma_fault_applied ? 0 : 1 + int'(lcb::rma_fault);
      c_dis = int'(lcb::dis_at);
      if (cfg.en_cov) cg_run.sample();
    endfunction

    function int seed_outcome(int s);
      if (!eg2_lcmgr_seed_evaluated(h, s)) return 0;
      if (eg2_lcmgr_seed_valid(h, s)) return 1;
      if (!lcb::seed_fault_applied[s]) return 1;
      return lcb::seed_fault[s] == SeedMismatch ? 2 : lcb::seed_fault[s] == SeedRdErr ? 3 : 4;
    endfunction

    virtual function void check_phase(uvm_phase phase);
      super.check_phase(phase);
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: no seed check to corrupt (scenario without valid seeds)")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      super.report_phase(phase);
      `uvm_info(get_type_name(), $sformatf({"checked against the model: scenario %s, %0d cycles, %0d controller requests ",
          "(%0d seed, %0d RMA operations), %0d seed words, %0d RMA words written, %0d read back, %0d key acks; ",
          "end state %s, init_done %0b, rma_ack %0b, rma_sw_dis seen %0b, rma_dis_access seen %0b, intg_err %0b; ",
          "%0d checks, %0d output samples, %0d failed"},
          lcb::scn.name(), cyc, n_req, lcb::n_seed_ops, lcb::n_rma_ops, n_seed_words, n_rma_wr, n_rma_rd,
          lcb::n_key_acks, eg2_lcmgr_state_name(h), init_done_seen, ack_seen, sw_dis_seen, dis_access_seen,
          intg_seen_dut, n_checks, n_out_checks, n_err), UVM_LOW)
      if (cfg.en_cov)
        `uvm_info(get_type_name(), $sformatf({"functional coverage (instance, this run): cg_run %0.2f%%: scenario %0.1f%%, ",
            "lc_seed_hw_rd_en %0.1f%%, creator seed %0.1f%%, owner seed %0.1f%%, RMA %0.1f%%, disable %0.1f%%"},
            cg_run.get_inst_coverage(), cg_run.cp_scn.get_inst_coverage(), cg_run.cp_seed_en.get_inst_coverage(),
            cg_run.cp_seed0.get_inst_coverage(), cg_run.cp_seed1.get_inst_coverage(),
            cg_run.cp_rma.get_inst_coverage(), cg_run.cp_dis.get_inst_coverage()), UVM_LOW)
      foreach (n_msg[k]) if (n_msg[k] > 3)
        `uvm_info(get_type_name(), $sformatf("mismatch seen %0d times (3 reported): %s", n_msg[k], k), UVM_LOW)
      if (h != null) eg2_lcmgr_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_lcmgr_cmodel_random_test extends rram_ctrl_lcmgr_base_test;
    `uvm_component_utils(rram_ctrl_lcmgr_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      rram_ctrl_lcmgr_scoreboard::type_id::set_type_override(rram_ctrl_lcmgr_cmodel_scoreboard::get_type());
      test_seq = "rram_ctrl_lcmgr_cmodel_vseq";
      timeout_ns = 50_000_000;
      super.build_phase(phase);
    endfunction
  endclass

  // Must fail: the scoreboard flips one bit of the first predicted seed (a provisioned scenario).
  class rram_ctrl_lcmgr_cmodel_random_inject_fail_test extends rram_ctrl_lcmgr_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_lcmgr_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass

  // A full RMA wipe (about a million bus words), with or without a late fault.
  class rram_ctrl_lcmgr_cmodel_rma_test extends rram_ctrl_lcmgr_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_lcmgr_cmodel_rma_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(null, "", "full_rma", 1'b1);
      super.build_phase(phase);
      timeout_ns = 2_000_000_000;
      uvm_root::get().set_timeout(timeout_ns * 1ns, 1);
    endfunction
  endclass
endpackage
