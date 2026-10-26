// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of the EG2 memory, RRAM, debug and bus tests' sequences (software in
// sw/device/tests/eg2_mem, eg2_rram, eg2_debug, eg2_tl; their shared library eg2_chk.h).
//
// The software and the bench meet at lines of the software's DV log (`EG2_CHK_SYNC` / `LOG_INFO`
// in eg2_chk.h): the sequence waits for a line (wait_sw_line), then injects a fault or drives
// JTAG in zero or JTAG time, while the software waits a stated time or polls a flag the bench sets
// over SBA. Every deliberate fault is announced (announce): to the EG2 injection registry when it
// is compiled in (EG2_PERIPH_SB, decision 2026-10-24 "Deliberate corruption is announced"), and to
// the log in any case.
//
// `model_gaps` (every test's knob for what the SystemC model does not model) stays 0 on the chip;
// a test sequence never writes it.
class chip_sw_eg2_chk_base_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_chk_base_vseq)

  `uvm_object_new

  // Set by a test sequence that uses the debug module: the RV_DM TAP is strapped before reset, as
  // chip_jtag_base_vseq does.
  bit select_rv_dm_tap;

  virtual task pre_start();
    if (select_rv_dm_tap) cfg.chip_vif.tap_straps_if.drive(JtagTapRvDm);
    super.pre_start();
  endtask

  // Waits until the software's DV log prints a line that starts with `prefix`; returns the line.
  // With `wait_new`, the line already printed does not count (after a reset, the last line of the
  // previous boot is still there).
  // If the software ends (passed or failed) first, `line` is "" and the caller stops: a run that
  // failed early (an `_inject_fail` build) ends on its own verdict, not on this timeout.
  protected task wait_sw_line(input string prefix, output string line,
                              input int unsigned timeout_ns = 100_000_000, input bit wait_new = 0);
    bit found, ended;
    line = "";
    fork : isolation_fork
      begin
        fork
          begin
            if (wait_new) @(cfg.sw_logger_vif.printed_log_event);
            forever begin
              string s = string'(cfg.sw_logger_vif.printed_log);
              if (s.len() >= prefix.len() && s.substr(0, prefix.len() - 1) == prefix) begin
                line = s;
                found = 1'b1;
                break;
              end
              @(cfg.sw_logger_vif.printed_log_event);
            end
          end
          begin
            #(timeout_ns * 1ns);
          end
          begin
            wait (cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusPassed,
                                                                SwTestStatusFailed});
            ended = 1'b1;
          end
        join_any
        disable fork;
      end
    join
    if (!found && ended) begin
      `uvm_info(`gfn, $sformatf("the software ended before the line '%s'", prefix), UVM_LOW)
      return;
    end
    `DV_CHECK_FATAL(found, $sformatf("timed out after %0d ns waiting for the software line '%s'",
                                     timeout_ns, prefix))
    `uvm_info(`gfn, $sformatf("EG2 software line: %s", line), UVM_LOW)
  endtask

  // Announces a deliberate fault: `source` names the sequence, `targets` the checks that must see
  // it (comma-separated instance names), `kind` what it is.
  protected function void announce(string source, string targets, string kind);
`ifdef EG2_PERIPH_SB
    if (eg2_inject_registry::active()) begin
      void'(eg2_inject_registry::announce(source, targets, kind));
    end
`endif
    `uvm_info("EG2_INJECT", $sformatf("%s: %s on %s", source, kind, targets), UVM_LOW)
  endfunction

  // SBA access through the JTAG debugger model (the debug module active first). Returns the SBA
  // error status (0: none).
  protected task sba_wr32(bit [31:0] addr, bit [31:0] data, output sba_access_err_e status);
    logic [31:0] q[$];
    q.push_back(data);
    cfg.debugger.sba_write(addr, q, status);
    // A write the core does not make: the instruction-set model reads its own memory since review
    // R6 (2026-10-25), so it gets the word the bench wrote (a memory the write did not reach as
    // written then mismatches on the core's next load).
    if (status == SbaErrNone) eg2_iss_wrapper::backdoor_write32(addr, data);
  endtask

  protected task sba_rd32(bit [31:0] addr, output bit [31:0] data, output sba_access_err_e status);
    logic [31:0] q[$];
    cfg.debugger.sba_read(addr, q, status);
    data = q.size() > 0 ? q[0] : '0;
  endtask

  // The RRAM data array, by backdoor (rram_ctrl_bkdr_util): `offset` is a byte offset into the
  // data partition. inject_errors flips `nbits` random bits of the 144-bit row (data and ECC).
  protected function void rram_inject_errors(bit [31:0] offset, int unsigned nbits);
    cfg.mem_bkdr_util_h[RramData].inject_errors(offset, nbits);
  endfunction

endclass : chip_sw_eg2_chk_base_vseq
