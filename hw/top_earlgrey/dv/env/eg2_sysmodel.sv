// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 system model in the chip bench (every EG2 co-simulation build: EG2_COSIM and
// EG2_SYSMODEL; doc/scoreboard/eg2_reference_model.png in eg2_top_verification). The bench reaches
// it through eg2_sysmodel::get(), one object chosen by factory override:
//
// - eg2_sysmodel: no system model; it predicts nothing, and every check that needs it is off.
// - eg2_sysmodel_systemc: libeg2sysmodel.so (hw/top_earlgrey/dv/eg2_model/dpi/eg2_sysmodel.h), the
//   devices and memories of the top without an instruction-set model. chip_base_test selects it
//   unless +eg2_sysmodel=0.
//
// The SystemC model is driven by:
//
// - eg2_iss_wrapper: at the first RVFI reset item the model is built with the images the bench
//   loaded into the chip (ROM and test ELFs, the OTP image) and the bench's backdoor symbol writes
//   (chip_sw_base_vseq's sw_symbol_backdoor_access, recorded here); every later reset item resets
//   it (the cause the model predicted, or power-on). Each retired instruction's data access goes to
//   the model at the chip's time: a store configures the model's device as it configured the
//   chip's; a load is compared with the chip's where the model predicts it (memory it knows, a
//   register no hardware updates), and is an input event otherwise (a register hardware updates,
//   memory it never saw written: the chip's value goes in, as Spike's load value does). Every
//   access's error response is compared. A capability access (CHERIoT mode) is not compared: the
//   bench's item does not give its memory format, so its bytes become unknown to the model.
// - eg2_scoreboard: each UART's items (uart<N>, TX) are compared, in order, with the model's;
//   the bytes the chip received (RX) go into the model as input events.
//
// When the model stops following (a feature it does not model yet, eg2_sysmodel_stop_reason), the
// run goes on without its predictions and the report says why.

import "DPI-C" function chandle eg2_sysmodel_create(string rom_elf, string test_elf,
                                                    string otp_vmem, string log_dir);
import "DPI-C" function int eg2_sysmodel_reset(chandle h, longint unsigned now_ps);
import "DPI-C" function void eg2_sysmodel_step(chandle h, longint unsigned now_ps);
import "DPI-C" function int eg2_sysmodel_read(chandle h, int unsigned addr, int unsigned size,
                                              output int unsigned data, output int err);
import "DPI-C" function void eg2_sysmodel_load_event(chandle h, int unsigned addr,
                                                     int unsigned size, int unsigned data);
import "DPI-C" function int eg2_sysmodel_write(chandle h, int unsigned addr, int unsigned size,
                                               int unsigned data);
import "DPI-C" function void eg2_sysmodel_forget(chandle h, int unsigned addr, int unsigned size);
import "DPI-C" function void eg2_sysmodel_backdoor_write8(chandle h, int unsigned addr,
                                                          byte unsigned data);
import "DPI-C" function int eg2_sysmodel_input(chandle h, string key, int unsigned value);
import "DPI-C" function void eg2_sysmodel_wfi(chandle h);
import "DPI-C" function int eg2_sysmodel_pending(chandle h, string inst);
import "DPI-C" function string eg2_sysmodel_pull(chandle h, string inst);
import "DPI-C" function string eg2_sysmodel_verdict(chandle h);
import "DPI-C" function string eg2_sysmodel_stop_reason(chandle h);
import "DPI-C" function string eg2_sysmodel_create_error();
import "DPI-C" function void eg2_sysmodel_release(chandle h);
`ifdef EG2_PERIPH_SB
import "DPI-C" function int eg2_sysmodel_fault(chandle h, string name, longint unsigned now_ps);
`endif
import "DPI-C" function int eg2_sysmodel_irq_sources(chandle h);
import "DPI-C" function int eg2_sysmodel_irq_class(chandle h, int id);
import "DPI-C" function string eg2_sysmodel_irq_name(chandle h, int id);
import "DPI-C" function int eg2_sysmodel_irq_enabled(chandle h, int id);
import "DPI-C" function int eg2_sysmodel_irq_level(chandle h, int id);
import "DPI-C" function int eg2_sysmodel_irq_aon(chandle h, int id);
import "DPI-C" function int eg2_sysmodel_irq_event(chandle h, output int id, output int level,
                                                   output longint unsigned t_ps);
import "DPI-C" function int eg2_sysmodel_core_irqs(chandle h);
import "DPI-C" function int eg2_sysmodel_plic_claims(chandle h, output int adopted,
                                                     output int unknown);

// One edge of an interrupt line: its new level and when (ps).
typedef struct {
  bit              level;
  longint unsigned t;
} eg2_irq_edge_t;

// No system model: nothing is predicted.
typedef class eg2_iss_wrapper;

