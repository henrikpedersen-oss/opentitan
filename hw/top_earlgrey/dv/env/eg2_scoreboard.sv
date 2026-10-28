// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 chip scoreboard (doc/scoreboard/eg2_scoreboard_plan.md in eg2_top_verification).
//
// It has no agents and no models: it receives sequence items on analysis FIFOs, one per agent
// monitor (actual) and one per reference-model output (expected), and compares them. Every item
// is counted and written to <name>.chip.log in the run directory; a monitor that the test's
// stimulus reached but that saw nothing is an error (REQ_STE_04).
//
// Instruction comparator (phase 1): rvfi_exp carries the reference model's verdict on each
// retired instruction (eg2_iss_wrapper: Spike steps with the chip's RVFI and checks it); a
// mismatch is reported with the chip's last instructions before it.
//
// Plusargs:
//   +eg2_sb_log=0                 no per-item logs (counts only)
//   +sb_corrupt=<check>.<field>   checker fault injection (REQ_STE_03): rvfi.count, uart<N>.count
//                                 (the count is zeroed before it is checked); insn.rd (one register
//                                 result corrupted before Spike checks it). The run must fail.

class eg2_scoreboard extends uvm_scoreboard;
  `uvm_component_utils(eg2_scoreboard)

  chip_env_cfg cfg;

  // Actual side: one FIFO per agent monitor.
  uvm_tlm_analysis_fifo #(eg2_rvfi_item) rvfi_fifo;
  uvm_tlm_analysis_fifo #(uart_item)     uart_fifo[NUM_UARTS];
  // Expected side: the reference model's verdict per retired instruction.
  uvm_tlm_analysis_fifo #(eg2_rvfi_exp_item) rvfi_exp_fifo;

  // Set by chip_env: whether the RVFI agent has an interface (RVFI build), and whether the
  // reference model's instruction check runs (co-simulation build).
  bit rvfi_enabled;
  bit insn_enabled;

  protected int unsigned rvfi_count;
  protected int unsigned insn_count;
  protected int unsigned insn_mismatches;
  // The chip's last instructions, printed with a mismatch.
  protected string       history[$];
  localparam int unsigned HistoryLen = 8;
  protected int unsigned uart_count[NUM_UARTS];
  protected bit          en_log = 1'b1;
  protected int          rvfi_log;
  protected int          uart_log[NUM_UARTS];
  protected string       sb_corrupt;
  // An insn mismatch inside an announced injection (eg2_inject_registry) is EXPECTED rather than an
  // error, in every build; after one from a bench fault (`diverges`) the ISS (set by chip_env)
  // stops until the chip reset (2026-10-28, chip_cheriot_fi_meta).
  eg2_iss_wrapper        iss;
  protected int unsigned insn_expected;
`ifdef EG2_PERIPH_SB
  // Set by chip_env in an EG2 periph run (+eg2_periph_sb=1): the UARTs have their own scoreboards
  // (eg2_periph_env) and rvfi.chip.log gets the REQ_STE_10 header.
  bit                    periph_mode;
  protected eg2_sb_log   rvfi_slog;
`endif

`ifdef EG2_SYSMODEL
  // The EG2 SystemC model (eg2_sysmodel.sv): each UART's TX items compared with the model's, in
  // order; the bytes the chip received (the agents' RX items) go into the model.
  uvm_tlm_analysis_fifo #(uart_item) uart_rx_fifo[NUM_UARTS];
  protected string       uart_act[NUM_UARTS][$];
  protected int unsigned uart_compared[NUM_UARTS];
  protected int unsigned uart_mismatched[NUM_UARTS];
  // The model's items the chip had not sent when the run ended (a byte still on the line).
  localparam int unsigned UartInFlight = 2;
`endif

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    rvfi_fifo = new("rvfi_fifo", this);
    rvfi_exp_fifo = new("rvfi_exp_fifo", this);
    foreach (uart_fifo[i]) uart_fifo[i] = new($sformatf("uart_fifo%0d", i), this);
`ifdef EG2_SYSMODEL
    foreach (uart_rx_fifo[i]) uart_rx_fifo[i] = new($sformatf("uart_rx_fifo%0d", i), this);
