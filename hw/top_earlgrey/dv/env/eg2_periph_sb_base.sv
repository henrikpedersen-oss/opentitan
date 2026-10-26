// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Base of the EG2 per-peripheral scoreboards (status/decisions.md 2026-10-24: one scoreboard per
// peripheral, independent of eg2_scoreboard; expected items made from the peripheral's own TL-UL
// traffic by its extended monitor; deliberate corruption announced). Compiled with EG2_PERIPH_SB
// only, built with +eg2_periph_sb=1 only (eg2_periph_env).
//
// One instance per peripheral instance, with its own analysis FIFOs, all of the peripheral's item
// type ITEM_T:
//   act_fifo  the agent monitor's pin items: what the chip sent                    (actual)
//   exp_fifo  the items the extended monitor made from the peripheral's TL-UL writes (expected)
//   drv_fifo  what the bench drove into the chip, as the chip took it: the expected values of
//             the core's later reads of the receive registers                    (expected, RX)
//   rd_fifo   those reads, from the TL-UL read responses                          (actual, RX)
// Each direction is compared in order, item by item (compare()), as far as both sides have come.
// A control marker in an expected stream (eg2_periph_marker) keeps its place among the items: a
// flush drops what the peripheral will no longer send, keeping the first `keep` items as
// optional (they may be on the wire already); an optional item may be missing on the actual side.
//
// Every item is counted and logged to <instance>.chip.log (actual) and <instance>.expected.log
// (expected), both with the REQ_STE_10 header (eg2_sb_log) and an `# end:` line. At the end:
//   - expected items the chip did not send (beyond in_flight_max, still in a FIFO when the test
//     ended) and items the chip sent that nothing predicted are errors;
//   - a peripheral the test uses (the vseq said so, or the core gave it something to send) whose
//     monitor saw nothing is an error: `EG2_SB <instance>: the test uses it but its monitor saw
//     nothing`;
//   - a peripheral nobody predicts (predicted = 0) is counted and logged, with the verdict NOT
//     PREDICTED: never a silent pass.
// Every mismatch consults eg2_inject_registry: inside an announced injection on this instance it
// is reported as EXPECTED and counted, otherwise it is an error.
//
// Checker fault injection (REQ_STE_03): +sb_corrupt=<instance>.count zeroes the item count before
// the "used but saw nothing" check; +sb_corrupt=<instance>.item corrupts the n-th actual item
// (+sb_corrupt_at=<n>, default 1) before it is compared. Both are announced in the registry, so
// the run passes when the check reports them as EXPECTED and fails with MISSED when it does not.
// +eg2_sb_flip=<instance>.item corrupts the same item without the announcement: the run must fail
// with the instance's mismatch (a must-fail run).
//
// An actual item the monitor saw that is not an output of the chip (is_output(): a USB packet the
// host sent, a bus event) is logged and counted, not compared; the n-th item of +sb_corrupt counts
// the compared items only.

