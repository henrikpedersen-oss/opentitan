// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 injection registry (eg2_top_verification, status/decisions.md 2026-10-24 "Deliberate
// corruption is announced"; spec "Checker fault injection"). Compiled in every build; active with
// +eg2_periph_sb=1, and in any run once a bench fault is announced (in_use(): the instruction
// check of a co-simulation build consults it then too, 2026-10-28, chip_cheriot_fi_meta).
//
// Every source of an intended fault announces it here: the source ("eg2_mutate <name>",
// "sb_corrupt uart1.item", "eg2_sysmodel fault lockstep", "uart1 agent"), the checks it targets
// (instance names: "uart1", "spi_host0", "path", "insn", "rvfi"), its kind and its time window.
// The window (review R12, 2026-10-25: the default was "open until the end of the run", so after
// one fault every later mismatch of the check was EXPECTED, second boot included):
//   - by default it is narrow: open from `start` until each target has had `hits` mismatches
//     (one by default) or the next chip reset (chip_reset), whichever comes first;
//   - `stop` >= 0: the fixed window [start, stop], any number of mismatches inside it;
//   - `stop` == UntilEnd, only on purpose: until the end of the run (a mutation, which a reset does
//     not remove).
//
// A check that finds a mismatch consults the registry with its instance and the time the
// corrupted value was made:
//   - inside an announced injection that targets it: the mismatch is EXPECTED: the check reports
//     `EG2_SB <instance> EXPECTED mismatch (injected by <source>: <kind>)` at UVM_LOW and counts
//     it, no error;
//   - otherwise: an error, as without the registry.
// At the end (report()) every announced injection must have been detected by each check it
// targets that ran in this run (register_check): otherwise `EG2_SB <instance> MISSED injection
// <source>` is an error. A target whose check is not in the run is listed, not an error.
//
// A must-detect run (an `+sb_corrupt` run of an EG2 periph build) therefore passes exactly when its
// check saw its own corruption; a dead check fails it with MISSED.