`endif
    void'($value$plusargs("eg2_sb_log=%b", en_log));
    void'($value$plusargs("sb_corrupt=%s", sb_corrupt));
    if (sb_corrupt != "" &&
`ifdef EG2_PERIPH_SB
        // eg2_periph_env checks the names of an EG2 periph run.
        !eg2_inject_registry::active() &&
`endif
        !(sb_corrupt inside {"rvfi.count", "uart0.count", "uart1.count",
                                                  "uart2.count", "uart3.count", "insn.rd",
                                                  "insn.cheriot_rd"})) begin
      `uvm_fatal(`gfn, $sformatf("+sb_corrupt=%s: no such check.field", sb_corrupt))
    end
  endfunction

  function void start_of_simulation_phase(uvm_phase phase);
    super.start_of_simulation_phase(phase);
`ifdef EG2_PERIPH_SB
    if (periph_mode) begin
      if (rvfi_enabled) eg2_inject_registry::register_check("rvfi");
      if (insn_enabled) eg2_inject_registry::register_check("insn");
      if (rvfi_enabled && en_log) begin
        rvfi_slog = new("rvfi");
        rvfi_slog.set_header("the core's RVFI", "rv_core_ibex",
                             "the main core: one item per retired instruction, interrupt and reset",
                             "eg2_top_verification eg2_toplevel_verification_spec.md (REQ_CIB)",
                             "eg2_rvfi_agent (passive)", {"the instruction-set model ",
                             "(eg2_iss_wrapper; its verdicts are compared by the insn check)"},
                             cfg.rom_ext_assembled ? "boot secure" : "boot test ROM");
      end
      return;
    end
`endif
    if (!en_log) return;
    if (rvfi_enabled) rvfi_log = open_log("rvfi.chip.log");
    foreach (uart_log[i]) uart_log[i] = open_log($sformatf("uart%0d.chip.log", i));
  endfunction

  virtual task run_phase(uvm_phase phase);
    fork
      collect_rvfi();
      compare_insn();
      begin
        foreach (uart_fifo[i]) begin
          automatic int idx = i;
          fork collect_uart(idx); join_none
`ifdef EG2_SYSMODEL
          fork forward_uart_rx(idx); join_none
`endif
        end
      end
    join_none
  endtask

  protected task collect_rvfi();
    eg2_rvfi_item item;
    forever begin
      rvfi_fifo.get(item);
      if (!item.reset) rvfi_count++;
`ifdef EG2_PERIPH_SB
      if (rvfi_slog != null) rvfi_slog.line(item.log_line());
`endif
      if (rvfi_log) $fdisplay(rvfi_log, "%s", item.log_line());
    end
  endtask

  // Instruction comparator: the reference model's verdict on each instruction it stepped.
  protected task compare_insn();
    eg2_rvfi_exp_item exp;
    forever begin
      rvfi_exp_fifo.get(exp);
      insn_count++;
      if (!exp.match) begin
        string last;
        insn_mismatches++;
        foreach (history[i]) last = {last, "\n    ", history[i]};
        if (eg2_inject_registry::in_use()) begin
          string src = eg2_inject_registry::consult("insn");
          if (src != "") begin
            insn_mismatches--;
            insn_expected++;
            `uvm_info(`gfn, $sformatf("EG2_SB insn EXPECTED mismatch (injected by %s) at order %0d pc %08h: %s",
                                      src, exp.order, exp.pc, exp.errors), UVM_LOW)
            // The model no longer has the chip's state: no check until the reset the fault must
            // escalate to (a stop no reset ends fails the run, eg2_iss_wrapper check_phase).
            if (eg2_inject_registry::diverges_last() && iss != null) begin
              iss.stop_until_reset({"injected by ", src});
            end
            history.push_back(exp.act.log_line());
            if (history.size() > HistoryLen) void'(history.pop_front());
            continue;
          end
        end
        `uvm_error(`gfn, $sformatf({"EG2_SB insn mismatch at order %0d pc %08h: %s",
                                    "\n  chip: %s\n  instructions before it:%s"},
                                   exp.order, exp.pc, exp.errors, exp.act.log_line(), last))
      end
      history.push_back(exp.act.log_line());
      if (history.size() > HistoryLen) void'(history.pop_front());
    end
  endtask

  protected task collect_uart(int idx);
    uart_item item;
    forever begin
      uart_fifo[idx].get(item);
`ifdef EG2_PERIPH_SB
      if (periph_mode) continue;  // eg2_uart_sb has it
`endif
      uart_count[idx]++;
      if (uart_log[idx]) begin
        $fdisplay(uart_log[idx], "%02h %s", item.data,
                  (item.data inside {[8'h20:8'h7e]}) ? string'(item.data) : ".");
      end
`ifdef EG2_SYSMODEL
      uart_act[idx].push_back($sformatf("%02h %s", item.data,
                                        (item.data inside {[8'h20:8'h7e]}) ? string'(item.data)
                                                                          : "."));
      compare_uart(idx);
`endif
    end
  endtask

`ifdef EG2_SYSMODEL
  protected task forward_uart_rx(int idx);
    uart_item item;
    forever begin
      uart_rx_fifo[idx].get(item);
`ifdef EG2_PERIPH_SB
      if (periph_mode) continue;  // eg2_uart_sb gives the model its input
`endif
      eg2_sysmodel::give_input($sformatf("uart%0d.rx", idx), item.data);
    end
  endtask

  // The chip's items against the model's, in order, as far as both have come (the model's time
  // follows the chip's; its item can come a little after the chip's).
  protected function void compare_uart(int idx);
    string inst = $sformatf("uart%0d", idx);
    if (!eg2_sysmodel::active() || eg2_sysmodel::stop_reason() != "") return;
    eg2_sysmodel::step();
    while (uart_act[idx].size() > 0 && eg2_sysmodel::pending(inst) > 0) begin
      string act = uart_act[idx].pop_front();
      string exp = eg2_sysmodel::pull(inst);
      uart_compared[idx]++;
      if (eg2_sysmodel::corrupt == inst && uart_compared[idx] == 10) begin
        act = "00 .";  // +eg2_model_corrupt=uart<N>: the run must fail
        `uvm_info(`gfn, $sformatf("+eg2_model_corrupt=%s: item 10 corrupted", inst), UVM_LOW)
      end
      if (act != exp) begin
        uart_mismatched[idx]++;
        `uvm_error(`gfn, $sformatf("EG2_SB %s item %0d: chip \"%s\", model \"%s\"", inst,
                                   uart_compared[idx], act, exp))
      end
    end
  endfunction

  // At the end: what is left on either side.
  protected function void check_uart_end(int idx);
    string inst = $sformatf("uart%0d", idx);
    int    left;
    if (!eg2_sysmodel::active()) return;
    compare_uart(idx);
    if (eg2_sysmodel::stop_reason() != "") begin
      if (uart_act[idx].size() > 0) begin
        `uvm_info(`gfn, $sformatf("EG2_SB %s: %0d items after the model stopped, not compared",
                                  inst, uart_act[idx].size()), UVM_LOW)
      end
      return;
    end
    if (uart_act[idx].size() > 0) begin
      `uvm_error(`gfn, $sformatf("EG2_SB %s: the chip sent %0d items the model does not (first \"%s\")",
                                 inst, uart_act[idx].size(), uart_act[idx][0]))
    end
    left = eg2_sysmodel::pending(inst);
    if (left > UartInFlight) begin
      `uvm_error(`gfn, $sformatf("EG2_SB %s: the model sent %0d items the chip did not (first \"%s\")",
                                 inst, left, eg2_sysmodel::pull(inst)))
    end else if (left > 0) begin
      `uvm_info(`gfn, $sformatf("EG2_SB %s: %0d model items still in flight at the end", inst,
                                left), UVM_LOW)
    end
  endfunction
