// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Base of every EG2 framework test (eg2_top_verification, doc/eg2_test_framework.md, part 1).
//
// Before the CPU starts, it writes into the software image:
//   - kEg2Seed, the software's own seed, drawn from the run's seed: the test's random choices in
//     software (eg2_rand) come from it, so a run is reproduced from its seed alone;
//   - kEg2Param_<name>, one 32-bit word per test parameter, from the rand fields of the test's
//     own vseq, which overrides write_params() and holds the constraints;
//   - kEg2StopOnFail, 1 for an _inject_fail image: the run ends at its first failed check.
//
// Each value can be pinned from the command line, which overrides the random draw:
//   +eg2_seed=<n>  +eg2_param_<name>=<n>  +eg2_stop_on_fail=<0|1>
// Every value written is logged with where it came from (random or plusarg); the software prints
// what it read back, so a mismatch shows in the log. A symbol the image does not have is an
// error: a parameter renamed on one side only must not silently stop being randomised.
//
// The symbols are constants in .rodata (EG2_TEST_PARAMS in eg2_params.h), which the backdoor
// writes in the RRAM image. Writable globals cannot be used: the start-up code overwrites them.
// A console character from the DV channel (eg2_dv_console_if.sv), put on UART0's monitor FIFO
// like a received byte so that every reader of the console lines works on both channels.
class eg2_console_item extends uart_item;
  // The line's UVM verbosity, from the software (eg2_verbosity_t).
  uvm_verbosity verbosity = UVM_LOW;
  `uvm_object_utils(eg2_console_item)
  `uvm_object_new
endclass

