// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// alert_handler against the alert handler reference model (hw/top_earlgrey/dv/eg2_model/ip/
// eg2_alert_core.h, shared with eg2sim, through eg2_alert_cmodel_pkg). On with
// +alert_handler_cmodel=1 (cfg.en_cmodel), next to OpenTitan's scoreboard.
//
// The model sees only the block's ports and its TL-UL accesses, never a signal inside the design:
//   - each alert the alert agents receive (a completed alert handshake on alert_tx/alert_rx);
//   - the outcome of the pings the design chooses (its LFSR timer is not predicted) and the signal
//     integrity failures, as the local alerts alert_handler.hjson names: an alert ping that timed out
//     (local alert 0), an escalation ping that timed out (1), an alert integrity failure (2), an
//     escalation integrity failure (3); the first and third for enabled alerts only (the documents
//     do not say; a finding);
//   - the low-power groups' lpg_cg_en / lpg_rst_en (MuBi4True: in low power);
//   - every TL-UL write as it completes, every read's data.
// Checked: every register read against the model's expectation (the fields hardware updates only
// while settled; the escalation counters within a tolerance); while settled, every cycle, the four
// escalation lines (esc_p 1 / esc_n 0 held for two cycles: a ping is a single pulse) and the four
// class interrupt pins; the crash dump output once the model has latched it. The documents give no
// cycle latency for the alert handshake or the start of Phase0, so the design counts as settled
// ("quiet") only when no alert, local alert, low-power-group change or register write has happened
// for `settle` cycles; a level difference counts only once it has lasted `settle` cycles.
class alert_handler_cmodel_sb extends uvm_component;
  `uvm_component_utils(alert_handler_cmodel_sb)

  alert_handler_env_cfg cfg;
  int unsigned settle = 24;   // +alert_handler_cmodel_settle: cycles without an event
  int unsigned esc_tol = 24;  // +alert_handler_cmodel_esc_tol: ESC_CNT tolerance in cycles
  // +alert_handler_cmodel_corrupt=1: the first exactly predicted read is expected with bit 0
  // inverted, so the run must fail.
  bit corrupt;
  bit verbose;                // +alert_handler_cmodel_verbose=1: the model's notes

  uvm_tlm_analysis_fifo #(tl_seq_item)    tl_d_fifo;
  uvm_tlm_analysis_fifo #(alert_seq_item) alert_fifo[NUM_ALERTS];
  uvm_tlm_analysis_fifo #(esc_seq_item)   esc_fifo[NUM_ESCS];

  alert_handler_cmodel_cov cov;

  protected chandle          h;
  protected longint unsigned cyc;
  protected longint unsigned last_event;  // the cycle of the last input event
  protected int unsigned     esc_diff[NUM_ESCS];
  protected int unsigned     intr_diff[4];
  protected bit [NUM_ESCS-1:0] esc_prev;
  protected bit [alert_handler_reg_pkg::NLpg-1:0] lpg_last;
  protected bit              in_reset = 1'b1;
  protected bit              dump_checked;

  // Counts for the report.
  protected int unsigned n_rd, n_rd_exact, n_rd_tol, n_esc_cmp, n_intr_cmp, n_mismatch, n_writes;
  protected int unsigned n_alerts, n_local, n_lpg, n_dump_cmp;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_d_fifo = new("tl_d_fifo", this);
    foreach (alert_fifo[i]) alert_fifo[i] = new($sformatf("alert_fifo[%0d]", i), this);
    foreach (esc_fifo[i]) esc_fifo[i] = new($sformatf("esc_fifo[%0d]", i), this);
    void'($value$plusargs("alert_handler_cmodel_settle=%d", settle));
    void'($value$plusargs("alert_handler_cmodel_esc_tol=%d", esc_tol));
    void'($value$plusargs("alert_handler_cmodel_corrupt=%b", corrupt));
    void'($value$plusargs("alert_handler_cmodel_verbose=%b", verbose));
    if (cfg.en_cov) cov = alert_handler_cmodel_cov::type_id::create("alert_handler_cmodel_cov");
    h = eg2_alert_cmodel_pkg::eg2_alert_create(NUM_ALERTS);
    eg2_alert_cmodel_pkg::eg2_alert_set_tol(h, esc_tol);
    eg2_alert_cmodel_pkg::eg2_alert_verbose(h, verbose);
    // Each alert's low-power group: the top's configuration (top_earlgrey alert_lpgs).
    for (int i = 0; i < NUM_ALERTS; i++) begin
      eg2_alert_cmodel_pkg::eg2_alert_lpg_map(h, i, alert_handler_reg_pkg::LpgMap[i]);
    end
  endfunction

  task run_phase(uvm_phase phase);
    fork
      watch_reset();
      forever begin
        @(posedge cfg.clk_rst_vif.clk);
        tick();
      end
      forever process_tl();
      process_alerts();
      process_escs();
    join_none
  endtask

  protected task watch_reset();
    forever begin
      @(negedge cfg.clk_rst_vif.rst_n);
      in_reset = 1'b1;
      eg2_alert_cmodel_pkg::eg2_alert_reset(h, cyc);
      tl_d_fifo.flush();
      foreach (alert_fifo[i]) alert_fifo[i].flush();
      foreach (esc_fifo[i]) esc_fifo[i].flush();
      esc_diff = '{default: 0};
      intr_diff = '{default: 0};
      esc_prev = '0;
      lpg_last = '0;
      dump_checked = 1'b0;
      last_event = cyc;
      note("block reset");
      @(posedge cfg.clk_rst_vif.rst_n);
      in_reset = 1'b0;
      last_event = cyc;
    end
  endtask

  protected function bit quiet();
    return cyc - last_event >= settle;
  endfunction

  protected function void event_seen();
    last_event = cyc;
  endfunction

  // ---- Every cycle: time, low-power groups, levels ----

  protected function void tick();
    bit [alert_handler_reg_pkg::NLpg-1:0] lpg_now;
    cyc++;
    if (in_reset || cfg.clk_rst_vif.rst_n !== 1'b1) return;
    eg2_alert_cmodel_pkg::eg2_alert_time(h, cyc);
    for (int g = 0; g < alert_handler_reg_pkg::NLpg; g++) begin
      lpg_now[g] = cfg.alert_handler_vif.lpg_cg_en[g] == prim_mubi_pkg::MuBi4True ||
                   cfg.alert_handler_vif.lpg_rst_en[g] == prim_mubi_pkg::MuBi4True;
    end
    if (lpg_now !== lpg_last) begin
      for (int g = 0; g < alert_handler_reg_pkg::NLpg; g++) begin
        if (lpg_now[g] != lpg_last[g]) eg2_alert_cmodel_pkg::eg2_alert_lpg(h, g, lpg_now[g]);
      end
      n_lpg++;
      event_seen();
      lpg_last = lpg_now;
    end
    check_levels();
    if (cov != null) cov.sample_model(h);
  endfunction

  protected function void check_levels();
    bit q = quiet();
    // Not at a state change of the model: the documents fix no cycle latency for the trigger,
    // Phase0 or a phase's end.
    bit near = eg2_alert_cmodel_pkg::eg2_alert_near_transition(h) != 0;
    for (int e = 0; e < NUM_ESCS; e++) begin
      bit now_lvl = cfg.esc_device_cfg[e].vif.esc_tx.esc_p === 1'b1 &&
                    cfg.esc_device_cfg[e].vif.esc_tx.esc_n === 1'b0;
      bit dut = now_lvl && esc_prev[e];
      bit mdl = eg2_alert_cmodel_pkg::eg2_alert_esc(h, e) != 0;
      esc_prev[e] = now_lvl;
      if (!q || near) begin
        esc_diff[e] = 0;
        continue;
      end
      n_esc_cmp++;
      if (dut != mdl) begin
        esc_diff[e]++;
        if (esc_diff[e] == settle) begin
          n_mismatch++;
          `uvm_error(`gfn, $sformatf("escalation %0d: alert_handler %0b, model %0b for %0d cycles (%0s)",
                                     e, dut, mdl, settle, eg2_alert_cmodel_pkg::eg2_alert_last_note(h)))
        end
      end else begin
        esc_diff[e] = 0;
      end
    end
    begin
      bit [3:0] pins = cfg.intr_vif.pins[3:0];
      bit [3:0] mdl = eg2_alert_cmodel_pkg::eg2_alert_intr_state(h) &
                      eg2_alert_cmodel_pkg::eg2_alert_intr_enable(h);
      for (int k = 0; k < 4; k++) begin
        if (!q) begin
          intr_diff[k] = 0;
          continue;
        end
        n_intr_cmp++;
        if (pins[k] != mdl[k]) begin
          intr_diff[k]++;
          if (intr_diff[k] == settle) begin
            n_mismatch++;
            `uvm_error(`gfn, $sformatf("class %0d (A=0) interrupt pin: alert_handler %0b, model %0b for %0d cycles",
                                       k, pins[k], mdl[k], settle))
          end
        end else begin
          intr_diff[k] = 0;
        end
      end
    end
    // The crash dump, once latched: held until every escalated class is cleared (theory of
    // operation "Crashdump Output"), so it compares exactly.
    if (q && !dump_checked && eg2_alert_cmodel_pkg::eg2_alert_dump_latched(h)) begin
      check_dump();
      dump_checked = 1'b1;
    end else if (!eg2_alert_cmodel_pkg::eg2_alert_dump_latched(h)) begin
      dump_checked = 1'b0;
    end
  endfunction

  // The latched crash dump. The documents say the dump is captured "upon entering" the
  // CLASSx_CRASHDUMP_TRIGGER_SHADOWED phase (alert_handler.hjson) and then held, but not whether the
  // held class states and escalation counters are those of the last cycle before that phase or of
  // its first cycle (a finding); those two members (class_esc_state, class_esc_cnt: the low
  // 4*3 + 4*32 bits) are not compared. The alert causes, local causes and accumulator counts are.
  protected function void check_dump();
    localparam int unsigned Skip = 4 * 3 + 4 * alert_handler_reg_pkg::EscCntDw;
    bit [NUM_CRASHDUMP-1:0] dut = cfg.crashdump_vif.pins;
    bit [NUM_CRASHDUMP-1:0] exp = '0;
    int unsigned nw = eg2_alert_cmodel_pkg::eg2_alert_dump_words(h);
    n_dump_cmp++;
    for (int w = 0; w < nw; w++) begin
      bit [31:0] word = eg2_alert_cmodel_pkg::eg2_alert_dump_word(h, w);
      for (int b = 0; b < 32; b++) if (32 * w + b < NUM_CRASHDUMP) exp[32 * w + b] = word[b];
    end
    if ((dut >> Skip) !== (exp >> Skip)) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf({"crashdump (alert causes, local causes, accumulators): ",
                                  "alert_handler %0h, model %0h"}, dut >> Skip, exp >> Skip))
    end
  endfunction

  // ---- Alert and escalation channels (the agents' monitors) ----

  protected task process_alerts();
    foreach (alert_fifo[i]) begin
      automatic int idx = i;
      fork
        forever begin
          alert_seq_item it;
          alert_fifo[idx].get(it);
          if (in_reset) continue;
          eg2_alert_cmodel_pkg::eg2_alert_time(h, cyc);
          if (it.m_trans_type == AlertSigTrans && !it.m_ping_timeout &&
              it.m_alert_handshake_sta == AlertReceived) begin
            n_alerts++;
            note($sformatf("alert %0d", idx));
            eg2_alert_cmodel_pkg::eg2_alert_alert(h, idx);
            event_seen();
          end else if (it.m_trans_type == AlertIntFail && alert_en(idx)) begin
            local_alert(2, $sformatf("alert %0d integrity failure", idx));
          end else if (it.m_trans_type == AlertPingTrans && it.m_ping_timeout && alert_en(idx)) begin
            local_alert(0, $sformatf("alert %0d ping timeout", idx));
          end
        end
      join_none
    end
  endtask

  protected task process_escs();
    foreach (esc_fifo[i]) begin
      automatic int idx = i;
      fork
        forever begin
          esc_seq_item it;
          esc_fifo[idx].get(it);
          if (in_reset) continue;
          eg2_alert_cmodel_pkg::eg2_alert_time(h, cyc);
          if (it.m_trans_type == EscIntFail ||
              (it.m_esc_handshake_sta == EscHSIntFail && !it.m_ping_timeout)) begin
            local_alert(3, $sformatf("escalation %0d integrity failure", idx));
          end else if (it.m_trans_type == EscPingTrans && it.m_ping_timeout) begin
            local_alert(1, $sformatf("escalation %0d ping timeout", idx));
          end
        end
      join_none
    end
  endtask

  // The alert's enable as the model holds it (ALERT_EN_SHADOWED_i committed).
  protected function bit alert_en(int idx);
    return eg2_alert_cmodel_pkg::eg2_alert_csr_value(
        h, 32'(cfg.ral.alert_en_shadowed[idx].get_offset())) & 1;
  endfunction

  protected function void local_alert(int unsigned i, string what);
    n_local++;
    note(what);
    eg2_alert_cmodel_pkg::eg2_alert_local(h, i);
    event_seen();
  endfunction

  // ---- Registers ----

  protected task process_tl();
    tl_seq_item  it;
    int unsigned addr, exp, mask, tol;
    bit          q;
    tl_d_fifo.get(it);
    if (in_reset || it.d_error) return;
    // The register's offset: the bench randomises the block's base address (cip_base_env_cfg).
    addr = 32'(it.a_addr - cfg.ral.default_map.get_base_addr()) & ~32'h3;
    eg2_alert_cmodel_pkg::eg2_alert_time(h, cyc);
    q = quiet();
    if (it.is_write()) begin
      bit [31:0] be = {{8{it.a_mask[3]}}, {8{it.a_mask[2]}}, {8{it.a_mask[1]}}, {8{it.a_mask[0]}}};
      n_writes++;
      eg2_alert_cmodel_pkg::eg2_alert_csr_write(h, addr, it.a_data, be);
      event_seen();
      return;
    end
    eg2_alert_cmodel_pkg::eg2_alert_csr_read(h, addr, q, exp, mask, tol);
    n_rd++;
    if (tol != 0 && q) begin
      int unsigned d = it.d_data;
      n_rd_tol++;
      if ((d > exp ? d - exp : exp - d) > tol) begin
        n_mismatch++;
        `uvm_error(`gfn, $sformatf("read 0x%03h: alert_handler %0d, model %0d (tolerance %0d)",
                                   addr, d, exp, tol))
      end
      return;
    end
    if (tol != 0) return;  // a counter while unsettled
    if (mask != 0) n_rd_exact++;
    if (corrupt && mask[0]) begin
      exp[0] = ~exp[0];
      corrupt = 1'b0;
      note($sformatf("+alert_handler_cmodel_corrupt: read 0x%03h expected with bit 0 inverted", addr));
    end
    if (((it.d_data ^ exp) & mask) != 0) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf("read 0x%03h: alert_handler 0x%08h, model 0x%08h, compared bits 0x%08h%0s",
                                 addr, it.d_data, exp, mask, q ? "" : " (settling)"))
    end
  endtask

  protected function void note(string s);
    if (verbose) `uvm_info(`gfn, $sformatf("@%0d %0s", cyc, s), UVM_LOW)
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"alert_handler model: %0d reads (%0d with compared bits, %0d counters within ",
                               "tolerance), %0d writes, %0d alerts, %0d local alerts, %0d LPG ",
                               "changes, %0d escalation and %0d interrupt samples, %0d crash dumps ",
                               "compared, %0d mismatches"},
                              n_rd, n_rd_exact, n_rd_tol, n_writes, n_alerts, n_local, n_lpg,
                              n_esc_cmp, n_intr_cmp, n_dump_cmp, n_mismatch), UVM_LOW)
    if (cfg.en_cmodel && n_rd == 0 && n_esc_cmp == 0) begin
      `uvm_error(`gfn, "alert_handler model: compared nothing")
    end
  endfunction

endclass