class eg2_sysmodel extends uvm_object;
  `uvm_object_utils(eg2_sysmodel)

  protected static eg2_sysmodel inst;

  // For the report.
  int unsigned loads_predicted, loads_event, load_mismatches, stores, error_mismatches,
               cap_accesses, resets, unpredicted_resets;
  // +eg2_model_corrupt=load (the 100th predicted load's chip value inverted before it is compared)
  // or uart<N> (the scoreboard corrupts that UART's 10th item): checker fault injection, the run
  // must fail.
  string corrupt;

  function new(string name = "eg2_sysmodel");
    super.new(name);
  endfunction

  // The one model of the run, created at the first use (after chip_base_test's override).
  static function eg2_sysmodel get();
    if (inst == null) inst = eg2_sysmodel::type_id::create("eg2_sysmodel");
    return inst;
  endfunction

  // A model is selected for this run (it may not be built yet: that is the first reset item).
  virtual function bit enabled();
    return 1'b0;
  endfunction

  // The model is built and running.
  virtual function bit active();
    return 1'b0;
  endfunction

  static function longint unsigned now_ps();
    return longint'($realtime / 1ps);
  endfunction

  // A bench backdoor write into memory (sw_symbol_backdoor_access).
  virtual function void backdoor_write(bit [31:0] addr, bit [7:0] data[]);
  endfunction

  // An RVFI reset item: the chip left reset.
  virtual function void chip_reset(chip_env_cfg cfg);
  endfunction

  // Time advances to now (the scoreboard, before it compares or gives an input).
  virtual function void step();
  endfunction

  // One retired instruction's data access (eg2_iss_wrapper): what the model finds wrong with it.
  virtual function string access(eg2_rvfi_item item);
    return "";
  endfunction

  // The chip's clocks into the model.
  virtual task watch_clocks(virtual chip_if vif);
  endtask

  // An agent's item into the model, as the chip received it (uart<N>.rx, gpio.in).
  virtual function void give_input(string key, int unsigned value);
  endfunction

  virtual function int pending(string name);
    return 0;
  endfunction

  virtual function string pull(string name);
    return "";
  endfunction

  virtual function string verdict();
    return "";
  endfunction

  // Why the model stopped following ("" while it follows).
  virtual function string stop_reason();
    return "";
  endfunction

  function string summary();
    if (!enabled()) return "none (+eg2_sysmodel=0)";
    return $sformatf({"loads %0d predicted (%0d mismatched), %0d input events; stores %0d; ",
                      "error responses %0d mismatched; capability accesses %0d (not compared); ",
                      "resets %0d (%0d not predicted); verdict \"%s\"; stop \"%s\"; ",
                      "interrupts: %s"},
                     loads_predicted, load_mismatches, loads_event, stores, error_mismatches,
                     cap_accesses, resets, unpredicted_resets, verdict(), stop_reason(),
                     irq_summary());
  endfunction

  // The interrupt check (eg2_sysmodel_systemc): the chip's interrupt lines against the model's.
  virtual task watch_irqs(virtual chip_if vif);
  endtask

  virtual function string irq_summary();
    return "not checked (no model)";
  endfunction

  // The end of the run: the edges still waiting for a partner.
  virtual function void irq_finish();
  endfunction

  // The model's software stopped following the chip until the next reset (the ISS stops at
  // debug-mode entry, eg2_iss_wrapper debug_stopped): the model's interrupt lines are no
  // prediction until then.
  virtual function void irq_suspend(string why);
  endfunction

`ifdef EG2_PERIPH_SB
  // A fault the bench injects into the chip (a lockstep mismatch, cheriot_ena forced low), announced
  // in the injection registry: a mismatch it causes on `targets` (comma-separated: "insn", a
  // peripheral) is EXPECTED, and each of them must see one.
  virtual function void fault(string name, string targets);
    void'(eg2_inject_registry::announce({"eg2_sysmodel fault ", name}, targets,
                                        "fault injected into the chip and the model"));
  endfunction