class chip_sw_eg2_base_vseq extends chip_sw_base_vseq;
  `uvm_object_utils(chip_sw_eg2_base_vseq)

  `uvm_object_new

  // Never 0: the software reads 0 as "no bench wrote a seed".
  rand bit [31:0] sw_seed;
  constraint sw_seed_c {
    sw_seed != 0;
  }

  // The framework's console: the seed and parameters as the software read them, the status
  // module's findings (`STATUS ...`), each step, a trap (`CTRAP ...`) and the verdict. It has two
  // channels (eg2_result.h). By default (`dv_console`, +eg2_dv_console=1) the software writes it
  // to the DV window, which costs no simulated time; each line is printed in run.log as `EG2_SW`
  // with the verbosity the software gave it (UVM_LOW unless it says otherwise), so +UVM_VERBOSITY
  // selects what is shown, and a line reporting a failure is always printed. UART0 then carries
  // only the verdict, `PASS!` or `FAIL! <first failing line>`, printed as `EG2_UART`. With
  // +eg2_dv_console=0 the whole console goes over UART0, as on every other platform.
  // The DV channel's characters are put on UART0's monitor FIFO (eg2_console_item), so each
  // reader of the console lines (this class, chip_sw_eg2_rand_vseq's bench steps, the boot
  // checks) sees one stream.
  bit dv_console = 1'b1;

  // The image prints on UART0 (the EG2 framework's console, or at least its verdict), so the EG2
  // scoreboard requires UART0 traffic. A vseq whose image does not (an OTTF image: OTTF reports on
  // the DV log in sim_dv, the verdict included) clears it before body().
  protected bit uart0_console = 1'b1;

  // What a console line came with: its channel and, from the DV channel, its verbosity.
  typedef struct {
    bit           dv;
    uvm_verbosity verbosity;
  } console_line_t;
  // The line being assembled per channel (console_char).
  protected string dv_line, uart_line;
  protected uvm_verbosity dv_line_verbosity = UVM_LOW;

  virtual task body();
    configure_uart_agent(.uart_idx(0), .enable(1));
    cfg.eg2_sb_uart_required[0] = uart0_console;
    fork
      forward_dv_console();
      log_uart_lines(0);
      log_boot_profile();
      uart_notional_reset();
    join_none
    super.body();
  endtask

  // The UART agents have no reset pin: a chip reset the chip makes itself (an escalation, a software
  // reset) that cut a character off mid-bit made the monitor report "Expect uart_tx stable"
  // (chip_cheriot_fi_meta, 2026-10-29). Each agent is in its notional reset while its UART is in
  // reset; the checks the test configured stay as they are (re_enable_chk_mon 0).
  protected task uart_notional_reset();
    foreach (cfg.m_uart_agent_cfgs[i]) begin
      automatic int idx = i;
      fork
        forever begin
          wait (cfg.chip_vif.uart_rst_n[idx] === 1'b0);
          cfg.m_uart_agent_cfgs[idx].reset_asserted();
          wait (cfg.chip_vif.uart_rst_n[idx] === 1'b1);
          cfg.m_uart_agent_cfgs[idx].reset_deasserted(.re_enable_chk_mon(1'b0));
        end
      join_none
    end
  endtask

  // Where the simulated time before the test goes: each boot step as it completes, so a choice
  // about shortening the boot is made on numbers. The test ROM's start and the test's start are
  // logged by the software status interface ("SW test transitioned to ...").
  protected task log_boot_profile();
    fork
      watch_core_progress();
    join_none
    fork
      begin
        wait (cfg.chip_vif.lc_ready);
        `uvm_info("EG2_BOOT", "life cycle ready (OTP and LC initialised)", UVM_LOW)
      end
      begin
        wait (cfg.chip_vif.rom_ctrl_done);
        `uvm_info("EG2_BOOT", $sformatf("ROM check done (good = %0d)", cfg.chip_vif.rom_ctrl_good),
                  UVM_LOW)
      end
      begin
        wait (cfg.chip_vif.pwrmgr_cpu_fetch_en);
        `uvm_info("EG2_BOOT", "CPU fetch enabled", UVM_LOW)
      end
      begin
        wait (cfg.chip_vif.cpu_rst_n === 1'b0);
        wait (cfg.chip_vif.cpu_rst_n === 1'b1);
        `uvm_info("EG2_BOOT", "CPU out of reset", UVM_LOW)
      end
    join
  endtask

  // A run that makes no progress must fail with a reason, not spin until the wall-clock limit kills
  // it with no verdict (chip_sw_eg2_status_smoketest[_cheriot]_secure_boot, 2026-11-04: 6.5 and 7 h,
  // 17 and 20 ms simulated, the core polling one register in a loop of a few instructions, while
  // the software timeout, 300 ms, was out of reach).
  // +eg2_stall_limit_ns=<n> (0, the default, is off): the core's PC (ID stage, sampled every
  // 10 us while the CPU is out of reset) staying inside one 256-byte window for n ns is an error
  // that names the window, so the loop can be looked up in the image's .dis. The window restarts
  // whenever the PC leaves it and at every CPU reset. While it is on, a progress line
  // (`EG2_PROGRESS`: simulated time, PC, how long the PC has been in its window) is printed every
  // +eg2_progress_ns (default 1 ms), so a slow run and a stuck run are told apart in run.log.
  // n must exceed the longest wait the software is meant to make in one place (the ROM's OTBN
  // signature check: about 6 ms).
  protected task watch_core_progress();
    localparam string PcPath =
        "tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_core.u_ibex_core.pc_id";
    localparam int unsigned Window = 256;
    longint unsigned limit_ns, progress_ns = 1_000_000;
    bit [31:0] pc, lo, hi, new_lo, new_hi;
    realtime   since, last_progress;
    bit        in_window;
    string     vals[$];
    // The last +eg2_stall_limit_ns wins, so a run's RUN_OPTS overrides the dvsim entry's value.
    if (uvm_cmdline_processor::get_inst().get_arg_values("+eg2_stall_limit_ns=", vals) == 0) return;
    limit_ns = vals[$].atoi();
    if (limit_ns == 0) return;
    void'($value$plusargs("eg2_progress_ns=%d", progress_ns));
    `uvm_info("EG2_PROGRESS", $sformatf("stall check on: the PC may stay in one %0d-byte window for %0d ns",
                                        Window, limit_ns), UVM_LOW)
    last_progress = $realtime;
    forever begin
      #10us;
      if (cfg.chip_vif.cpu_rst_n !== 1'b1) begin
        in_window = 1'b0;
        continue;
      end
      if (!uvm_hdl_read(PcPath, pc)) `uvm_fatal("EG2_PROGRESS", {"cannot read ", PcPath})
      new_lo = (in_window && lo < pc) ? lo : pc;
      new_hi = (in_window && hi > pc) ? hi : pc;
      if (!in_window || new_hi - new_lo >= Window) begin
        in_window = 1'b1;
        lo = pc;
        hi = pc;
        since = $realtime;
      end else begin
        lo = new_lo;
        hi = new_hi;
      end
      if ($realtime - last_progress >= progress_ns * 1ns) begin
        last_progress = $realtime;
        `uvm_info("EG2_PROGRESS", $sformatf("pc %08h, in the window %08h-%08h for %0.0f us", pc, lo,
                                            hi, ($realtime - since) / 1us), UVM_LOW)
      end
      if ($realtime - since >= limit_ns * 1ns) begin
        `uvm_error("EG2_PROGRESS", $sformatf({"EG2 stall: the core ran only between pc %08h and ",
                                              "%08h for %0.0f us (+eg2_stall_limit_ns=%0d): a loop ",
                                              "that does not end (look the PCs up in the image's ",
                                              ".dis)"}, lo, hi, ($realtime - since) / 1us, limit_ns))
        override_test_status_and_finish(.passed(1'b0));
        return;
      end
    end
  endtask

  // The instruction cache at boot, drawn from the run's seed so regressions run each test with the
  // cache on and off (EG2 spec, Boot modes). +eg2_icache=0|1 pins it (chip_sw_base_vseq).
  rand bit icache_at_boot;

  virtual task cpu_init();
    bit pinned;
    string mutation;
    super.cpu_init();
    if (!$value$plusargs("eg2_icache=%d", pinned)) set_otp_icache(icache_at_boot, "random");
    // Whether a must-fail image prints the EG2_INJECT markers (eg2_result.h): it does if it links
    // eg2_inject_failed. scripts/eg2_results.py then counts the run only with EG2_INJECT_CAUGHT
    // before its verdict. Read from the image's symbols, so also behind ROM_EXT.
    if (cfg.sw_images.exists(SwTypeTestSlotA) &&
        str_utils_pkg::str_has_substr(cfg.sw_images[SwTypeTestSlotA], "_inject_fail")) begin
      longint unsigned addr, size;
      bit markers = dv_utils_pkg::sw_symbol_get_addr_size(
          $sformatf("%0s.elf", cfg.sw_images[SwTypeTestSlotA]), "eg2_inject_failed", 1'b1, addr, size);
      `uvm_info(`gfn, $sformatf("EG2 must-fail image %0s the EG2_INJECT markers",
                                markers ? "prints" : "does not print"), UVM_LOW)
    end
    // Behind ROM_EXT the test is a signed owner image: a write into its .rodata would break the
    // signature, and ROM_EXT would rightly refuse to boot it. Until the overrides live outside the
    // signed region, a secure-boot run keeps the image's defaults (seed 0, default parameters).
    if (cfg.rom_ext_assembled) begin
      `uvm_info(`gfn, {"EG2 secure boot: the owner image is signed, so the seed, the parameters ",
                       "and the mutation name keep the image's defaults"}, UVM_LOW)
      return;
    end
    write_word("kEg2Seed", "eg2_seed", sw_seed);
    // A run that must fail (an _inject_fail image) ends at its first failed check (eg2_fail_stop
    // in eg2_result.c): that failure is its result, and the rest of the test would only take time.
    write_word("kEg2StopOnFail", "eg2_stop_on_fail",
               cfg.sw_images.exists(SwTypeTestSlotA) &&
               str_utils_pkg::str_has_substr(cfg.sw_images[SwTypeTestSlotA], "_inject_fail"));
    write_console_channel();
`ifdef EG2_PERIPH_SB
    // For the header of the EG2 scoreboards' logs (eg2_sb_log).
    begin
      bit [31:0] s = sw_seed;
      bit        ic = icache_at_boot;
      void'($value$plusargs("eg2_seed=%d", s));
      void'($value$plusargs("eg2_icache=%d", ic));
      eg2_sb_log::set_sw(s, ic ? "on" : "off");
    end
`endif
    if ($value$plusargs("eg2_mutate=%s", mutation)) write_mutation_name(mutation);
    write_params();
  endtask

  // The active mutation's name into kEg2MutationName (eg2_params.h), so the software's log names
  // it. The size must match the software's array exactly.
  localparam int unsigned MutationNameSize = 48;
  protected function void write_mutation_name(string name);
    bit [7:0] bytes[MutationNameSize];
    `DV_CHECK_LT_FATAL(name.len(), MutationNameSize, $sformatf("mutation name too long: %s", name))
    foreach (bytes[i]) bytes[i] = (i < name.len()) ? name[i] : 8'h00;
    sw_symbol_backdoor_overwrite("kEg2MutationName", bytes);
  endfunction

  protected task log_uart_lines(int uart_idx);
    uart_item item;
    string line;
    console_line_t cl;
    forever begin
      p_sequencer.uart_tx_fifos[uart_idx].get(item);
      if (console_char(item, cl, line)) log_console_line(line, cl);
    end
  endtask

  // The DV channel's words, one character each, onto UART0's monitor FIFO.
  protected task forward_dv_console();
    bit [31:0] w;
    eg2_console_item item;
    cfg.eg2_console_vif.console_addr = SW_DV_EG2_CONSOLE_ADDR;
    forever begin
      wait (cfg.eg2_console_vif.words.size() > 0);
      w = cfg.eg2_console_vif.words.pop_front();
      item = eg2_console_item::type_id::create("eg2_console_item");
      item.data = w[7:0];
      case (w[11:8])
        0:       item.verbosity = UVM_NONE;
        1:       item.verbosity = UVM_LOW;
        2:       item.verbosity = UVM_MEDIUM;
        3:       item.verbosity = UVM_HIGH;
        default: item.verbosity = UVM_DEBUG;
      endcase
      p_sequencer.uart_tx_fifos[0].analysis_export.write(item);
    end
  endtask

  // Adds one console character to `line`. Returns 1 when a line is complete: `line` then holds it
  // and `cl` its channel and verbosity, taken from its first character; the next call starts anew.
  // Lines from the two channels are kept apart, so a UART line in flight is not broken by
  // characters the DV channel delivers meanwhile.
  protected function bit console_char(uart_item item, ref console_line_t cl, ref string line);
    eg2_console_item dv_item;
    bit dv, eol;
    dv = $cast(dv_item, item);
    eol = item.data inside {8'h0a, 8'h0d};
    if (dv) begin
      if (eol) begin
        line = dv_line;
        dv_line = "";
        cl = '{dv: 1'b1, verbosity: dv_line_verbosity};
        return line != "";
      end
      if (dv_line == "") dv_line_verbosity = dv_item.verbosity;
      dv_line = {dv_line, string'(item.data)};
      return 1'b0;
    end
    if (eol) begin
      line = uart_line;
      uart_line = "";
      cl = '{dv: 1'b0, verbosity: UVM_LOW};
      return line != "";
    end
    uart_line = {uart_line, string'(item.data)};
    return 1'b0;
  endfunction

  // Prints a console line in run.log: `EG2_SW` with the software's verbosity for the DV channel,
  // `EG2_UART` at UVM_LOW for UART0. A line reporting a failure (FAIL, MISMATCH, a status finding
  // `STATUS `, a trap `TRAP`) is printed whatever the verbosity.
  protected function void log_console_line(string line, console_line_t cl);
    uvm_verbosity v = cl.verbosity;
    if (console_has(line, "FAIL") || console_has(line, "MISMATCH") ||
        console_has(line, "STATUS ") || console_has(line, "TRAP")) v = UVM_NONE;
    if (cl.dv) `uvm_info("EG2_SW", line, v)
    else       `uvm_info("EG2_UART", line, v)
  endfunction

  protected function bit console_has(string line, string word);
    for (int i = 0; i + word.len() <= line.len(); i++) begin
      if (line.substr(i, i + word.len() - 1) == word) return 1'b1;
    end
    return 1'b0;
  endfunction

  // kEg2ConsoleDv: the console's channel (dv_console, +eg2_dv_console=0|1).
  protected function void write_console_channel();
    bit on = dv_console;
    bit [7:0] bytes[4];
    void'($value$plusargs("eg2_dv_console=%d", on));
    bytes = '{8'(on), 8'h00, 8'h00, 8'h00};
    sw_symbol_backdoor_overwrite("kEg2ConsoleDv", bytes);
    `uvm_info(`gfn, on ? {"EG2 console on the DV channel (lines in run.log as EG2_SW; UART0 ",
                          "carries the verdict)"} : "EG2 console on UART0", UVM_LOW)
  endfunction

  // Overridden by a test's vseq: one set_param() per parameter, from its rand fields.
  virtual function void write_params();
  endfunction

  // Writes the test parameter `name` (kEg2Param_<name>); +eg2_param_<name>=<n> overrides it.
  protected function void set_param(string name, bit [31:0] value);
    write_word({"kEg2Param_", name}, {"eg2_param_", name}, value);
  endfunction

  protected function void write_word(string symbol, string plusarg, bit [31:0] value);
    bit [7:0] bytes[4];
    bit [31:0] pinned;
    string from = "random";
    if ($value$plusargs({plusarg, "=%d"}, pinned)) begin
      value = pinned;
      from = {"+", plusarg};
    end
    // Little-endian: bytes[0] is the lowest address.
    foreach (bytes[i]) bytes[i] = value[8 * i +: 8];
    sw_symbol_backdoor_overwrite(symbol, bytes);
    `uvm_info(`gfn, $sformatf("EG2 %s = %0d (0x%08x, %s)", symbol, value, value, from), UVM_LOW)
  endfunction

endclass : chip_sw_eg2_base_vseq