class eg2_inject_registry;

  // `stop` of announce(): the narrow default, and open until the end of the run. R12, 2026-10-25.
  localparam realtime UntilConsumed = -1.0;
  localparam realtime UntilEnd      = -2.0;

  typedef struct {
    string   source;
    string   kind;
    string   targets[$];
    realtime start;
    realtime stop;       // UntilConsumed, UntilEnd, or the end of a fixed window
    bit      must_detect; // 0: stimulus with errors (bad parity, a NACK): may cause a mismatch, need not
    int      hits;       // UntilConsumed: the mismatches it covers per target
    bit      closed;     // UntilConsumed: ended by a chip reset
    bit      diverges;   // a fault in the chip the model cannot follow (not a checker corruption)
    int      detected[string];
  } entry_t;

  protected static entry_t entries[$];
  // The entry the last consult() matched (-1: none).
  protected static int     last_hit = -1;
  // The checks of this run that can detect an injection, by instance.
  protected static bit     checks[string];
  // Mismatches reported as EXPECTED, per instance.
  protected static int     expected_cnt[string];
  protected static bit     reported;

  // Whether the registry is in use: an EG2 periph run (+eg2_periph_sb=1).
  static function bit active();
    static bit init, on;
    if (!init) begin
      int v;
      init = 1'b1;
      on = $value$plusargs("eg2_periph_sb=%d", v) && v != 0;
    end
    return on;
  endfunction

  // Whether a check must consult the registry: an EG2 periph run, or any run with an announcement
  // (a bench fault of a co-simulation build).
  static function bit in_use();
    return active() || entries.size() > 0;
  endfunction

  // A check that can detect injections on `target` is part of this run.
  static function void register_check(string target);
    checks[target] = 1'b1;
  endfunction

  static function bit has_check(string target);
    return checks.exists(target);
  endfunction

  // Announces an injection; `targets` is a comma-separated list. Returns its id. The default window
  // covers `hits` mismatches per target and ends at the next chip reset (R12, 2026-10-25).
  // `diverges`: a fault in the chip (a bench fault on a bus or a memory), after which the reference
  // model no longer has the chip's state; a check that consumes it stops until the reset
  // (diverges_last). A checker corruption (sb_corrupt) leaves it 0.
  static function int announce(string source, string targets, string kind,
                               realtime start = $realtime, realtime stop = UntilConsumed,
                               bit must_detect = 1'b1, int hits = 1, bit diverges = 1'b0);
    entry_t e;
    string  t[$];
    split(targets, t);
    e.source = source;
    e.kind   = kind;
    e.targets = t;
    e.start  = start;
    e.stop   = stop;
    e.must_detect = must_detect;
    e.hits   = hits;
    e.diverges = diverges;
    entries.push_back(e);
    `uvm_info("EG2_INJECT", $sformatf("announced: %s (%s) on %s from %0t%s", source, kind,
                                      targets, start,
                                      stop == UntilEnd ? " until the end" :
                                      stop < 0 ? $sformatf(" for %0d mismatch(es), until a reset", hits) :
                                      $sformatf(" to %0t", stop)),
              UVM_LOW)
    return entries.size() - 1;
  endfunction

  // Closes the window of an open injection.
  static function void close(int id, realtime stop = $realtime);
    if (id >= 0 && id < entries.size()) entries[id].stop = stop;
  endfunction

  // A chip reset that began at `t`: every default-window injection announced before it ends (a
  // fault's consequences do not outlive the reset; the second boot is checked again). R12,
  // 2026-10-25.
  static function void chip_reset(realtime t);
    foreach (entries[i]) begin
      if (entries[i].stop == UntilConsumed && !entries[i].closed && entries[i].start < t) begin
        entries[i].closed = 1'b1;
        `uvm_info("EG2_INJECT", $sformatf("closed at the chip reset: %s (%s)", entries[i].source,
                                          entries[i].kind), UVM_MEDIUM)
      end
    end
  endfunction

  // A mismatch on `target`, of a value made at time `t`. Returns "" if no announcement covers it,
  // else "<source>: <kind>" (and counts the detection).
  static function string consult(string target, realtime t = $realtime);
    last_hit = -1;
    foreach (entries[i]) begin
      if (!(target inside {entries[i].targets})) continue;
      if (t < entries[i].start) continue;
      if (entries[i].stop >= 0 && t > entries[i].stop) continue;
      // The default window: consumed after `hits` mismatches of this target, ended by a reset.
      if (entries[i].stop == UntilConsumed &&
          (entries[i].closed ||
           (entries[i].detected.exists(target) && entries[i].detected[target] >= entries[i].hits))) begin
        continue;
      end
      if (!entries[i].detected.exists(target)) entries[i].detected[target] = 0;
      entries[i].detected[target]++;
      if (!expected_cnt.exists(target)) expected_cnt[target] = 0;
      expected_cnt[target]++;
      last_hit = i;
      return {entries[i].source, ": ", entries[i].kind};
    end
    return "";
  endfunction

  // Whether the injection the last consult() matched was announced with `diverges`.
  static function bit diverges_last();
    return last_hit >= 0 && entries[last_hit].diverges;
  endfunction

  // How many injections target `target`, and how many of them it detected.
  static function int injected_on(string target);
    int n;
    foreach (entries[i]) if (entries[i].must_detect && target inside {entries[i].targets}) n++;
    return n;
  endfunction

  static function int detected_on(string target);
    int n;
    foreach (entries[i]) begin
      if (entries[i].must_detect && target inside {entries[i].targets} &&
          entries[i].detected.exists(target)) n++;
    end
    return n;
  endfunction

  static function int expected_mismatches(string target);
    return expected_cnt.exists(target) ? expected_cnt[target] : 0;
  endfunction

  // One line per injection on `target`, for its <instance>.chip.log.
  static function void describe(string target, ref string lines[$]);
    foreach (entries[i]) begin
      if (!(target inside {entries[i].targets})) continue;
      lines.push_back($sformatf("%s (%s) from %0t%s: %s", entries[i].source, entries[i].kind,
                                entries[i].start,
                                entries[i].stop == UntilEnd ? " to the end" :
                                entries[i].stop < 0 ? $sformatf(" (%0d mismatch(es)%s)", entries[i].hits,
                                                                entries[i].closed ? ", closed at a reset" : "") :
                                $sformatf(" to %0t", entries[i].stop),
                                entries[i].detected.exists(target) ?
                                $sformatf("detected, %0d EXPECTED mismatches",
                                          entries[i].detected[target]) :
                                (!entries[i].must_detect ? "stimulus error, caused no mismatch" :
                                 has_check(target) ? "NOT detected" : "check not in this run")));
    end
  endfunction

  // At the end of the run: every injection must have been seen by every check it targets.
  static function void report();
    if (reported) return;
    reported = 1'b1;
    foreach (entries[i]) begin
      foreach (entries[i].targets[j]) begin
        string t = entries[i].targets[j];
        if (entries[i].detected.exists(t)) begin
          `uvm_info("EG2_INJECT", $sformatf("EG2_SB %s detected injection %s (%s): %0d EXPECTED mismatches",
                                            t, entries[i].source, entries[i].kind,
                                            entries[i].detected[t]), UVM_LOW)
        end else if (!entries[i].must_detect) begin
          `uvm_info("EG2_INJECT", $sformatf("EG2_SB %s: stimulus error %s (%s) caused no mismatch",
                                            t, entries[i].source, entries[i].kind), UVM_LOW)
        end else if (!has_check(t)) begin
          `uvm_info("EG2_INJECT", $sformatf({"EG2_SB %s: injection %s (%s) targets a check that is ",
                                             "not in this run"}, t, entries[i].source,
                                            entries[i].kind), UVM_LOW)
        end else begin
          `uvm_error("EG2_INJECT", $sformatf("EG2_SB %s MISSED injection %s (%s)", t,
                                             entries[i].source, entries[i].kind))
        end
      end
    end
  endfunction

  // For coverage (eg2_inject_cg): each injection and target, its source kind (0 a mutation, 1 a
  // checker corruption, 2 a model fault, 3 stimulus with errors) and its outcome (0 detected,
  // 1 MISSED, 2 the check was not in the run, 3 caused no mismatch).
  static function void outcomes(ref int unsigned kinds[$], ref int unsigned outs[$]);
    foreach (entries[i]) begin
      int unsigned k = 3;
      if (eg2_sb_log_has(entries[i].source, "eg2_mutate")) k = 0;
      else if (eg2_sb_log_has(entries[i].source, "sb_corrupt")) k = 1;
      else if (eg2_sb_log_has(entries[i].source, "eg2_sysmodel fault")) k = 2;
      foreach (entries[i].targets[j]) begin
        string t = entries[i].targets[j];
        kinds.push_back(k);
        if (entries[i].detected.exists(t)) outs.push_back(0);
        else if (!entries[i].must_detect) outs.push_back(3);
        else if (!has_check(t)) outs.push_back(2);
        else outs.push_back(1);
      end
    end
  endfunction

  protected static function bit eg2_sb_log_has(string s, string sub);
    for (int i = 0; i + sub.len() <= s.len(); i++) begin
      if (s.substr(i, i + sub.len() - 1) == sub) return 1'b1;
    end
    return 1'b0;
  endfunction

  protected static function void split(string s, ref string out[$]);
    string cur;
    for (int i = 0; i < s.len(); i++) begin
      if (s[i] == ",") begin
        if (cur != "") out.push_back(cur);
        cur = "";
      end else if (s[i] != " ") begin
        cur = {cur, s[i]};
      end
    end
    if (cur != "") out.push_back(cur);
  endfunction
endclass