class eg2_periph_sb_base #(type ITEM_T = uvm_sequence_item) extends uvm_scoreboard;
  `uvm_component_param_utils(eg2_periph_sb_base #(ITEM_T))

  uvm_tlm_analysis_fifo #(ITEM_T) act_fifo;
  uvm_tlm_analysis_fifo #(ITEM_T) exp_fifo;
  uvm_tlm_analysis_fifo #(ITEM_T) drv_fifo;
  uvm_tlm_analysis_fifo #(ITEM_T) rd_fifo;
  // Every access at the peripheral's TL-UL port, classified against the documents' prediction of
  // an error response (eg2_tl_err): EXPECTED_ERR is counted and logged, UNEXPECTED_ERR (d_error
  // nobody predicted) and MISSING_ERR (a predicted error that did not come) are mismatches.
  uvm_tlm_analysis_fifo #(eg2_tl_access_item) tl_fifo;
  protected int unsigned n_tl, n_tl_exp_err, n_tl_unexp_err, n_tl_missing_err;
  eg2_tl_err_cg          tl_err_cg;

  chip_env_cfg cfg;

  // Set by eg2_periph_env before the run.
  string       inst;                 // "uart1"
  string       port = "port 0";
  string       block, used_for, spec;
  string       source = "the peripheral's TL-UL traffic, converted by its extended monitor";
  bit          predicted = 1'b1;     // 0: count and log only (NOT PREDICTED)
  bit          has_rx = 1'b0;        // the RX direction is checked
  bit          required;             // the test declared the peripheral used
  int unsigned in_flight_max = 1;    // expected items that may still be pending at the end

  typedef struct {
    ITEM_T   it;
    bit      optional;
    realtime t;
    bit      corrupted;
  } entry_t;

  protected entry_t      act_q[$], exp_q[$], drv_q[$], rd_q[$];
  protected int unsigned n_act, n_exp, n_cmp, n_mis, n_drv, n_rd, n_rd_cmp, n_rd_mis, n_unpred,
                         n_expected_mis;
  protected realtime     first_mis = -1;
  protected bit          unpredictable;
  protected string       unpredictable_why;
  protected string       sb_corrupt;
  protected string       sb_flip;              // +eg2_sb_flip: the same corruption, not announced
  protected int unsigned corrupt_at = 1;
  protected int unsigned n_out;                // output items +sb_corrupt may pick
  protected bit          flipped;
  protected int unsigned n_in_only;            // actual items logged only (not outputs)
  protected string       notes[$];             // comment lines for the end of the log
  protected int          corrupt_id = -1;
  protected bit          errors_at_end;
  eg2_sb_log             log, exp_log;
  protected bit          header_set;
  // The expected OUT items come from elsewhere (the SystemC model, pull_model): the TL-UL items
  // are counted and logged only.
  protected bit          exp_external;
  // The TL-UL items kept while exp_external, in case the run turns out to have no model.
  protected entry_t      ext_held_q[$];

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    act_fifo = new("act_fifo", this);
    exp_fifo = new("exp_fifo", this);
    drv_fifo = new("drv_fifo", this);
    rd_fifo  = new("rd_fifo", this);
    tl_fifo  = new("tl_fifo", this);
    void'($value$plusargs("sb_corrupt=%s", sb_corrupt));
    void'($value$plusargs("eg2_sb_flip=%s", sb_flip));
    void'($value$plusargs("sb_corrupt_at=%d", corrupt_at));
    if (corrupt_at == 0) corrupt_at = 1;
  endfunction

  // A line for the end of the log (before `# end:`), e.g. what a mode left NOT PREDICTED.
  function void note(string s);
    foreach (notes[i]) if (notes[i] == s) return;
    notes.push_back(s);
  endfunction

  function void start_of_simulation_phase(uvm_phase phase);
    super.start_of_simulation_phase(phase);
    log = new(inst);
    exp_log = new({inst, ".expected"});
    eg2_inject_registry::register_check(inst);
  endfunction

  // ----- Type-specific parts (the subclasses) -----

  // One log line for an item (also the form the model's items take, where it predicts).
  virtual function string item_line(ITEM_T it);
    return it.convert2string();
  endfunction

  // Whether the actual item matches the expected one; why not, if it does not.
  virtual function bit compare(ITEM_T act, ITEM_T exp, output string why);
    why = "";
    return act.compare(exp);
  endfunction

  // The same for the RX direction (a register read against what the bench drove).
  virtual function bit compare_rx(ITEM_T rd, ITEM_T drv, output string why);
    return compare(rd, drv, why);
  endfunction

  // Items of a byte-stream peripheral are split into one item per byte before they are compared
  // (rx: the RX direction's items); an item that is not an output of the chip is dropped.
  virtual function void split(ITEM_T it, ref ITEM_T out[$], input bit rx = 1'b0);
    out.push_back(it);
  endfunction

  // +sb_corrupt=<inst>.item: the item's content changed so that it no longer matches.
  virtual function void corrupt(ITEM_T it);
  endfunction

  // Whether an actual item is an output of the chip, to be compared (else logged only).
  virtual function bit is_output(ITEM_T it);
    return 1'b1;
  endfunction

  // Whether +sb_corrupt / +eg2_sb_flip may pick this output item: one the bench predicts (the
  // monitor marks a transaction it does not predict), so the corruption can be seen.
  virtual function bit corruptible(ITEM_T it);
    return 1'b1;
  endfunction

  // The agent's configuration, for the header.
  virtual function string agent_settings();
    return "";
  endfunction

  // Where the expected items come from, for the header.
  virtual function string predictor();
    if (!predicted) return {"nothing: NOT PREDICTED (", unpredictable_why, ")"};
    return source;
  endfunction

  // The SystemC model's items, where it predicts this peripheral (the UART): pulled into exp_q.
  virtual function void pull_model();
  endfunction

  // Functional coverage (eg2_periph_cov.sv): created by eg2_periph_env when coverage is on,
  // sampled from the items the scoreboard sees.
  virtual function void create_cov(string path);
  endfunction

  // An actual item; an expected one with `pending` expected items not yet sent before it.
  virtual function void cov_act(ITEM_T it);
  endfunction

  virtual function void cov_exp(ITEM_T it, int unsigned pending);
  endfunction

  // ----- The run -----

  virtual task run_phase(uvm_phase phase);
    fork
      forever begin
        ITEM_T it;
        act_fifo.get(it);
        take_act(it);
      end
      forever begin
        ITEM_T it;
        exp_fifo.get(it);
        take_exp(it);
      end
      forever begin
        ITEM_T it;
        drv_fifo.get(it);
        take_drv(it);
      end
      forever begin
        ITEM_T it;
        rd_fifo.get(it);
        take_rd(it);
      end
      forever begin
        eg2_tl_access_item it;
        tl_fifo.get(it);
        take_tl(it);
      end
    join_none
  endtask

  protected function void header();
    if (header_set) return;
    header_set = 1'b1;
    log.set_header(port, block, used_for, spec, agent_settings(), predictor(), config_line());
    exp_log.set_header(port, block, used_for, spec, agent_settings(),
                       {"this file holds the expected items: ", predictor()}, config_line());
  endfunction

  protected function string config_line();
    string boot = cfg.rom_ext_assembled ? "boot secure (ROM, ROM_EXT, owner image)" :
                  "boot test ROM";
    return $sformatf("%s, OTP image %s", boot, cfg.use_otp_image.name());
  endfunction

  protected function void take_act(ITEM_T it);
    ITEM_T parts[$];
    split(it, parts);
    foreach (parts[i]) begin
      entry_t e;
      bit     out;
      n_act++;
      e.it = parts[i];
      e.t  = $realtime;
      out = is_output(e.it);
      if (out && corruptible(e.it)) n_out++;
      if (out && corruptible(e.it) && sb_corrupt == {inst, ".item"} && n_out == corrupt_at &&
          corrupt_id < 0) begin
        corrupt(e.it);
        e.corrupted = 1'b1;
        corrupt_id = eg2_inject_registry::announce({"sb_corrupt ", inst, ".item"}, inst,
                                                   $sformatf("actual item %0d corrupted", n_act),
                                                   $realtime, $realtime);
      end else if (out && corruptible(e.it) && sb_flip == {inst, ".item"} && n_out == corrupt_at &&
                   !flipped) begin
        flipped = 1'b1;
        // Not announced: the mismatch it causes must fail the run.
        corrupt(e.it);
        log.comment($sformatf("%0t: +eg2_sb_flip: actual item %0d corrupted, not announced",
                              $realtime, n_act));
      end
      header();
      log.line({out ? "" : "(in) ", item_line(e.it)});
      cov_act(e.it);
      if (!out) begin
        n_in_only++;
        continue;
      end
      if (unpredictable || !predicted) begin
        n_unpred++;
        continue;
      end
      act_q.push_back(e);
    end
    pull_model();
    compare_out();
  endfunction

  protected function void take_exp(ITEM_T it);
    eg2_periph_marker::mark_t m;
    ITEM_T parts[$];
    if (eg2_periph_marker::is_marked(it, m) && m.kind != eg2_periph_marker::MarkOptional) begin
      // With the model as the expected source, the TL-UL stream's events apply to its kept items.
      if (exp_external) apply_marker(m, ext_held_q, "expected (TL-UL, kept)");
      else apply_marker(m, exp_q, "expected");
      eg2_periph_marker::forget(it);
      return;
    end
    split(it, parts);
    foreach (parts[i]) begin
      entry_t e;
      e.it = parts[i];
      e.t  = $realtime;
      e.optional = eg2_periph_marker::is_marked(it, m);
      n_exp++;
      header();
      exp_log.line({e.optional ? "(optional) " : "", item_line(e.it)});
      cov_exp(e.it, exp_q.size());
      if (!unpredictable && predicted && !exp_external) exp_q.push_back(e);
      else if (!unpredictable && predicted) ext_held_q.push_back(e);
    end
    eg2_periph_marker::forget(it);
    compare_out();
  endfunction

  protected function void take_drv(ITEM_T it);
    eg2_periph_marker::mark_t m;
    ITEM_T parts[$];
    bit    optional;
    if (eg2_periph_marker::is_marked(it, m) && m.kind != eg2_periph_marker::MarkOptional) begin
      apply_marker(m, drv_q, "RX expected");
      eg2_periph_marker::forget(it);
      return;
    end
    optional = eg2_periph_marker::is_marked(it, m);
    eg2_periph_marker::forget(it);
    split(it, parts, 1'b1);
    foreach (parts[i]) begin
      entry_t e;
      e.it = parts[i];
      e.t  = $realtime;
      e.optional = optional;
      n_drv++;
      header();
      exp_log.line({"< ", e.optional ? "(optional) " : "", item_line(e.it)});
      drv_q.push_back(e);
    end
    compare_in();
  endfunction

  protected function void take_rd(ITEM_T it);
    ITEM_T parts[$];
    split(it, parts, 1'b1);
    foreach (parts[i]) begin
      entry_t e;
      e.it = parts[i];
      e.t  = $realtime;
      n_rd++;
      header();
      log.line({"< ", item_line(e.it)});
      rd_q.push_back(e);
    end
    compare_in();
  endfunction

  function void create_tl_cov(string path);
    tl_err_cg = new({path, "::eg2_tl_err_cg"});
  endfunction

  protected function void take_tl(eg2_tl_access_item it);
    n_tl++;
    if (tl_err_cg != null) tl_err_cg.sample(int'(it.cls), it.opcode);
    case (it.cls)
      Eg2TlOk: ;
      Eg2TlExpectedErr: begin
        n_tl_exp_err++;
        header();
        log.line({"! ", it.line()});
      end
      Eg2TlUnexpectedErr: begin
        n_tl_unexp_err++;
        header();
        log.line({"! ", it.line()});
        mismatch($sformatf(" TL UNEXPECTED_ERR: an error response nobody predicted: %s", it.line()),
                 it.t);
      end
      Eg2TlMissingErr: begin
        n_tl_missing_err++;
        header();
        log.line({"! ", it.line()});
        mismatch($sformatf(" TL MISSING_ERR: a predicted error response did not come: %s",
                           it.line()), it.t);
      end
    endcase
  endfunction

  protected function void apply_marker(eg2_periph_marker::mark_t m, ref entry_t q[$],
                                       input string what);
    case (m.kind)
      eg2_periph_marker::MarkFlush: begin
        int dropped;
        while (q.size() > m.keep) begin
          void'(q.pop_back());
          dropped++;
        end
        foreach (q[i]) q[i].optional = 1'b1;
        header();
        log.comment($sformatf("%0t: %s: %0d pending %s items dropped, %0d kept as optional",
                              $realtime, m.why, dropped, what, q.size()));
      end
      eg2_periph_marker::MarkNack: begin
        foreach (q[i]) q[i].optional = 1'b1;
        header();
        log.comment($sformatf("%0t: %s: %0d pending %s items now optional", $realtime, m.why,
                              q.size(), what));
      end
      eg2_periph_marker::MarkUnpredicted: begin
        if (!unpredictable) begin
          unpredictable = 1'b1;
          unpredictable_why = m.why;
          header();
          log.comment($sformatf("%0t: from here NOT PREDICTED: %s", $realtime, m.why));
          `uvm_info(`gfn, $sformatf("EG2_SB %s: from here not predicted: %s", inst, m.why),
                    UVM_LOW)
          act_q.delete();
          exp_q.delete();
        end
      end
      default: ;
    endcase
  endfunction

  // The OUT direction: the chip's pin items against the expected ones, in order.
  protected function void compare_out();
    while (act_q.size() > 0 && exp_q.size() > 0) begin
      string why;
      entry_t a = act_q[0];
      entry_t e = exp_q[0];
      bit ok = compare(a.it, e.it, why);
      if (!ok && e.optional) begin
        // An optional item the chip did not send: skip it, try the next expected one.
        log.comment($sformatf("%0t: optional expected item not sent: %s", $realtime,
                              item_line(e.it)));
        void'(exp_q.pop_front());
        continue;
      end
      void'(act_q.pop_front());
      void'(exp_q.pop_front());
      n_cmp++;
      if (!ok) begin
        n_mis++;
        if (first_mis < 0) first_mis = a.t;
        mismatch($sformatf(" item %0d: chip \"%s\", expected \"%s\"%s", n_cmp, item_line(a.it),
                           item_line(e.it), why == "" ? "" : {" (", why, ")"}),
                 a.t, a.corrupted);
      end
    end
  endfunction

  // The RX direction: each read of a receive register against what the bench drove, in order.
  protected function void compare_in();
    while (rd_q.size() > 0 && drv_q.size() > 0) begin
      string why;
      entry_t r = rd_q[0];
      entry_t d = drv_q[0];
      bit ok = compare_rx(r.it, d.it, why);
      if (!ok && d.optional) begin
        log.comment($sformatf("%0t: optional RX item not read: %s", $realtime, item_line(d.it)));
        void'(drv_q.pop_front());
        continue;
      end
      void'(rd_q.pop_front());
      void'(drv_q.pop_front());
      n_rd_cmp++;
      if (!ok) begin
        n_rd_mis++;
        if (first_mis < 0) first_mis = r.t;
        mismatch($sformatf(" RX read %0d: the core read \"%s\", the bench drove \"%s\"%s", n_rd_cmp,
                           item_line(r.it), item_line(d.it), why == "" ? "" : {" (", why, ")"}),
                 r.t, 1'b0);
      end
    end
  endfunction

  // A mismatch: EXPECTED inside an announced injection on this instance, else an error.
  protected function void mismatch(string what, realtime t = $realtime, bit corrupted = 1'b0);
    string src = eg2_inject_registry::consult(inst, t);
    if (src != "") begin
      n_expected_mis++;
      if (log != null) log.comment($sformatf("EXPECTED mismatch (injected by %s)%s", src, what));
      `uvm_info(`gfn, $sformatf("EG2_SB %s EXPECTED mismatch (injected by %s)%s", inst, src, what),
                UVM_LOW)
    end else begin
      errors_at_end = 1'b1;
      if (log != null) log.comment($sformatf("MISMATCH%s", what));
      `uvm_error(`gfn, $sformatf("EG2_SB %s%s", inst, what))
    end
  endfunction

  // ----- The end of the run -----

  function void check_phase(uvm_phase phase);
    int unsigned left_exp, count;
    bit          used;
    super.check_phase(phase);
    pull_model();
    compare_out();
    compare_in();
    header();
    // What is left on either side.
    foreach (exp_q[i]) if (!exp_q[i].optional) left_exp++;
    if (act_q.size() > 0) begin
      mismatch($sformatf(": the chip sent %0d items nothing predicted (first \"%s\")",
                         act_q.size(), item_line(act_q[0].it)), act_q[0].t);
    end
    if (left_exp > in_flight_max) begin
      mismatch($sformatf(": the chip did not send %0d predicted items (first \"%s\")", left_exp,
                         item_line(exp_q[0].it)), exp_q[0].t);
    end else if (left_exp > 0) begin
      log.comment($sformatf("%0d predicted items still in flight at the end (at most %0d allowed)",
                            left_exp, in_flight_max));
    end
    if (rd_q.size() > 0) begin
      mismatch($sformatf(": the core read %0d values the bench never drove (first \"%s\")",
                         rd_q.size(), item_line(rd_q[0].it)), rd_q[0].t);
    end
    if (drv_q.size() > 0) begin
      log.comment($sformatf("%0d items the bench drove were not read by the core", drv_q.size()));
    end
    // A peripheral the test uses whose monitor saw nothing (REQ_STE_10). The vseq declares it
    // (required), or the run does (+eg2_sb_required=<instance>,...), or the core gave it
    // something to send.
    begin
      string req;
      if ($value$plusargs("eg2_sb_required=%s", req)) begin
        req = {",", req, ","};
        if (eg2_sb_log::has(req, {",", inst, ","})) required = 1'b1;
      end
    end
    used  = required || n_exp > 0;
    count = n_act;
    if (sb_corrupt == {inst, ".count"}) begin
      void'(eg2_inject_registry::announce({"sb_corrupt ", inst, ".count"}, inst,
                                          "item count zeroed before the check"));
      count = 0;
    end
    if (used && count == 0) mismatch(": the test uses it but its monitor saw nothing");
  endfunction

  function void report_phase(uvm_phase phase);
    string verdict, lines[$], summary;
    int    inj, det;
    super.report_phase(phase);
    inj = eg2_inject_registry::injected_on(inst);
    det = eg2_inject_registry::detected_on(inst);
    eg2_inject_registry::describe(inst, lines);
    foreach (lines[i]) log.comment({"injection: ", lines[i]});
    foreach (notes[i]) log.comment(notes[i]);
    if (errors_at_end) verdict = "FAIL";
    else if (!predicted) verdict = "NOT PREDICTED";
    else if (unpredictable) verdict = "PARTLY PREDICTED";
    else if (inj > det) verdict = "FAIL (MISSED injection)";
    else verdict = "PASS";
    summary = $sformatf({"%0d items%s, %0d compared, %0d mismatches%s", "%s ; injected %0d, ",
                         "detected %0d ; TL errors %0d expected, %0d unexpected, %0d missing ",
                        "(%0d accesses) ; verdict %s at %0.3f us"},
                        n_act, n_in_only > 0 ? $sformatf(" (%0d inputs, logged only)", n_in_only) : "",
                        n_cmp, n_mis,
                        first_mis >= 0 ? $sformatf(" (first at %0.3f us)", first_mis / 1us) : "",
                        has_rx ? $sformatf(" ; RX %0d reads, %0d compared, %0d mismatches",
                                           n_rd, n_rd_cmp, n_rd_mis) : "",
                        inj, det, n_tl_exp_err, n_tl_unexp_err, n_tl_missing_err, n_tl, verdict,
                        $realtime / 1us);
    log.finish(summary);
    exp_log.finish($sformatf("%0d expected items, %0d expected RX values", n_exp, n_drv));
    `uvm_info(`gfn, $sformatf("EG2_SB %s counts: %s", inst, summary), UVM_LOW)
  endfunction

  // For eg2_periph_env's summary table.
  function string counts();
    return $sformatf("%-10s %6d items %6d compared %4d mismatches %s", inst, n_act, n_cmp,
                     n_mis + n_rd_mis + n_tl_unexp_err + n_tl_missing_err,
                     predicted ? (unpredictable ? "(partly predicted)" : "") :
                     "(not predicted)");
  endfunction
endclass