`endif

`ifdef EG2_PERIPH_SB
  // In an EG2 periph run the end checks come in the check phase, so that a deliberate corruption
  // they detect is counted before eg2_periph_env grades the injections; the UARTs are not here.
  function void check_phase(uvm_phase phase);
    int unsigned n_rvfi = rvfi_count;
    super.check_phase(phase);
    if (!periph_mode) return;
    if (sb_corrupt == "rvfi.count") begin
      void'(eg2_inject_registry::announce("sb_corrupt rvfi.count", "rvfi",
                                          "the RVFI item count zeroed before the check"));
      n_rvfi = 0;
    end
    if (insn_enabled && insn_count == 0) begin
      `uvm_error(`gfn, "EG2_SB insn: the reference model compared no instruction")
    end
    if (rvfi_enabled && n_rvfi == 0) begin
      string src = eg2_inject_registry::consult("rvfi");
      if (src != "") begin
        `uvm_info(`gfn, $sformatf({"EG2_SB rvfi EXPECTED mismatch (injected by %s): RVFI is ",
                                   "built but no instruction retired"}, src), UVM_LOW)
      end else begin
        `uvm_error(`gfn, "EG2_SB rvfi: RVFI is built but no instruction retired")
      end
    end
  endfunction
`endif

  function void report_phase(uvm_phase phase);
    string table_s;
    super.report_phase(phase);
`ifdef EG2_PERIPH_SB
    if (periph_mode) begin
      string s = $sformatf("%0d items ; insn %0d compared, %0d mismatched, %0d EXPECTED (injected) ; verdict %s",
                           rvfi_count, insn_count, insn_mismatches, insn_expected,
                           insn_mismatches == 0 ? "PASS" : "FAIL");
      if (rvfi_slog != null) rvfi_slog.finish(s);
      `uvm_info(`gfn, {"EG2_SB counts: rvfi ", s,
                       insn_enabled ? "" : " (no co-simulation build)"}, UVM_LOW)
      return;
    end
