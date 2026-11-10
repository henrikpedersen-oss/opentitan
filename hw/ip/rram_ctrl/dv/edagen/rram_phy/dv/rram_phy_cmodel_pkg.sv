// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The PHY unit bench's own part (edagen rram_phy.yaml dv.files), on edagen's generated skeleton:
//   - rram_phy_cmodel_vseq: constrained-random stimulus as rram_ctrl drives rram_phy: host reads
//     (held until host_gnt, back to back, with address locality so the read buffer hits), controller
//     reads (one at a time, held until ctrl_rd_done) and controller writes (whole RRAM words, aligned,
//     one bus word at a time until ctrl_wr_done, ctrl_wr_last on the last; 1 to 40 RRAM words, so
//     page ends and the 32-word bound are crossed). Each page used has fixed attributes (scramble_en,
//     ecc_en, addr_xor_en: memory protection gives them per page). The scrambling keys: a stretch on
//     the reset keys, then keys_valid with new keys. It also plays the RRAM macro by its documented
//     interface (rram_ctrl README "RRAM Controller - Macro Interface"): init_done after a delay, one
//     request at a time (ack after 0-2 cycles, done after a random latency, a new request only after
//     done), a store buffer that pushes fill and a commit writes, read data with an ECC status per
//     word (corrected single/double, and uncorrectable in the ECC test).
//   - rram_phy_cmodel_scoreboard: every macro write request against the PHY reference model
//     (eg2_rram_phy_core.h through eg2_rram_phy_dpi.cc), every host and controller read response
//     (data, integrity, error flag) against the model, the macro protocol (no request before
//     init_done, a request held stable until ack), at most two host reads outstanding, one response
//     per request, the shadow read (every RRAM read of a word followed by a second read of the same
//     word, address and configuration), corrected ECC errors reported once per RRAM read with their
//     address, uncorrectable ones as ecc_fatal_err and an error response, the fatal errors of each
//     fault test, and nothing after a disable; the functional coverage.
//   - the tests: random (clean), ecc (uncorrectable words), rdfault (one corrupted macro read:
//     rd_intg_err must follow), wrintg (one write word with a bad integrity: wr_intg_err and the
//     zero-filled word), disable (rram_disable mid-run: no grant, no new access), and
//     random_inject_fail, which flips one bit of one expected read and must fail.
package rram_phy_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import rram_ctrl_pkg::RndCnstAddrKeyDefault;
  import rram_ctrl_pkg::RndCnstDataKeyDefault;
  import rram_phy_ph_agent_pkg::*;
  import rram_phy_env_pkg::*;
  import rram_phy_test_pkg::*;

  import "DPI-C" function chandle eg2_rram_phy_create(input bit [127:0] rst_addr_key,
                                                      input bit [127:0] rst_data_key);
  import "DPI-C" function void eg2_rram_phy_release(chandle h);
  import "DPI-C" function void eg2_rram_phy_step(chandle h, int unsigned keys_disable, int keys_valid,
      input bit [127:0] ak, input bit [127:0] dk, input bit [127:0] rak, input bit [127:0] rdk);
  import "DPI-C" function int unsigned eg2_rram_phy_epoch(chandle h);
  import "DPI-C" function void eg2_rram_phy_preload(chandle h, int part, int unsigned addr,
                                                    input bit [127:0] d);
  import "DPI-C" function void eg2_rram_phy_set_status(chandle h, int part, int unsigned addr,
                                                       int unsigned st);
  import "DPI-C" function void eg2_rram_phy_wr_word(chandle h, int unsigned bus_addr,
      input bit [38:0] d39, int part, int scr, int ecc, int xr, int last);
  import "DPI-C" function int eg2_rram_phy_macro_wr(chandle h, int last, int unsigned addr, int part,
                                                    int ecc, input bit [127:0] d);
  import "DPI-C" function string eg2_rram_phy_error(chandle h);
  import "DPI-C" function int unsigned eg2_rram_phy_pending(chandle h);
  import "DPI-C" function int unsigned eg2_rram_phy_wr_intg_errs(chandle h);
  import "DPI-C" function void eg2_rram_phy_read(chandle h, int unsigned bus_addr, int part, int scr,
      int ecc, int xr, output int err, output bit [38:0] d39);
  import "DPI-C" function int unsigned eg2_rram_phy_intg(int unsigned w);

  localparam bit [3:0] MuBi4True = 4'h6, MuBi4False = 4'h9;
  typedef enum int {ModeRandom, ModeEcc, ModeRdFault, ModeWrIntg, ModeDisable} mode_e;

  // The pages the bench uses, each with fixed attributes (memory protection gives them per page):
  // the data partition at the start, in the middle, the last data pages before the OTP region, an
  // OTP page (addr_xor off, controller only) and the last page; two info pages.
  class page_t;
    bit part;
    int unsigned page;
    bit scr, ecc, xr, host_ok;
  endclass

  // What the stimulus/macro and the scoreboard share: the environment's facts (never the DUT's
  // state).
  class rram_phy_ctx;
    static mode_e mode = ModeRandom;
    static bit ready;                     // pages and preload set up
    static page_t pages[$];
    static bit [127:0] image[int unsigned];     // the bench macro's contents (part << 17 | addr)
    static int unsigned status[int unsigned];   // its ECC status per word: 1, 2 corrected, 4 not
    static int unsigned fault_key = '1;         // the word whose macro read was corrupted
    static longint fault_cycle = -1;
    static longint wrintg_cycle = -1;           // a write word with a bad integrity was issued
    static longint disable_cycle = -1;
    static bit stalled;                         // no access completed for 20000 cycles
    static function int unsigned key(bit part, int unsigned addr);
      return (int'(part) << 17) | (addr & 32'h1ffff);
    endfunction
    static function page_t page_of(bit part, int unsigned page);
      foreach (pages[i]) if (pages[i].part == part && pages[i].page == page) return pages[i];
      return null;
    endfunction
  endclass

  function automatic bit [127:0] rand128();
    return {$urandom(), $urandom(), $urandom(), $urandom()};
  endfunction

  // ---------------------------------------------------------------------------------------------
  class rram_phy_cmodel_vseq extends rram_phy_base_vseq;
    `uvm_object_utils(rram_phy_cmodel_vseq)

    int unsigned num_ops = 1500;
    virtual rram_phy_ph_if vif;
    longint cyc;

    // macro responder state
    bit m_busy, m_ackd, m_is_rd, m_is_last, m_part, m_ecc;
    int unsigned m_addr, m_lat, m_ack_wait;
    bit [127:0] sbuf[32];
    bit [127:0] m_wdata;
    bit sbuf_v[32];
    bit fault_armed, wrintg_armed;
    bit m_ack_now;
    bit latch_pending;
    bit latch_early;               // +macro_latch_early=1
    bit e_is_rd, e_is_last, e_part, e_ecc;
    int unsigned e_addr;
    bit [127:0] e_wdata;
    int unsigned n_vanished;       // acks rram_phy did not take (its request had gone)  // ack driven: latch the request in the cycle the ack is seen  // acknowledged in this call: the same request is still visible in the next

    // host and controller drivers
    bit h_on;
    int unsigned h_addr;
    page_t h_pg;
    int unsigned busy_pct = 60;
    int unsigned ops_done;
    bit c_on, c_wr;
    int unsigned c_idx;
    int unsigned c_addr[$];
    bit [38:0] c_data[$];
    page_t c_pg;
    int unsigned idle_gap;
    int unsigned last_ops;
    longint last_progress;

    function new(string name = "rram_phy_cmodel_vseq");
      super.new(name);
    endfunction

    function void add_page(bit part, int unsigned page, bit scr, bit ecc, bit xr, bit host_ok);
      page_t p = new();
      p.part = part; p.page = page; p.scr = scr; p.ecc = ecc; p.xr = xr; p.host_ok = host_ok;
      rram_phy_ctx::pages.push_back(p);
    endfunction

    function void setup();
      mode_e mode = rram_phy_ctx::mode;
      // Data pages (host reads allowed), OTP page 4090 (addr_xor off, controller only), the last
      // page; info pages (controller only).
      add_page(0, 0, 1, 1, 1, 1);
      add_page(0, 1, 0, 1, 1, 1);
      add_page(0, 2, 1, 0, 1, 1);
      add_page(0, 1500, $urandom_range(0, 1), 1, 1, 1);
      add_page(0, 4089, 1, 1, 1, 1);
      add_page(0, 4090, 0, 1, 0, 0);
      add_page(0, 4095, $urandom_range(0, 1), $urandom_range(0, 1), 1, 1);
      add_page(1, 0, 1, 1, 1, 0);
      add_page(1, 5, 0, 1, 1, 0);
      // The macro's initial contents and ECC status.
      foreach (rram_phy_ctx::pages[i]) begin
        page_t p = rram_phy_ctx::pages[i];
        for (int w = 0; w < 32; w++) begin
          int unsigned k = rram_phy_ctx::key(p.part, p.page * 32 + w);
          rram_phy_ctx::image[k] = rand128();
          case ($urandom_range(0, 99))
            0, 1, 2: rram_phy_ctx::status[k] = 1;
            3: rram_phy_ctx::status[k] = 2;
            4, 5, 6: if (mode == ModeEcc) rram_phy_ctx::status[k] = 4;
            default: ;
          endcase
        end
      end
      fault_armed = mode == ModeRdFault;
      wrintg_armed = mode == ModeWrIntg;
      rram_phy_ctx::ready = 1;
    endfunction

    bit plain_only;      // before the key change: unscrambled pages only (+keys_mid_traffic=1: all)
    bit keys_mid_traffic;
    function page_t pick_page(bit for_host);
      page_t p;
      do p = rram_phy_ctx::pages[$urandom_range(0, rram_phy_ctx::pages.size() - 1)];
      while ((for_host && !p.host_ok) || (plain_only && p.scr));
      return p;
    endfunction

    // A bus-word address in page p, near the last one now and then (read-buffer hits).
    function int unsigned pick_addr(page_t p, int unsigned last, bit near);
      if (near && (last >> 7) == p.page) return (last & ~32'h3) | $urandom_range(0, 3);
      return p.page * 128 + $urandom_range(0, 127);
    endfunction

    // ---- the RRAM macro ----
    task macro_cycle();
      bit just_done = 0;
      vif.m_ack <= 1'b0;
      vif.m_done <= 1'b0;
      vif.m_err <= 1'b0;
      vif.m_ecc_err <= 3'b000;
      if (m_busy && m_ackd && latch_pending && !(vif.m_rd_req === 1'b1 || vif.m_wr_req === 1'b1)) begin
        // rram_phy dropped its request before seeing the ack: nothing was accepted.
        latch_pending = 0;
        m_busy = 0;
        m_ackd = 0;
        n_vanished++;
      end else if (m_busy && m_ackd && latch_pending) begin
        // The cycle the ack is seen: the request as rram_phy presents it now is the accepted one
        // (+macro_latch_early=1: the request as it was first seen, the README's "held stable until
        // ack" contract taken at its word).
        latch_pending = 0;
        if (latch_early) begin
          m_is_rd = e_is_rd; m_is_last = e_is_last; m_addr = e_addr; m_part = e_part; m_ecc = e_ecc;
          m_wdata = e_wdata;
        end else begin
        m_is_rd = vif.m_rd_req;
        m_is_last = vif.m_wr_last;
        m_addr = vif.m_addr;
        m_part = vif.m_part;
        m_ecc = vif.m_ecc_en;
        m_wdata = vif.m_wr_data;
        end
        m_lat = m_is_rd ? $urandom_range(0, 5) : (m_is_last ? $urandom_range(4, 30) : $urandom_range(0, 1));
      end else if (m_busy && m_ackd) begin
        if (m_lat == 0) begin
          m_busy = 0;
          m_ackd = 0;
          just_done = 1;
          vif.m_done <= 1'b1;
          if (m_is_rd) begin
            int unsigned k = rram_phy_ctx::key(m_part, m_addr);
            bit [127:0] d = rram_phy_ctx::image.exists(k) ? rram_phy_ctx::image[k] : '0;
            int unsigned st = (m_ecc && rram_phy_ctx::status.exists(k)) ? rram_phy_ctx::status[k] : 0;
            if (st == 4) d = ~d ^ {4{32'h5a5a_c3c3}};  // uncorrectable: lost, the same garbage each read
            vif.m_ecc_err <= st == 4 ? 3'b100 : (st == 2 ? 3'b010 : (st == 1 ? 3'b001 : 3'b000));
            if (fault_armed && ops_done > num_ops / 3 && st == 0) begin
              // A fault on the read path: one data bit flipped, no ECC status (the shadow read must
              // catch it, rd_intg_err).
              begin int unsigned fb = $urandom_range(0, 127); d[fb] = !d[fb]; end
              fault_armed = 0;
              rram_phy_ctx::fault_key = k;
              rram_phy_ctx::fault_cycle = cyc;
              `uvm_info(get_type_name(), $sformatf("rdfault: macro read of %05h (part %0d, ecc_en %0d) corrupted at %0t",
                                                   m_addr, m_part, m_ecc, $time), UVM_LOW)
            end
            vif.m_rd_data <= d;
          end else if (m_is_last) begin
            bit any = 0;
            for (int w = 0; w < 32; w++) if (sbuf_v[w]) begin
              int unsigned k = rram_phy_ctx::key(m_part, ((m_addr >> 5) << 5) | w);
              rram_phy_ctx::image[k] = sbuf[w];
              rram_phy_ctx::status.delete(k);
              sbuf_v[w] = 0;
              any = 1;
            end
            if (!any) vif.m_err <= 1'b1;  // a commit with an empty store buffer is rejected
          end else begin
            sbuf[m_addr & 31] = m_wdata;
            sbuf_v[m_addr & 31] = 1;
          end
        end else begin
          m_lat--;
        end
      end
      // A new request: not in the cycle that ends the last one (the request seen now may still be
      // the one just acknowledged; rram_phy drops it only after it has seen the ack).
      if (!m_busy && !just_done && !m_ack_now && (vif.m_rd_req === 1'b1 || vif.m_wr_req === 1'b1)) begin
        m_busy = 1;
        e_is_rd = vif.m_rd_req; e_is_last = vif.m_wr_last; e_addr = vif.m_addr; e_part = vif.m_part;
        e_ecc = vif.m_ecc_en; e_wdata = vif.m_wr_data;
        m_ack_wait = $urandom_range(0, 2);
      end
      m_ack_now = 0;
      if (m_busy && !m_ackd) begin
        if (m_ack_wait == 0) begin
          m_ack_now = 1;
          // ack this cycle (the request is held until it): latch the request as accepted.
          vif.m_ack <= 1'b1;
          m_ackd = 1;
          latch_pending = 1;
        end else begin
          m_ack_wait--;
        end
      end
    endtask

    // ---- host reads ----
    task host_cycle(bit issue);
      if (h_on && vif.host_gnt === 1'b1) begin
        h_on = 0;
        ops_done++;
      end
      if (!h_on && issue && $urandom_range(0, 99) < busy_pct) begin
        bit near = $urandom_range(0, 2) != 0;
        if (h_pg == null || $urandom_range(0, 9) == 0 || (plain_only && h_pg.scr)) h_pg = pick_page(1);
        h_addr = pick_addr(h_pg, h_addr, near);
        h_on = 1;
      end
      vif.host_req <= h_on;
      vif.host_addr <= h_addr;
      vif.host_scramble_en <= h_pg == null ? 1'b0 : h_pg.scr;
      vif.host_ecc_en <= h_pg == null ? 1'b1 : h_pg.ecc;
    endtask

    // ---- controller reads and writes ----
    task ctrl_cycle(bit issue);
      if (c_on) begin
        if (!c_wr && vif.ctrl_rd_done === 1'b1) begin
          c_on = 0;
          ops_done++;
          idle_gap = $urandom_range(0, 6);
        end else if (c_wr && vif.ctrl_wr_done === 1'b1) begin
          c_idx++;
          if (c_idx == c_addr.size()) begin
            c_on = 0;
            ops_done++;
            idle_gap = $urandom_range(0, 10);
          end
        end
      end
      if (!c_on && idle_gap > 0) idle_gap--;
      else if (!c_on && issue && $urandom_range(0, 99) < busy_pct / 3) begin
        c_pg = pick_page(0);
        c_wr = $urandom_range(0, 3) == 0 && rram_phy_ctx::fault_cycle < 0;
        c_addr.delete();
        c_data.delete();
        c_idx = 0;
        if (c_wr) begin
          // 1-40 RRAM words from an aligned start in the page (later pages follow if they exist).
          int unsigned n = $urandom_range(0, 4) == 0 ? $urandom_range(30, 40) : $urandom_range(1, 6);
          int unsigned start = c_pg.page * 128 + 4 * $urandom_range(0, 31);
          for (int unsigned i = 0; i < 4 * n; i++) begin
            int unsigned a = start + i;
            bit [31:0] w = $urandom();
            bit [6:0] ig = 7'(eg2_rram_phy_intg(w));
            if (rram_phy_ctx::page_of(c_pg.part, a >> 7) == null) break;
            if (wrintg_armed && ops_done > num_ops / 3 && i == 2) begin
              begin int unsigned fb = $urandom_range(0, 6); ig[fb] = !ig[fb]; end  // a bus-integrity error on one write word
              wrintg_armed = 0;
              rram_phy_ctx::wrintg_cycle = cyc;
            end
            c_addr.push_back(a);
            c_data.push_back({ig, w});
          end
          // Whole RRAM words only.
          while (c_addr.size() % 4 != 0) begin
            void'(c_addr.pop_back());
            void'(c_data.pop_back());
          end
          if (c_addr.size() == 0) c_wr = 0;
        end
        if (!c_wr) c_addr.push_back(pick_addr(c_pg, h_addr, $urandom_range(0, 1)));
        c_on = 1;
      end
      vif.ctrl_req <= c_on;
      vif.ctrl_rd <= c_on && !c_wr;
      vif.ctrl_wr <= c_on && c_wr;
      if (c_on) begin
        page_t p = rram_phy_ctx::page_of(c_pg.part, c_addr[c_idx] >> 7);
        vif.ctrl_addr <= c_addr[c_idx];
        vif.ctrl_part <= c_pg.part;
        vif.ctrl_scramble_en <= p.scr;
        vif.ctrl_ecc_en <= p.ecc;
        vif.ctrl_addr_xor_en <= p.xr;
        vif.ctrl_wr_data <= c_wr ? c_data[c_idx] : '0;
        vif.ctrl_wr_last <= c_wr && (c_idx == c_addr.size() - 1);
      end else begin
        vif.ctrl_wr_last <= 1'b0;
      end
    endtask

    task step(bit issue);
      @(posedge vif.clk);
      cyc++;
      macro_cycle();
      host_cycle(issue);
      ctrl_cycle(issue);
      if ($urandom_range(0, 399) == 0) busy_pct = $urandom_range(10, 100);
    endtask

    virtual task body();
      bit [127:0] ak = rand128(), dk = rand128();
      int unsigned init_delay = $urandom_range(5, 60);
      vif = cfg.ph_cfg.vif;
      void'($value$plusargs("num_ops=%d", num_ops));
      void'($value$plusargs("macro_latch_early=%d", latch_early));
      void'($value$plusargs("keys_mid_traffic=%d", keys_mid_traffic));
      setup();
      @(posedge vif.clk);
      vif.rram_disable <= MuBi4False;
      vif.keys_valid <= 1'b0;
      vif.addr_key <= rand128();
      vif.data_key <= rand128();
      vif.rand_addr_key <= rand128();
      vif.rand_data_key <= rand128();
      vif.m_init_done <= 1'b0;
      vif.m_fatal_err <= 1'b0;
      vif.m_recov_err <= 1'b0;
      super.body();
      // The macro's initialization, then a stretch on the reset keys, then keys_valid.
      repeat (init_delay) @(posedge vif.clk);
      vif.m_init_done <= 1'b1;
      // Traffic on the reset keys: unscrambled pages only, then drained and the shadow reads done
      // before the keys change (whether a key change invalidates the read buffer is not documented:
      // finding; +keys_mid_traffic=1 keeps all pages and changes the keys with traffic in flight).
      plain_only = !keys_mid_traffic;
      repeat ($urandom_range(100, 400)) step(1);
      if (!keys_mid_traffic) begin
        for (int i = 0; i < 5000 && (h_on || c_on || m_busy); i++) step(0);
        repeat (300) step(0);
      end
      plain_only = 0;
      @(posedge vif.clk);
      vif.addr_key <= ak;
      vif.data_key <= dk;
      vif.keys_valid <= 1'b1;
      step(0);
      vif.keys_valid <= 1'b0;
      // Let any operation started on the reset keys finish before the new keys count.
      repeat (200) step(0);
      last_ops = ops_done;
      last_progress = cyc;
      while (ops_done < num_ops) begin
        if (ops_done != last_ops) begin
          last_ops = ops_done;
          last_progress = cyc;
        end else if (cyc > last_progress + 20000) begin
          // rram_phy stopped answering: end the run (the scoreboard reports what is unanswered).
          rram_phy_ctx::stalled = 1;
          `uvm_info(get_type_name(), $sformatf("no access completed for 20000 cycles (at %0t): stopping", $time), UVM_LOW)
          break;
        end
        if (rram_phy_ctx::mode == ModeDisable && rram_phy_ctx::disable_cycle < 0 &&
            ops_done > num_ops / 2) begin
          vif.rram_disable <= $urandom_range(0, 1) ? MuBi4True : 4'h0;
          rram_phy_ctx::disable_cycle = cyc;
          repeat (2000) step(1);
          break;
        end
        step(1);
        if (rram_phy_ctx::fault_cycle >= 0 && cyc > rram_phy_ctx::fault_cycle + 500) break;
      end
      // Drain: no new requests until everything has completed.
      for (int i = 0; i < 5000 && (h_on || c_on || m_busy) && !rram_phy_ctx::stalled; i++) step(0);
      repeat (500) step(0);  // shadow reads and the last commit
    endtask
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_phy_cmodel_scoreboard extends rram_phy_scoreboard;
    `uvm_component_utils(rram_phy_cmodel_scoreboard)

    typedef struct {bit [38:0] d; bit err; int unsigned addr; int unsigned key; longint t;} exp_t;
    typedef struct {int unsigned key; bit part; int unsigned addr; longint t;} rep_t;

    chandle h;
    bit inject_fail, injected, preloaded;
    longint cyc;
    exp_t host_exp[$];
    int unsigned n_host, n_ctrl_rd, n_ctrl_wr, n_push, n_commit, n_fill, n_verify, n_dropped_verify;
    int unsigned n_corr1, n_corr2, n_fatal_exp, n_fatal, n_checks, n_err, n_unpred, n_host_err;
    int unsigned max_out;
    int unsigned txn_words;
    // macro protocol
    bit prev_req, prev_rd, prev_wr, prev_last, prev_part, prev_ecc, prev_ack;
    int unsigned prev_addr;
    bit [127:0] prev_data;
    bit init_seen, init_seen_prev;
    // the macro read in flight (accepted with ack, done to come)
    bit rd_open;
    int unsigned rd_key;
    bit rd_part, rd_ecc;
    int unsigned rd_addr;
    // shadow reads: words read once and not yet a second time (key with ecc_en in bit 31)
    int unsigned unverified[int unsigned];
    int unsigned dropped[int unsigned];
    rep_t corr_exp[2][$];
    longint fatal_due[$];
    // fault tests
    bit unmask_unstable;
    bit trace, p_intg, p_ctrl;  // +rram_phy_trace=1
    // Known finding (status/rtl_tickets.md "rram_phy: an uncorrectable read frees its read-buffer
    // entry while a queued read still depends on it"): after the first uncorrectable RRAM read,
    // rd_ctrl_err, rd_intg_err and wrong responses are counted, not errors, unless unmask_ecc
    // (rram_phy_cmodel_ecc_unmasked_test, which must fail).
    bit unmask_ecc;
    longint unc_cycle = -1;
    int unsigned n_after_unc;
    int unsigned n_unstable_drop, n_unstable_change, n_intg_after_dis, n_rsp_after_dis;
    longint rd_intg_seen = -1, wr_intg_seen = -1, fatal_seen = -1, dis_cycle = -1, c_start = -1;

    // coverage
    int c_who, c_part, c_scr, c_ecc, c_xr, c_err, c_out, c_st, c_kind, c_wlen, c_lat;
    covergroup cg_rsp;
      cp_who: coverpoint c_who { bins host = {0}; bins ctrl = {1}; }
      cp_part: coverpoint c_part { bins data = {0}; bins info = {1}; }
      cp_scr: coverpoint c_scr { bins plain = {0}; bins scrambled = {1}; }
      cp_ecc: coverpoint c_ecc { bins off = {0}; bins on = {1}; }
      cp_xr: coverpoint c_xr { bins off = {0}; bins on = {1}; }
      cp_err: coverpoint c_err { bins good = {0}; bins error = {1}; }
      cp_out: coverpoint c_out iff (c_who == 0) { bins one = {1}; bins two = {2}; }
      cp_lat: coverpoint c_lat { bins hit_fast = {[1:2]}; bins l3_6 = {[3:6]}; bins l7_15 = {[7:15]};
                                 bins longer = {[16:$]}; }
      x_who_scr_ecc: cross cp_who, cp_scr, cp_ecc;
      x_who_part: cross cp_who, cp_part;
    endgroup
    covergroup cg_macro;
      cp_kind: coverpoint c_kind { bins fill = {0}; bins verify = {1}; bins push = {2}; bins commit = {3}; }
      cp_st: coverpoint c_st iff (c_kind < 2) { bins none = {0}; bins corr1 = {1}; bins corr2 = {2};
                                                bins uncorr = {4}; }
      x_kind_st: cross cp_kind, cp_st {
        ignore_bins writes = binsof(cp_kind.push) || binsof(cp_kind.commit);  // no ECC status
      }
    endgroup
    covergroup cg_wr;
      cp_wlen: coverpoint c_wlen { bins one = {1}; bins few = {[2:6]}; bins page_cross = {[7:31]};
                                   bins full_page = {32}; bins more = {[33:$]}; }
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_rsp = new();
      cg_macro = new();
      cg_wr = new();
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      void'($value$plusargs("rram_phy_unmask_unstable=%d", unmask_unstable));
      void'($value$plusargs("rram_phy_trace=%d", trace));
      void'(uvm_config_db#(bit)::get(this, "", "unmask_ecc", unmask_ecc));
      h = eg2_rram_phy_create(RndCnstAddrKeyDefault, RndCnstDataKeyDefault);
    endfunction

    function bit after_unc();
      return unc_cycle >= 0 && !unmask_ecc;
    endfunction

    function void expect_ok(bit ok, string what);
      n_checks++;
      if (!ok) begin
        n_err++;
        `uvm_error(get_type_name(), what)
      end
    endfunction

    function void do_preload();
      foreach (rram_phy_ctx::image[k]) eg2_rram_phy_preload(h, k >> 17, k & 32'h1ffff, rram_phy_ctx::image[k]);
      foreach (rram_phy_ctx::status[k]) eg2_rram_phy_set_status(h, k >> 17, k & 32'h1ffff, rram_phy_ctx::status[k]);
      preloaded = 1;
    endfunction

    // The page attributes the bench uses (the model's read takes them like rram_phy gets them).
    function page_t attrs(bit part, int unsigned bus_addr);
      return rram_phy_ctx::page_of(part, bus_addr >> 7);
    endfunction

    function void check_read(string who, int unsigned bus_addr, bit part, bit [38:0] got, bit got_err,
                             exp_t e);
      if (dis_cycle >= 0) begin
        n_rsp_after_dis++;  // in flight at the disable: descrambled with the random keys
        return;
      end
      if (after_unc() && (got_err !== e.err || (!e.err && got !== e.d))) begin
        n_after_unc++;
        return;
      end
      if (rram_phy_ctx::fault_cycle >= 0 && e.key == rram_phy_ctx::fault_key) begin
        n_unpred++;  // the corrupted word: its data is not predicted (rd_intg_err is checked)
        return;
      end
      expect_ok(got_err === e.err, $sformatf("%s read of %05h (part %0d): error flag %b, the model's %b",
                                             who, bus_addr, part, got_err, e.err));
      if (!e.err) begin
        bit [38:0] exp = e.d;
        if (inject_fail && !injected) begin
          exp[0] = !exp[0];
          injected = 1;
          `uvm_info(get_type_name(), "inject_fail: one bit of an expected read flipped", UVM_LOW)
        end
        expect_ok(got === exp, $sformatf("%s read of %05h (part %0d): %010h, the model's %010h",
                                         who, bus_addr, part, got, exp));
      end
    endfunction

    virtual function void predict_ph(rram_phy_ph_item item);
      bit mode_ok_rd_intg = rram_phy_ctx::mode == ModeRdFault;
      bit mode_ok_wr_intg = rram_phy_ctx::mode == ModeWrIntg;
      bit mode_ok_fatal = rram_phy_ctx::mode == ModeEcc;
      bit dis;
      cyc++;
      if (item.rram_disable !== MuBi4False && dis_cycle < 0 && rram_phy_ctx::ready) dis_cycle = cyc;
      dis = dis_cycle >= 0 && cyc > dis_cycle + 6;
      if (!rram_phy_ctx::ready) begin
        eg2_rram_phy_step(h, item.rram_disable, item.keys_valid === 1'b1, item.addr_key, item.data_key,
                          item.rand_addr_key, item.rand_data_key);
        return;
      end
      if (!preloaded) do_preload();

      // ---- fatal and consistency errors ----
      if (item.rd_ctrl_err === 1'b1 && after_unc()) n_after_unc++;
      expect_ok(item.fsm_err !== 1'b1 && item.cnt_err !== 1'b1 && (item.rd_ctrl_err !== 1'b1 || after_unc()) &&
                item.arb_err !== 1'b1 && item.fifo_err !== 1'b1 && item.spurious_done !== 1'b1 &&
                item.host_gnt_err !== 1'b1,
                $sformatf("fatal/consistency error: fsm %b cnt %b rd_ctrl %b arb %b fifo %b spurious %b host_gnt %b",
                          item.fsm_err, item.cnt_err, item.rd_ctrl_err, item.arb_err, item.fifo_err,
                          item.spurious_done, item.host_gnt_err));
      if (item.rd_intg_err === 1'b1 && dis_cycle >= 0) begin
        // Known finding: after rram_disable the scrambler uses the random keys, and a shadow read
        // still pending compares data descrambled with another key (counted, not an error).
        n_intg_after_dis++;
      end else if (item.rd_intg_err === 1'b1 && after_unc()) begin
        n_after_unc++;
      end else if (item.rd_intg_err === 1'b1) begin
        expect_ok(mode_ok_rd_intg && rram_phy_ctx::fault_cycle >= 0, "rd_intg_err without a corrupted macro read");
        if (rd_intg_seen < 0) rd_intg_seen = cyc;
      end
      if (item.wr_intg_err === 1'b1) begin
        expect_ok(mode_ok_wr_intg && rram_phy_ctx::wrintg_cycle >= 0, "wr_intg_err without a bad write word");
        if (wr_intg_seen < 0) wr_intg_seen = cyc;
      end

      if (trace) begin
        if (item.rd_intg_err === 1'b1 && !p_intg) `uvm_info("TRACE", "rd_intg_err rises", UVM_LOW)
        if (item.rd_ctrl_err === 1'b1 && !p_ctrl) `uvm_info("TRACE", "rd_ctrl_err rises", UVM_LOW)
        if (item.ecc_fatal_err === 1'b1) `uvm_info("TRACE", "ecc_fatal_err", UVM_LOW)
        if (item.host_gnt === 1'b1) `uvm_info("TRACE", $sformatf("host grant %05h", item.host_addr), UVM_LOW)
        if (item.host_rd_done === 1'b1) `uvm_info("TRACE", $sformatf("host response %010h err %b", item.host_rd_data, item.host_rd_err), UVM_LOW)
        if (item.ctrl_rd_done === 1'b1) `uvm_info("TRACE", $sformatf("controller response %05h part %0d: %010h err %b", item.ctrl_addr, item.ctrl_part, item.ctrl_rd_data, item.ctrl_rd_err), UVM_LOW)
        if (item.m_ack === 1'b1 && item.m_wr_req === 1'b1 && item.m_wr_last === 1'b1) `uvm_info("TRACE", $sformatf("commit %05h part %0d", item.m_addr, item.m_part), UVM_LOW)
      end
      p_intg = item.rd_intg_err === 1'b1;
      p_ctrl = item.rd_ctrl_err === 1'b1;

      // ---- the macro protocol ----
      if (item.m_init_done === 1'b1) init_seen = 1;
      expect_ok(!(item.m_rd_req === 1'b1 || item.m_wr_req === 1'b1) || init_seen_prev,
                "a macro request before init_done");
      init_seen_prev = init_seen;
      expect_ok(!(item.m_rd_req === 1'b1 && item.m_wr_req === 1'b1), "macro rd_req and wr_req at once");
      if (prev_req && !prev_ack && !(item.m_rd_req === prev_rd && item.m_wr_req === prev_wr &&
          item.m_addr === prev_addr && item.m_part === prev_part && item.m_ecc_en === prev_ecc &&
          (!prev_wr || (item.m_wr_last === prev_last && (prev_last || item.m_wr_data === prev_data)))) &&
          !unmask_unstable) begin
        // Known finding (status/rtl_tickets.md "rram_phy: the macro read request is not held until
        // ack"): counted, not an error, unless +rram_phy_unmask_unstable=1.
        if (prev_rd && !(item.m_rd_req === 1'b1)) n_unstable_drop++; else n_unstable_change++;
      end else if (prev_req && !prev_ack) begin
        expect_ok(item.m_rd_req === prev_rd && item.m_wr_req === prev_wr && item.m_addr === prev_addr &&
                  item.m_part === prev_part && item.m_ecc_en === prev_ecc &&
                  (!prev_wr || (item.m_wr_last === prev_last && (prev_last || item.m_wr_data === prev_data))),
                  $sformatf({"a macro request changed or dropped before ack: was rd %b wr %b last %b addr %05h part %b ",
                             "ecc %b, now rd %b wr %b last %b addr %05h part %b ecc %b%0s"}, prev_rd, prev_wr, prev_last,
                            prev_addr, prev_part, prev_ecc, item.m_rd_req, item.m_wr_req, item.m_wr_last, item.m_addr,
                            item.m_part, item.m_ecc_en,
                            (prev_wr && !prev_last && item.m_wr_data !== prev_data) ? " (push data changed)" : ""));
      end
      // After rram_disable no new access starts: a write already under way may finish (rram_phy_wr
      // "Only disable during idle state"), a shadow read of a word already read may still be made
      // (it is not a host or controller request); anything else is an error.
      if (dis && !prev_req && (item.m_rd_req === 1'b1 || item.m_wr_req === 1'b1)) begin
        int unsigned kk = rram_phy_ctx::key(item.m_part, item.m_addr) | (int'(item.m_ecc_en) << 31);
        expect_ok((item.m_wr_req === 1'b1 && item.wr_busy === 1'b1) ||
                  (item.m_rd_req === 1'b1 && (unverified.exists(kk) || dropped.exists(kk))),
                  $sformatf("a new macro %s of %05h after rram_disable", item.m_rd_req === 1'b1 ? "read" : "write",
                            item.m_addr));
      end
      prev_req = item.m_rd_req === 1'b1 || item.m_wr_req === 1'b1;
      prev_rd = item.m_rd_req; prev_wr = item.m_wr_req; prev_last = item.m_wr_last;
      prev_addr = item.m_addr; prev_part = item.m_part; prev_ecc = item.m_ecc_en;
      prev_data = item.m_wr_data; prev_ack = item.m_ack;
      expect_ok(item.m_err !== 1'b1, "the macro rejected a request (err: commit with an empty store buffer)");

      // ---- controller write words (before the macro write: the word that completes an RRAM word
      //      is accepted in the cycle its push is) ----
      if (item.ctrl_req === 1'b1 && item.ctrl_wr === 1'b1 && item.ctrl_wr_done === 1'b1) begin
        expect_ok(!dis, "a controller write word accepted after rram_disable");
        eg2_rram_phy_wr_word(h, item.ctrl_addr, item.ctrl_wr_data, item.ctrl_part, item.ctrl_scramble_en,
                             item.ctrl_ecc_en, item.ctrl_addr_xor_en, item.ctrl_wr_last);
        n_ctrl_wr++;
        txn_words++;
        if (item.ctrl_wr_last === 1'b1) begin
          c_wlen = txn_words / 4;  // RRAM words in the transaction
          if (cfg.en_cov) cg_wr.sample();
          txn_words = 0;
        end
      end
      expect_ok(!(item.ctrl_wr_done === 1'b1 && !(item.ctrl_req === 1'b1 && item.ctrl_wr === 1'b1)),
                "ctrl_wr_done without a controller write");

      // ---- macro requests accepted (ack) ----
      if (item.m_ack === 1'b1 && item.m_wr_req === 1'b1) begin
        if (!eg2_rram_phy_macro_wr(h, item.m_wr_last, item.m_addr, item.m_part, item.m_ecc_en,
                                   item.m_wr_data))
          expect_ok(0, eg2_rram_phy_error(h));
        else n_checks++;
        if (item.m_wr_last === 1'b1) begin
          n_commit++;
          // The written page's words leave the shadow-read tracking (their buffer entries are
          // invalidated: "Any read-buffer entry matching the written page is invalidated").
          begin
            int unsigned gone[$];
            foreach (unverified[k])
              if (((k & 32'h7fff_ffff) >> 5) == (rram_phy_ctx::key(item.m_part, item.m_addr) >> 5))
                gone.push_back(k);
            foreach (gone[i]) begin
              dropped[gone[i]] = 1;
              unverified.delete(gone[i]);
            end
          end
          c_kind = 3; if (cfg.en_cov) cg_macro.sample();
        end else begin
          n_push++;
          c_kind = 2; if (cfg.en_cov) cg_macro.sample();
        end
      end
      if (item.m_ack === 1'b1 && item.m_rd_req === 1'b1) begin
        rd_open = 1;
        rd_addr = item.m_addr;
        rd_part = item.m_part;
        rd_ecc = item.m_ecc_en;
        rd_key = rram_phy_ctx::key(item.m_part, item.m_addr);
      end
      if (item.m_done === 1'b1 && rd_open) begin
        int unsigned k = rd_key | (int'(rd_ecc) << 31);
        int unsigned st = rd_ecc && rram_phy_ctx::status.exists(rd_key) ? rram_phy_ctx::status[rd_key] : 0;
        rd_open = 0;
        if (trace) `uvm_info("TRACE", $sformatf("macro read done %05h part %0d ecc_en %0d ecc_err %03b data %032h: %0s",
            rd_addr, rd_part, rd_ecc, item.m_ecc_err, item.m_rd_data,
            unverified.exists(k) ? "shadow read" : (dropped.exists(k) ? "shadow read (entry invalidated)" : "fill")), UVM_LOW)
        if (unverified.exists(k)) begin
          unverified.delete(k);  // the shadow read of a word read before
          n_verify++;
          c_kind = 1;
        end else if (dropped.exists(k)) begin
          dropped.delete(k);
          n_dropped_verify++;
          c_kind = 1;
        end else begin
          unverified[k] = 1;
          n_fill++;
          c_kind = 0;
          // A corrected error is reported once per RRAM read of the word, not again by its shadow
          // read; an uncorrectable one raises ecc_fatal_err.
          if (st == 1 || st == 2) corr_exp[st - 1].push_back('{rd_key, rd_part, rd_addr, cyc});
        end
        // An uncorrectable read raises ecc_fatal_err (whether also for the shadow read is not
        // documented: every read with the status is expected to, the fill's and the shadow read's).
        if (st == 4) begin
          fatal_due.push_back(cyc);
          n_fatal_exp++;
          if (unc_cycle < 0) unc_cycle = cyc;
        end
        c_st = st;
        if (cfg.en_cov) cg_macro.sample();
      end
      // Uncorrectable-error reports (ecc_fatal_err comes in the cycle of the macro's done).
      if (item.ecc_fatal_err === 1'b1) begin
        n_fatal++;
        expect_ok(mode_ok_fatal && fatal_due.size() > 0, "ecc_fatal_err without an uncorrectable RRAM read");
        if (fatal_due.size() > 0) void'(fatal_due.pop_front());
        if (fatal_seen < 0) fatal_seen = cyc;
      end
      while (fatal_due.size() > 0 && fatal_due[0] + 100 < cyc) begin
        expect_ok(0, "an uncorrectable RRAM read without ecc_fatal_err within 100 cycles");
        void'(fatal_due.pop_front());
      end
      // Corrected-error reports.
      for (int t = 0; t < 2; t++) begin
        bit pulse = t == 0 ? item.ecc_corr1_err === 1'b1 : item.ecc_corr2_err === 1'b1;
        if (pulse) begin
          int idx = -1;
          foreach (corr_exp[t][i]) if (corr_exp[t][i].addr == item.ecc_corr_addr && corr_exp[t][i].part == item.ecc_corr_part) begin
            idx = i;
            break;
          end
          expect_ok(idx >= 0, $sformatf("ecc_corr%0d_err for %05h part %0d: no corrected RRAM read of it pending",
                                        t + 1, item.ecc_corr_addr, item.ecc_corr_part));
          if (idx >= 0) corr_exp[t].delete(idx);
          if (t == 0) n_corr1++; else n_corr2++;
        end
        while (corr_exp[t].size() > 0 && corr_exp[t][0].t + 100 < cyc) begin
          expect_ok(0, $sformatf("a corrected (%0d-bit) RRAM read of %05h not reported within 100 cycles",
                                 t + 1, corr_exp[t][0].addr));
          void'(corr_exp[t].pop_front());
        end
      end

      // ---- host reads ----
      if (item.host_gnt === 1'b1) begin
        exp_t e;
        page_t p = attrs(0, item.host_addr);
        int err;
        expect_ok(item.host_req === 1'b1, "host_gnt without host_req");
        expect_ok(!dis, "host_gnt after rram_disable");
        expect_ok(p != null, "host read outside the bench's pages");
        if (p != null) begin
          eg2_rram_phy_read(h, item.host_addr, 0, item.host_scramble_en, item.host_ecc_en, 1, err, e.d);
          e.err = err; e.addr = item.host_addr; e.key = rram_phy_ctx::key(0, item.host_addr >> 2);
          e.t = cyc;
          host_exp.push_back(e);
          expect_ok(host_exp.size() <= 2, $sformatf("%0d host reads outstanding (at most 2)", host_exp.size()));
          if (host_exp.size() > max_out) max_out = host_exp.size();
          c_out = host_exp.size();
        end
      end
      if (item.host_rd_done === 1'b1) begin
        expect_ok(host_exp.size() > 0, "host_rd_done without an outstanding host read");
        if (host_exp.size() > 0) begin
          exp_t e = host_exp.pop_front();
          page_t p = attrs(0, e.addr);
          check_read("host", e.addr, 0, item.host_rd_data, item.host_rd_err, e);
          n_host++;
          if (item.host_rd_err === 1'b1) n_host_err++;
          c_who = 0; c_part = 0; c_scr = p.scr; c_ecc = p.ecc; c_xr = 1; c_err = item.host_rd_err;
          c_lat = int'(cyc - e.t);
          if (cfg.en_cov) cg_rsp.sample();
        end
      end

      // ---- controller reads ----
      if (item.ctrl_req === 1'b1 && item.ctrl_rd === 1'b1 && c_start < 0) c_start = cyc;
      if (item.ctrl_rd_done === 1'b1) begin
        expect_ok(item.ctrl_req === 1'b1 && item.ctrl_rd === 1'b1, "ctrl_rd_done without a controller read");
        if (item.ctrl_req === 1'b1 && item.ctrl_rd === 1'b1) begin
          exp_t e;
          int err;
          expect_ok(!dis, "ctrl_rd_done after rram_disable");
          eg2_rram_phy_read(h, item.ctrl_addr, item.ctrl_part, item.ctrl_scramble_en, item.ctrl_ecc_en,
                            item.ctrl_addr_xor_en, err, e.d);
          e.err = err; e.addr = item.ctrl_addr; e.key = rram_phy_ctx::key(item.ctrl_part, item.ctrl_addr >> 2);
          check_read("controller", item.ctrl_addr, item.ctrl_part, item.ctrl_rd_data, item.ctrl_rd_err, e);
          n_ctrl_rd++;
          c_who = 1; c_part = item.ctrl_part; c_scr = item.ctrl_scramble_en; c_ecc = item.ctrl_ecc_en;
          c_xr = item.ctrl_addr_xor_en; c_err = item.ctrl_rd_err; c_lat = int'(cyc - c_start) + 1;
          c_start = -1;
          if (cfg.en_cov) cg_rsp.sample();
        end
      end

      // The key registers take this cycle's inputs at the clock edge that ends it.
      eg2_rram_phy_step(h, item.rram_disable, item.keys_valid === 1'b1, item.addr_key, item.data_key,
                        item.rand_addr_key, item.rand_data_key);
    endfunction

    virtual function void check_phase(uvm_phase phase);
      mode_e mode = rram_phy_ctx::mode;
      super.check_phase(phase);
      if (rram_phy_ctx::stalled) begin
        if (after_unc()) n_after_unc++;
        else expect_ok(0, $sformatf("rram_phy stopped answering: %0d host reads outstanding, controller access pending",
                                    host_exp.size()));
        `uvm_info(get_type_name(), $sformatf("rram_phy stopped answering (%0d host reads outstanding)%0s", host_exp.size(),
                                             after_unc() ? ": after an uncorrectable read (known finding)" : ""), UVM_LOW)
      end else if (mode != ModeDisable) begin
        expect_ok(host_exp.size() == 0, $sformatf("%0d host reads never answered", host_exp.size()));
        expect_ok(eg2_rram_phy_pending(h) == 0, $sformatf("%0d expected macro write requests never made",
                                                           eg2_rram_phy_pending(h)));
      end
      if (rram_phy_ctx::stalled && after_unc()) n_after_unc += unverified.size();
      else if (mode != ModeDisable && mode != ModeRdFault)
        expect_ok(unverified.size() == 0, $sformatf("%0d RRAM reads without their shadow read", unverified.size()));
      expect_ok(corr_exp[0].size() == 0 && corr_exp[1].size() == 0, "corrected ECC errors never reported");
      if (mode == ModeRdFault)
        expect_ok(rram_phy_ctx::fault_cycle >= 0 && rd_intg_seen >= 0,
                  $sformatf("corrupted macro read at %0d: rd_intg_err %0s", rram_phy_ctx::fault_cycle,
                            rd_intg_seen >= 0 ? "seen" : "never raised"));
      if (mode == ModeWrIntg) begin
        expect_ok(rram_phy_ctx::wrintg_cycle >= 0 && wr_intg_seen >= 0,
                  $sformatf("bad write word at %0d: wr_intg_err %0s", rram_phy_ctx::wrintg_cycle,
                            wr_intg_seen >= 0 ? "seen" : "never raised"));
        expect_ok(eg2_rram_phy_wr_intg_errs(h) == 1, "the model saw no zero-filled word");
      end
      if (mode == ModeEcc) expect_ok(n_fatal_exp > 0 && fatal_seen >= 0, "no uncorrectable read seen/reported");
      if (mode == ModeDisable) expect_ok(dis_cycle >= 0, "rram_disable never driven");
      if (mode == ModeRandom && (n_host < 200 || n_ctrl_rd < 30 || n_commit < 10 || n_verify < 50))
        `uvm_error(get_type_name(), $sformatf("too little checked: %0d host reads, %0d controller reads, %0d commits, %0d shadow reads",
                                              n_host, n_ctrl_rd, n_commit, n_verify))
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: nothing was flipped")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      super.report_phase(phase);
      `uvm_info(get_type_name(), $sformatf({"checked against the model: %0d host reads (%0d error ",
          "responses, at most %0d outstanding), %0d controller reads, %0d controller write words, %0d ",
          "pushes, %0d commits; %0d RRAM reads, %0d shadow reads (%0d after a write invalidated the ",
          "entry); %0d+%0d corrected errors reported, %0d/%0d uncorrectable; %0d not predicted ",
          "(corrupted word); %0d checks, %0d failed; known finding: macro read request dropped %0d ",
          "times and changed %0d times before ack; after rram_disable: %0d rd_intg_err, %0d responses ",
          "not compared; after an uncorrectable read (known finding): %0d errors counted"}, n_host, n_host_err, max_out, n_ctrl_rd, n_ctrl_wr,
          n_push, n_commit, n_fill, n_verify, n_dropped_verify, n_corr1, n_corr2, n_fatal, n_fatal_exp,
          n_unpred, n_checks, n_err, n_unstable_drop, n_unstable_change, n_intg_after_dis, n_rsp_after_dis, n_after_unc), UVM_LOW)
      if (h != null) eg2_rram_phy_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_phy_cmodel_random_test extends rram_phy_base_test;
    `uvm_component_utils(rram_phy_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rram_phy_ctx::mode = ModeRandom;
    endfunction
    virtual function void build_phase(uvm_phase phase);
      set_mode();
      rram_phy_scoreboard::type_id::set_type_override(rram_phy_cmodel_scoreboard::get_type());
      test_seq = "rram_phy_cmodel_vseq";
      super.build_phase(phase);
    endfunction
  endclass

  class rram_phy_cmodel_ecc_test extends rram_phy_cmodel_random_test;
    `uvm_component_utils(rram_phy_cmodel_ecc_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rram_phy_ctx::mode = ModeEcc;
    endfunction
  endclass

  // Must fail while the uncorrectable-read finding is open: the same as the ECC test, the finding not
  // masked.
  class rram_phy_cmodel_ecc_unmasked_test extends rram_phy_cmodel_ecc_test;
    `uvm_component_utils(rram_phy_cmodel_ecc_unmasked_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "unmask_ecc", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass

  class rram_phy_cmodel_rdfault_test extends rram_phy_cmodel_random_test;
    `uvm_component_utils(rram_phy_cmodel_rdfault_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rram_phy_ctx::mode = ModeRdFault;
    endfunction
  endclass

  class rram_phy_cmodel_wrintg_test extends rram_phy_cmodel_random_test;
    `uvm_component_utils(rram_phy_cmodel_wrintg_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rram_phy_ctx::mode = ModeWrIntg;
    endfunction
  endclass

  class rram_phy_cmodel_disable_test extends rram_phy_cmodel_random_test;
    `uvm_component_utils(rram_phy_cmodel_disable_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void set_mode();
      rram_phy_ctx::mode = ModeDisable;
    endfunction
  endclass

  // Must fail: the scoreboard flips one bit of the first expected read.
  class rram_phy_cmodel_random_inject_fail_test extends rram_phy_cmodel_random_test;
    `uvm_component_utils(rram_phy_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass
endpackage
