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

// No system model: nothing is predicted.
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
                      "resets %0d (%0d not predicted); verdict \"%s\"; stop \"%s\""},
                     loads_predicted, load_mismatches, loads_event, stores, error_mismatches,
                     cap_accesses, resets, unpredicted_resets, verdict(), stop_reason());
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

  virtual function void close();
    if (h != null) eg2_sysmodel_release(h);
    h = null;
  endfunction
endclass