`endif
    case (sb_corrupt)
      "rvfi.count":  rvfi_count = 0;
      "uart0.count": uart_count[0] = 0;
      "uart1.count": uart_count[1] = 0;
      "uart2.count": uart_count[2] = 0;
      "uart3.count": uart_count[3] = 0;
      default: ;
    endcase
    table_s = $sformatf("\n  rvfi   %0d%s", rvfi_count, rvfi_enabled ? "" : " (no RVFI build)");
    table_s = {table_s, $sformatf("\n  insn   %0d compared, %0d mismatched, %0d EXPECTED (injected)%s",
                                  insn_count, insn_mismatches, insn_expected,
                                  insn_enabled ? "" : " (no co-simulation build)")};
    foreach (uart_count[i]) table_s = {table_s, $sformatf("\n  uart%0d  %0d", i, uart_count[i])};
`ifdef EG2_SYSMODEL
    foreach (uart_act[i]) check_uart_end(i);
    foreach (uart_compared[i]) begin
      table_s = {table_s, $sformatf("\n  uart%0d  %0d compared with the SystemC model, %0d mismatched",
                                    i, uart_compared[i], uart_mismatched[i])};
    end
    table_s = {table_s, "\n  model  ", eg2_sysmodel::summary()};
`endif
    `uvm_info(`gfn, {"EG2_SB counts (items seen per monitor):", table_s}, UVM_LOW)

    if (insn_enabled && insn_count == 0) begin
      `uvm_error(`gfn, "EG2_SB insn: the reference model compared no instruction")
    end
    if (rvfi_enabled && rvfi_count == 0) begin
      `uvm_error(`gfn, "EG2_SB rvfi: RVFI is built but no instruction retired")
    end
    foreach (uart_count[i]) begin
      if (cfg.eg2_sb_uart_required[i] && uart_count[i] == 0) begin
        `uvm_error(`gfn, $sformatf("EG2_SB uart%0d: the test uses it but its monitor saw nothing", i))
      end
    end
  endfunction

  function void final_phase(uvm_phase phase);
    super.final_phase(phase);
    if (rvfi_log) $fclose(rvfi_log);
    foreach (uart_log[i]) if (uart_log[i]) $fclose(uart_log[i]);
  endfunction

  protected function int open_log(string name);
    int fd = $fopen(name, "w");
    if (!fd) `uvm_error(`gfn, $sformatf("EG2_SB cannot open %s", name))
    return fd;
  endfunction
endclass
