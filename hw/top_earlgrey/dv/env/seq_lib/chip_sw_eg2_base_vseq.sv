// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Base of every EG2 framework test (eg2_top_verification, doc/eg2_test_framework.md, part 1).
//
// Before the CPU starts, it writes into the software image:
//   - kEg2Seed, the software's own seed, drawn from the run's seed: the test's random choices in
//     software (eg2_rand) come from it, so a run is reproduced from its seed alone;
//   - kEg2Param_<name>, one 32-bit word per test parameter, from the rand fields of the test's
//     own vseq, which overrides write_params() and holds the constraints.
//
// Each value can be pinned from the command line, which overrides the random draw:
//   +eg2_seed=<n>  +eg2_param_<name>=<n>
// Every value written is logged with where it came from (random or plusarg); the software prints
// what it read back, so a mismatch shows in the log. A symbol the image does not have is an
// error: a parameter renamed on one side only must not silently stop being randomised.
//
// The symbols are constants in .rodata (EG2_TEST_PARAMS in eg2_params.h), which the backdoor
// writes in the RRAM image. Writable globals cannot be used: the start-up code overwrites them.
class chip_sw_eg2_base_vseq extends chip_sw_base_vseq;
  `uvm_object_utils(chip_sw_eg2_base_vseq)

  `uvm_object_new

  // Never 0: the software reads 0 as "no bench wrote a seed".
  rand bit [31:0] sw_seed;
  constraint sw_seed_c {
    sw_seed != 0;
  }

  // UART0 carries the framework's console: the seed and parameters as the software read them, the
  // status module's findings (`STATUS ...`), each step, a trap (`CTRAP ...`) and the verdict. The
  // bench connects it and copies every line into the run log, so a failure explains itself.
  virtual task body();
    configure_uart_agent(.uart_idx(0), .enable(1));
    fork
      log_uart_lines(0);
    join_none
    super.body();
  endtask

  virtual task cpu_init();
    super.cpu_init();
    write_word("kEg2Seed", "eg2_seed", sw_seed);
    write_params();
  endtask

  protected task log_uart_lines(int uart_idx);
    uart_item item;
    string line;
    forever begin
      p_sequencer.uart_tx_fifos[uart_idx].get(item);
      if (item.data inside {8'h0a, 8'h0d}) begin
        if (line != "") `uvm_info("EG2_UART", line, UVM_LOW)
        line = "";
      end else begin
        line = {line, string'(item.data)};
      end
    end
  endtask

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