`endif

  virtual function void close();
  endfunction
endclass

// The SystemC model (libeg2sysmodel.so).
class eg2_sysmodel_systemc extends eg2_sysmodel;
  `uvm_object_utils(eg2_sysmodel_systemc)

  localparam bit [31:0] Wfi = 32'h1050_0073;

  protected chandle      h;
  protected bit          created;
  protected string       stop_seen;
  protected int unsigned clk_hz[string];  // the chip's clocks as measured (watch_clocks)
  // Backdoor writes before the model exists (the symbols cpu_init writes).
  protected bit [31:0]   early_addr[$];
  protected bit [7:0]    early_data[$];

  // ---- The interrupt check (watch_irqs; Henrik 2026-11-07: "we need to check the interrupts in
  // the chip against the scoreboards interrupt predictions") ----
  // Lines 0 to 221 are rv_plic's sources by ID (0 reserved), 222 to 225 the core's software,
  // timer, external and NMI lines (IrqCore + 0..3). Each edge of a line, the chip's (as it happens)
  // and the model's (each rv_plic source change at the model's time, eg2_sysmodel_irq_event; the
  // core lines as polled), waits in a queue for its partner: the other side's edge to the same
  // level within the window (+eg2_irq_window_ns, default 20 us). The model is loosely timed (an
  // access takes effect at the retirement of its instruction, a timer at its compare time from the
  // chip's measured clocks), so edges are paired, not cycles. 20 us is 20 times the largest skew
  // the first runs showed on main-clock blocks (1.0 us: uart, rv_timer, plic_all_irqs). A block on
  // the always-on clock (aon_timer, sysrst_ctrl, adc_ctrl, sensor_ctrl, pwrmgr) gets
  // +eg2_irq_aon_cycles (default 10) periods of the chip's measured aon clock more (50 us at
  // 200 kHz): its register writes reach the aon domain through a clock-domain crossing and its
  // interrupt comes back through another (aon_timer doc/programmers_guide.md: a write "takes a few
  // aon clock cycles"), which the model applies at the write's retirement. The report gives the
  // largest skew seen on each kind of line, so the window can be narrowed from evidence. The
  // core's external line and NMI take the always-on window too (an always-on source drives them).
  // How each line is judged:
  //   class 1 (the model raises its hardware event; the core's software, timer and NMI lines):
  //     both ways: an edge of either side with none of the other within the window is an error;
  //   class 2 (the model predicts only what software does: INTR_TEST, INTR_ENABLE, INTR_STATE):
  //     each rising edge of the model's line needs the chip's line high within the window (an
  //     edge, or already high); the chip's line high while the model's INTR_ENABLE bit for it is 0
  //     for longer than the window is an error; the chip's other edges are not predicted (counted);
  //   class 3 (no register block in the model): not predicted (counted);
  //   the core's external line: both ways, but only while no class 2 or 3 source is high in the
  //     chip (one the model cannot know would raise it), and a window after that.
  // Nothing goes from the chip into the model: the model's lines are its own.
  localparam int IrqLines = 226;
  localparam int IrqCore  = 222;
  protected eg2_irq_edge_t   irq_chip_q[IrqLines][$];
  protected eg2_irq_edge_t   irq_model_q[IrqLines][$];
  protected bit              irq_chip_lvl[IrqLines];   // the chip's level now
  protected bit              irq_chip_base[IrqLines];  // the chip's level before its first queued edge
  protected bit [3:0]        irq_core_model;           // the model's core lines at the last poll
  protected int              irq_cls[IrqLines];
  protected string           irq_name[IrqLines];
  protected bit              irq_on;                   // checking (from the model's reset)
  protected bit              irq_disabled;             // +eg2_irq_check=0
  protected longint unsigned irq_window = 64'd20_000_000;  // ps
  protected longint unsigned irq_poll = 64'd1_000_000;     // ps
  protected int unsigned     irq_aon_cycles = 10;
  protected bit              irq_aon[IrqLines];       // a block on the always-on clock
  longint unsigned           irq_max_skew_aon;
  protected longint unsigned irq_gate_since[IrqLines];  // class 2: the chip high, INTR_ENABLE 0
  protected bit              irq_gate_told[IrqLines];
  protected longint unsigned irq_meip_open;             // MEIP judged again from this time
  // Counts for the report.
  int unsigned     irq_chip_edges, irq_model_edges, irq_matched, irq_mismatches, irq_not_predicted,
                   irq_meip_skipped, irq_unjudged;
  longint unsigned irq_max_skew;
  int unsigned     irq_np_by_module[string];
  int unsigned     irq_compared_by_class[int];

  function new(string name = "eg2_sysmodel_systemc");
    super.new(name);
  endfunction

  virtual function bit enabled();
    return 1'b1;
  endfunction

  virtual function bit active();
    return h != null;
  endfunction

  virtual function void backdoor_write(bit [31:0] addr, bit [7:0] data[]);
    foreach (data[i]) begin
      if (h != null) eg2_sysmodel_backdoor_write8(h, addr + i, data[i]);
      else begin
        early_addr.push_back(addr + i);
        early_data.push_back(data[i]);
      end
    end
  endfunction

  // The first reset item builds the model.
  virtual function void chip_reset(chip_env_cfg cfg);
    int cause;
    if (!created) begin
      // The 24-bit OTP image (chip_sim_cfg's gen_otp_images_mode keeps it as <name>.24.vmem); the
      // <name>.vmem the chip loads is the same content converted to RRAM rows.
      string otp = "";
      if (cfg.otp_images.exists(cfg.use_otp_image)) begin
        otp = cfg.otp_images[cfg.use_otp_image];
        otp = {otp.substr(0, otp.len() - 6), ".24.vmem"};
      end
      created = 1'b1;
      void'($value$plusargs("eg2_model_corrupt=%s", corrupt));
      h = eg2_sysmodel_create({cfg.sw_images[SwTypeRom], ".elf"},
                              {cfg.sw_images[SwTypeTestSlotA], ".elf"}, otp, ".");
      if (h == null) `uvm_fatal("EG2_SYSMODEL", $sformatf(
          "cannot build the SystemC model (libeg2sysmodel): %s", eg2_sysmodel_create_error()))
      foreach (early_addr[i]) eg2_sysmodel_backdoor_write8(h, early_addr[i], early_data[i]);
      early_addr.delete();
      early_data.delete();
      foreach (clk_hz[name]) give_input({"clk.", name}, clk_hz[name]);
      `uvm_info("EG2_SYSMODEL", $sformatf("SystemC model built: ROM %s.elf, test %s.elf, OTP %s",
                                          cfg.sw_images[SwTypeRom],
                                          cfg.sw_images[SwTypeTestSlotA], otp), UVM_LOW)
    end
    if (h == null) return;
    cause = eg2_sysmodel_reset(h, now_ps());
    irq_restart();
    if (cause < 0 && resets > 0) begin
      unpredicted_resets++;
      `uvm_info("EG2_SYSMODEL", "the chip reset where the model predicted none: a power-on reset",
                UVM_LOW)
    end
    resets++;
    void'(check_stop());
  endfunction

  virtual function void step();
    if (h != null) eg2_sysmodel_step(h, now_ps());
  endfunction

  virtual function string access(eg2_rvfi_item item);
    bit        store = item.mem_wmask != 0;
    bit [3:0]  mask  = store ? item.mem_wmask : item.mem_rmask;
    bit [31:0] data  = store ? item.mem_wdata : item.mem_rdata;
    bit [31:0] word0, model, bytes;
    int unsigned d0, d1;
    int        e0, e1, n, off, n0, predicted, err;
    string     errors;
    if (h == null) return "";
    eg2_sysmodel_step(h, now_ps());
    if (item.insn == Wfi && !item.trap) eg2_sysmodel_wfi(h);
    // A trapping load or store: its bus transactions as they were (item.mem_acc), each with its own
    // error response. Its RVFI masks are 0 when the fault was a bus error (Ibex with the writeback
    // stage), so it was skipped before and its error charged to the next access; a half of a
    // misaligned access PMP stopped never reached the bus and is not given. A fault the core raised
    // itself (PMP, CHERI) has none. Review R14, 2026-10-26.
    if (item.trap) return {trapped_access(item), check_stop()};
    if (mask == 0) return check_stop();
    if (item.mem_is_cap) begin
      cap_accesses++;
      if (store) eg2_sysmodel_forget(h, item.mem_addr, 8);
      return check_stop();
    end
    n     = $countones(mask);
    off   = item.mem_addr[1:0];
    n0    = (off + n > 4) ? 4 - off : n;
    word0 = {item.mem_addr[31:2], 2'b00};
    bytes = n == 4 ? '1 : (32'h1 << (8 * n)) - 1;
    if (store) begin
      stores++;
      err = eg2_sysmodel_write(h, item.mem_addr, n0, data);
      if (n0 < n) err |= eg2_sysmodel_write(h, word0 + 4, n - n0, data >> (8 * n0));
      if (err != int'(item.mem_error)) begin
        error_mismatches++;
        errors = $sformatf("model: store [%08h]: error response chip %0d, model %0d; ",
                           item.mem_addr, item.mem_error, err);
      end
      return {errors, check_stop()};
    end
    predicted = eg2_sysmodel_read(h, item.mem_addr, n0, d0, e0);
    d1 = 0;
    e1 = 0;
    if (n0 < n) predicted &= eg2_sysmodel_read(h, word0 + 4, n - n0, d1, e1);
    // A bench mailbox (eg2_iss_wrapper::backdoor_mailbox): the chip's value is an input event.
    if (predicted && eg2_iss_wrapper::is_mailbox(item.mem_addr)) predicted = 1'b0;
    model = (d0 | (n0 < n ? d1 << (8 * n0) : 0)) & bytes;
    err   = e0 | e1;
    if (err != int'(item.mem_error)) begin
      error_mismatches++;
      errors = $sformatf("model: load [%08h]: error response chip %0d, model %0d; ",
                         item.mem_addr, item.mem_error, err);
    end else if (!item.mem_error) begin
      if (predicted) begin
        loads_predicted++;
        if (corrupt == "load" && loads_predicted == 100) begin
          data = ~data;
          `uvm_info("EG2_SYSMODEL", $sformatf("+eg2_model_corrupt=load: load [%08h] corrupted",
                                              item.mem_addr), UVM_LOW)
        end
        if (model != (data & bytes)) begin
          load_mismatches++;
          errors = $sformatf("model: load [%08h] chip %08h, model %08h; ", item.mem_addr,
                             data & bytes, model);
        end
      end else begin
        // The model cannot know it: the chip's value is an input event.
        loads_event++;
        eg2_sysmodel_load_event(h, item.mem_addr, n0, data);
        if (n0 < n) eg2_sysmodel_load_event(h, word0 + 4, n - n0, data >> (8 * n0));
      end
    end
    return {errors, check_stop()};
  endfunction

  // The bus transactions of a trapping load or store (R14, 2026-10-26): each goes to the model, and
  // its error response is compared. A load's value is not compared (the instruction did not
  // complete); where the model cannot predict it, the chip's value is an input event as above. A
  // capability access's bytes become unknown, as above.
  protected function string trapped_access(eg2_rvfi_item item);
    string errors;
    foreach (item.mem_acc[i]) begin
      eg2_dside_access_t a = item.mem_acc[i];
      int unsigned       off, n, d;
      int                err;
      if (a.be == 0) continue;
      n   = $countones(a.be);
      off = 0;
      while (!a.be[off]) off++;
      if (item.mem_is_cap) begin
        cap_accesses++;
        if (a.store) eg2_sysmodel_forget(h, a.addr, 4);
        continue;
      end
      if (a.store) begin
        stores++;
        err = eg2_sysmodel_write(h, a.addr + off, n, a.wdata >> (8 * off));
      end else if (!eg2_sysmodel_read(h, a.addr + off, n, d, err) && !a.err) begin
        loads_event++;
        eg2_sysmodel_load_event(h, a.addr + off, n, a.rdata >> (8 * off));
      end
      if (err != int'(a.err)) begin
        error_mismatches++;
        errors = {errors, $sformatf("model: %s [%08h/%h] of a trapping instruction: error response chip %0d, model %0d; ",
                                    a.store ? "store" : "load", a.addr, a.be, a.err, err)};
      end
    end
    return errors;
  endfunction

  // The chip's clocks, measured, into the model (Eg2System's clocks): it times its peripherals by
  // them (UART bit time, timers). The AST oscillators start at a random frequency and are
  // calibrated later, so the nominal sim_dv frequencies are not the chip's. Each clock is averaged
  // over a window of edges; a window with a gap or a step in it (sleep, calibration) is skipped,
  // and a frequency is given to the model when it moves by more than 0.1 %. Clocks are inputs of
  // the chip, not its state: this is an input event like a UART byte.
  virtual task watch_clocks(virtual chip_if vif);
    fork
      measure_clock(vif, "main", 64);
      measure_clock(vif, "io_div4", 64);
      measure_clock(vif, "aon", 8);
    join_none
  endtask

  protected task clock_edge(virtual chip_if vif, string name);
    case (name)
      "main":    @(posedge vif.sys_clk);
      "io_div4": @(posedge vif.io_div4_clk);
      "aon":     @(posedge vif.aon_clk);
      default:   `uvm_fatal("EG2_SYSMODEL", {"no chip clock ", name})
    endcase
  endtask

  protected task measure_clock(virtual chip_if vif, string name, int unsigned n);
    forever begin
      realtime t0, tp, t, p, pmin, pmax;
      clock_edge(vif, name);
      t0 = $realtime;
      tp = t0;
      for (int unsigned i = 0; i < n; i++) begin
        clock_edge(vif, name);
        t = $realtime;
        p = t - tp;
        tp = t;
        if (i == 0 || p < pmin) pmin = p;
        if (i == 0 || p > pmax) pmax = p;
      end
      if (pmax <= pmin * 1.05) begin
        int unsigned hz;
        hz = $rtoi(real'(n) * 1.0e12 / ((tp - t0) / 1ps) + 0.5);
        if (!clk_hz.exists(name) || (hz > clk_hz[name] ? hz - clk_hz[name] : clk_hz[name] - hz) >
            clk_hz[name] / 1000) begin
          `uvm_info("EG2_SYSMODEL", $sformatf("chip clock %s: %0d Hz", name, hz), UVM_LOW)
          clk_hz[name] = hz;
          give_input({"clk.", name}, hz);
        end
      end
    end
  endtask

  virtual function void give_input(string key, int unsigned value);
    if (h == null) return;
    eg2_sysmodel_step(h, now_ps());
    if (eg2_sysmodel_input(h, key, value)) begin
      `uvm_fatal("EG2_SYSMODEL", $sformatf("the model takes no input %s", key))
    end
    void'(check_stop());
  endfunction

  virtual function int pending(string name);
    return h == null ? 0 : eg2_sysmodel_pending(h, name);
  endfunction

  virtual function string pull(string name);
    return h == null ? "" : eg2_sysmodel_pull(h, name);
  endfunction

  virtual function string verdict();
    return h == null ? "" : eg2_sysmodel_verdict(h);
  endfunction

  virtual function string stop_reason();
    return h == null ? "" : eg2_sysmodel_stop_reason(h);
  endfunction

  // Reports the model's stop once; always "" (no error: from then on nothing is a prediction).
  protected function string check_stop();
    string why = eg2_sysmodel_stop_reason(h);
    if (why != "" && stop_seen == "") begin
      stop_seen = why;
      `uvm_info("EG2_SYSMODEL", $sformatf({"the model stopped following at %0t: %s. From here ",
                                           "nothing is compared with it"}, $realtime, why), UVM_LOW)
    end
    return "";
  endfunction

`ifdef EG2_PERIPH_SB
  // The fault also goes into the model as an input event (eg2_sysmodel_fault).
  virtual function void fault(string name, string targets);
    super.fault(name, targets);
    if (h == null) return;
    if (eg2_sysmodel_fault(h, name, now_ps())) begin
      `uvm_fatal("EG2_SYSMODEL", $sformatf("the model knows no fault %s", name))
    end
    void'(check_stop());
  endfunction
`endif

  // ---- The interrupt check ----

  virtual task watch_irqs(virtual chip_if vif);
    longint unsigned ns;
    if ($test$plusargs("eg2_irq_check=0")) begin
      irq_disabled = 1'b1;
      return;
    end
    if ($value$plusargs("eg2_irq_window_ns=%d", ns)) irq_window = ns * 1000;
    if ($value$plusargs("eg2_irq_poll_ns=%d", ns)) irq_poll = ns * 1000;
    void'($value$plusargs("eg2_irq_aon_cycles=%d", irq_aon_cycles));
    fork
      irq_watch_chip(vif);
      forever begin
        @(negedge vif.irq_plic_rst_n);
        irq_on = 1'b0;  // judged again from the model's reset (irq_restart)
      end
      forever begin
        #(irq_poll * 1ps);
        irq_check(vif);
      end
    join_none
  endtask

  protected function bit [IrqLines-1:0] irq_chip_lines(virtual chip_if vif);
    bit [IrqLines-1:0] v;
    for (int i = 0; i < IrqCore; i++) v[i] = vif.irq_plic_src[i] === 1'b1;
    v[IrqCore + 0] = vif.irq_core_sw === 1'b1;
    v[IrqCore + 1] = vif.irq_core_timer === 1'b1;
    v[IrqCore + 2] = vif.irq_core_ext === 1'b1;
    v[IrqCore + 3] = vif.irq_core_nm === 1'b1;
    return v;
  endfunction

  // Every edge of the chip's lines, as it happens.
  protected task irq_watch_chip(virtual chip_if vif);
    bit [IrqLines-1:0] cur, prev;
    forever begin
      @(vif.irq_plic_src or vif.irq_core_sw or vif.irq_core_timer or vif.irq_core_ext or
        vif.irq_core_nm);
      cur = irq_chip_lines(vif);
      for (int i = 0; i < IrqLines; i++) begin
        if (cur[i] != prev[i]) begin
          if (irq_on) begin
            irq_chip_q[i].push_back('{level: cur[i], t: now_ps()});
            irq_chip_edges++;
          end
          irq_chip_lvl[i] = cur[i];
        end
      end
      prev = cur;
    end
  endtask

  // The model's reset (each chip reset item): both sides start again from their own levels.
  // The model's software stopped following the chip (the ISS at debug-mode entry, which no model
  // follows): its writes to rv_timer, rv_plic and the IPs stop reaching the model, so its lines
  // freeze while the chip's software runs on (2026-11-08, chip_eg2_rv_dm_counters_cheriot: the
  // chip's MTIP dropped after a resume, the model's could not). Not judged until the next reset
  // (irq_restart), as after a model stop.
  virtual function void irq_suspend(string why);
    if (!irq_on) return;
    irq_on = 1'b0;
    `uvm_info("EG2_IRQ", {"interrupt check off until the next reset: ", why}, UVM_LOW)
  endfunction

  protected function void irq_restart();
    int id, level;
    longint unsigned t;
    if (h == null || irq_disabled) return;
    if (irq_name[1] == "") begin
      for (int i = 0; i < IrqCore; i++) begin
        irq_cls[i] = eg2_sysmodel_irq_class(h, i);
        irq_name[i] = eg2_sysmodel_irq_name(h, i);
        irq_aon[i] = eg2_sysmodel_irq_aon(h, i) != 0;
        if (irq_name[i] == "") irq_name[i] = $sformatf("source %0d", i);
      end
      irq_name[IrqCore + 0] = "core software interrupt (MSIP)";
      irq_name[IrqCore + 1] = "core timer interrupt (MTIP)";
      irq_name[IrqCore + 2] = "core external interrupt (MEIP)";
      irq_name[IrqCore + 3] = "core NMI";
      irq_cls[IrqCore + 0] = 1;
      irq_cls[IrqCore + 1] = 1;
      irq_cls[IrqCore + 2] = 4;
      irq_cls[IrqCore + 3] = 1;
      irq_cls[0] = 0;
      // The external line follows every source, the NMI aon_timer's watchdog bark: both take the
      // always-on window.
      irq_aon[IrqCore + 2] = 1'b1;
      irq_aon[IrqCore + 3] = 1'b1;
    end
    while (eg2_sysmodel_irq_event(h, id, level, t)) ;
    irq_core_model = eg2_sysmodel_core_irqs(h);
    for (int i = 0; i < IrqLines; i++) begin
      irq_chip_q[i].delete();
      irq_model_q[i].delete();
      irq_chip_base[i] = irq_chip_lvl[i];
      irq_gate_since[i] = 0;
      irq_gate_told[i] = 1'b0;
      // Both sides leave reset with every line low (state and enable cleared): a chip line high
      // now is an edge the model must have. Not for a line high in the model too: a low-power exit
      // does not reset the always-on blocks, so aon_timer's wkup_timer_expired, which woke the
      // chip, is high on both sides and no edge follows (2026-11-08,
      // chip_sw_aon_timer_sleep_wdog_sleep_pause_cheriot: the restart at the wake-up's reset made
      // it an edge the model could never have).
      if (irq_chip_lvl[i] && irq_cls[i] inside {1, 4} &&
          !(i < IrqCore ? eg2_sysmodel_irq_level(h, i) != 0 : irq_core_model[i - IrqCore])) begin
        irq_chip_q[i].push_back('{level: 1'b1, t: now_ps()});
        irq_chip_base[i] = 1'b0;
      end
    end
    irq_meip_open = now_ps();
    irq_on = 1'b1;
  endfunction

  // Line i's window (ps).
  protected function longint unsigned irq_win(int i);
    longint unsigned aon_period = 64'd5_000_000;  // 200 kHz until the chip's clock is measured
    if (!irq_aon[i]) return irq_window;
    if (clk_hz.exists("aon") && clk_hz["aon"] > 0) aon_period = 64'd1_000_000_000_000 / clk_hz["aon"];
    return irq_window + irq_aon_cycles * aon_period;
  endfunction

  protected function void irq_error(int i, string what);
    irq_mismatches++;
    `uvm_error("EG2_IRQ", $sformatf("interrupt line %s (%0d): %s",
                                    irq_name[i], i, what))
  endfunction

  protected function longint unsigned irq_absdiff(longint unsigned a, longint unsigned b);
    return a > b ? a - b : b - a;
  endfunction

  // The chip's level of line i at time t (from its queued edges).
  protected function bit irq_chip_at(int i, longint unsigned t);
    bit l = irq_chip_base[i];
    foreach (irq_chip_q[i][k]) begin
      if (irq_chip_q[i][k].t > t) break;
      l = irq_chip_q[i][k].level;
    end
    return l;
  endfunction

  protected function void irq_count_np(int i);
    string mod = irq_name[i];
    int dot;
    irq_not_predicted++;
    for (dot = 0; dot < mod.len(); dot++) if (mod[dot] == ".") break;
    mod = mod.substr(0, dot - 1);
    irq_np_by_module[mod]++;
  endfunction

  protected function void irq_drop_chip(int i);
    irq_chip_base[i] = irq_chip_q[i][0].level;
    void'(irq_chip_q[i].pop_front());
  endfunction

  protected function void irq_check(virtual chip_if vif);
    longint unsigned now, t;
    int id, level;
    bit [3:0] core;
    bit gate;
    if (!irq_on || h == null) return;
    if (stop_seen != "" || eg2_sysmodel_stop_reason(h) != "") begin
      irq_on = 1'b0;  // the model no longer follows: nothing is a prediction
      return;
    end
    now = now_ps();
    eg2_sysmodel_step(h, now);
    void'(check_stop());
    while (eg2_sysmodel_irq_event(h, id, level, t)) begin
      if (id <= 0 || id >= IrqCore) continue;
      irq_model_q[id].push_back('{level: level[0], t: t});
      irq_model_edges++;
    end
    core = eg2_sysmodel_core_irqs(h);
    for (int k = 0; k < 4; k++) begin
      if (core[k] != irq_core_model[k]) begin
        irq_model_q[IrqCore + k].push_back('{level: core[k], t: now});
        irq_model_edges++;
      end
    end
    irq_core_model = core;
    // The core's external line is judged only while no source the model cannot know is high.
    gate = 1'b0;
    for (int i = 1; i < IrqCore; i++) begin
      if (irq_chip_lvl[i] && irq_cls[i] inside {2, 3}) gate = 1'b1;
    end
    if (gate) irq_meip_open = now + irq_window;
    if (now < irq_meip_open) begin
      irq_meip_skipped += irq_chip_q[IrqCore + 2].size() + irq_model_q[IrqCore + 2].size();
      irq_chip_q[IrqCore + 2].delete();
      irq_model_q[IrqCore + 2].delete();
      irq_chip_base[IrqCore + 2] = irq_chip_lvl[IrqCore + 2];
    end
    // Class 2: the chip's line high while the model's INTR_ENABLE bit is 0.
    for (int i = 1; i < IrqCore; i++) begin
      if (irq_cls[i] != 2) continue;
      if (irq_chip_lvl[i] && eg2_sysmodel_irq_enabled(h, i) == 0) begin
        if (irq_gate_since[i] == 0) irq_gate_since[i] = now;
        else if (now - irq_gate_since[i] > irq_win(i) && !irq_gate_told[i]) begin
          irq_gate_told[i] = 1'b1;
          irq_error(i, $sformatf({"the chip's line has been high since %0t while the model's ",
                                  "INTR_ENABLE bit for it is 0"}, irq_gate_since[i] * 1ps));
        end
      end else begin
        irq_gate_since[i] = 0;
        irq_gate_told[i] = 1'b0;
      end
    end
    for (int i = 1; i < IrqLines; i++) begin
      if (irq_chip_q[i].size() || irq_model_q[i].size()) irq_match(i, now);
    end
  endfunction

  // Pairs line i's queued edges; reports those that can no longer find a partner.
  protected function void irq_match(int i, longint unsigned now);
    int cls = irq_cls[i];
    longint unsigned w = irq_win(i);
    if (cls == 0 || cls == 3) begin
      foreach (irq_chip_q[i][k]) irq_count_np(i);
      irq_chip_q[i].delete();
      irq_model_q[i].delete();
      irq_chip_base[i] = irq_chip_lvl[i];
      return;
    end
    while (irq_model_q[i].size()) begin
      eg2_irq_edge_t m = irq_model_q[i][0];
      int found = -1;
      if (cls == 2 && !m.level) begin
        // A falling edge of a class 2 line: the chip's line may stay high on an event the model
        // does not have (INTR_ENABLE 0 is judged by the gate above).
        void'(irq_model_q[i].pop_front());
        continue;
      end
      foreach (irq_chip_q[i][k]) begin
        if (irq_chip_q[i][k].t > m.t + w) break;
        if (irq_chip_q[i][k].level == m.level &&
            irq_absdiff(irq_chip_q[i][k].t, m.t) <= w) begin
          found = k;
          break;
        end
      end
      if (found >= 0) begin
        for (int k = 0; k < found; k++) begin
          if (cls == 2) irq_count_np(i);
          else irq_error(i, $sformatf("the chip's line went %0d at %0t and the model's did not",
                                      irq_chip_q[i][0].level, irq_chip_q[i][0].t * 1ps));
          irq_drop_chip(i);
        end
        if (irq_aon[i]) begin
          if (irq_absdiff(irq_chip_q[i][0].t, m.t) > irq_max_skew_aon) begin
            irq_max_skew_aon = irq_absdiff(irq_chip_q[i][0].t, m.t);
          end
        end else if (irq_absdiff(irq_chip_q[i][0].t, m.t) > irq_max_skew) begin
          irq_max_skew = irq_absdiff(irq_chip_q[i][0].t, m.t);
        end
        irq_matched++;
        irq_compared_by_class[cls]++;
        irq_drop_chip(i);
        void'(irq_model_q[i].pop_front());
        continue;
      end
      if (cls == 2 && irq_chip_at(i, m.t)) begin
        // Already high in the chip when the model's line rose.
        irq_matched++;
        irq_compared_by_class[cls]++;
        void'(irq_model_q[i].pop_front());
        continue;
      end
      if (now > m.t + w) begin
        irq_error(i, $sformatf("the model's line went %0d at %0t and the chip's did not within %0t",
                               m.level, m.t * 1ps, w * 1ps));
        void'(irq_model_q[i].pop_front());
        continue;
      end
      break;  // the chip's edge may still come
    end
    while (irq_chip_q[i].size() && irq_chip_q[i][0].t + w < now) begin
      if (cls == 2) irq_count_np(i);
      else irq_error(i, $sformatf("the chip's line went %0d at %0t and the model's did not within %0t",
                                  irq_chip_q[i][0].level, irq_chip_q[i][0].t * 1ps,
                                  w * 1ps));
      irq_drop_chip(i);
    end
  endfunction

  virtual function string irq_summary();
    string s, np;
    int adopted, unknown;
    if (irq_disabled) return "not checked (+eg2_irq_check=0)";
    if (irq_name[1] == "") return "not checked (the model never started)";
    foreach (irq_np_by_module[m]) np = {np, $sformatf(" %s %0d", m, irq_np_by_module[m])};
    s = $sformatf({"%0d chip edges, %0d model edges, %0d paired (%0d on lines the model raises ",
                   "itself, %0d on software-driven lines, max skew %0t, %0t on always-on ",
                   "blocks), %0d mismatched, ",
                   "%0d chip edges not predicted (%s), %0d external-line edges skipped (a source ",
                   "the model cannot know was high), %0d unjudged at the end; window %0t (+%0d aon ",
                   "clock periods on always-on blocks)"},
                  irq_chip_edges, irq_model_edges, irq_matched,
                  irq_compared_by_class.exists(1) ? irq_compared_by_class[1] : 0,
                  irq_compared_by_class.exists(2) ? irq_compared_by_class[2] : 0,
                  irq_max_skew * 1ps, irq_max_skew_aon * 1ps, irq_mismatches, irq_not_predicted,
                  np == "" ? "none" : np.substr(1, np.len() - 1), irq_meip_skipped,
                  irq_unjudged, irq_window * 1ps, irq_aon_cycles);
    if (h != null && eg2_sysmodel_plic_claims(h, adopted, unknown)) begin
      s = {s, $sformatf({"; rv_plic claims where the model took the chip's ID (a race the model ",
                         "does not time): %0d, of them %0d of a source the model had not raised"},
                        adopted, unknown)};
    end
    return s;
  endfunction

  // The end of the run: edges younger than the window cannot be judged; older ones are.
  // Then the summary, in every build (the peripheral build does not print the model line).
  virtual function void irq_finish();
    longint unsigned now = now_ps();
    if (irq_on && h != null) begin
      for (int i = 1; i < IrqLines; i++) begin
        if (irq_chip_q[i].size() || irq_model_q[i].size()) irq_match(i, now);
        irq_unjudged += irq_chip_q[i].size() + irq_model_q[i].size();
      end
      irq_on = 1'b0;
    end
    `uvm_info("EG2_IRQ", {"interrupt check: ", irq_summary()}, UVM_LOW)
  endfunction

  virtual function void close();
    if (h != null) eg2_sysmodel_release(h);
    h = null;
  endfunction
endclass
