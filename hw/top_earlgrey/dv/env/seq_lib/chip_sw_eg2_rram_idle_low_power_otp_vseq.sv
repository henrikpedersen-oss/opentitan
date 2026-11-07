// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_idle_low_power_otp (sw/device/tests/eg2_rram/eg2_rram_idle_low_power.c, operation 1;
// EG2 spec REQ_RRAM_26, REQ_SEC_21: an OTP write in flight at the low-power request, in each of its
// two RRAM writes, the data word's and the integrity word's; the sleep type).
//
// The bench times the request. For each OTP write the software prints
// `EG2 RRAM otp sleep step <s> flag 0x<addr>` and starts the DAI write. The sequence waits until
// rram_ctrl's OTP plug enters the step's RRAM write (rram_ctrl theory_of_operation.md "Integrity
// Scheme": the data word in `StWrite`, the integrity word in `StWriteIntg`), holds it there by
// keeping the plug's write-data ready low (no data reaches the RRAM; the plug stays in the step),
// and writes s + 1 to the software's flag over SBA. The software then executes WFI with low power
// requested, so pwrmgr evaluates the request while the step is in flight. The sequence records
// that the core slept while it held the step, and on `EG2 RRAM otp release` checks that the plug is
// still in the step and lets the write go on. The software checks the outcome (WAKE_INFO.ABORT,
// the word and its integrity read back). A chip reset while the step is held (a power-down the
// documents rule out) releases the hold; the software's second boot then finds the word short of
// its bits.
//
// The hold is a delay, not a corruption: nothing is announced to the injection registry.
class chip_sw_eg2_rram_idle_low_power_otp_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_idle_low_power_otp_vseq)

  function new(string name = "");
    super.new(name);
    select_rv_dm_tap = 1'b1;
  endfunction

  localparam string Otp = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl.u_rram_ctrl_otp";
  // The two paths as names of their own: a DV_CHECK macro quotes its expression, and Xcelium
  // rejects a string literal inside it (xmvlog INSSTR).
  localparam string OtpWready = {Otp, ".wready_i"};
  localparam string OtpState = {Otp, ".state_q"};
  localparam string CoreSleeping =
      "tb.dut.top_earlgrey.earlgrey_pd_aon.u_pwrmgr.pwr_cpu_i.core_sleeping";
  // rram_ctrl_otp's state encoding of the two write steps (its state_e). Observation only: which
  // state the plug is in says where the request landed; the expected outcome is the documents'.
  localparam bit [11:0] StWrite = 12'b100111110111;
  localparam bit [11:0] StWriteIntg = 12'b110001101111;

  rand int unsigned otp_writes, otp_step, sleep_type, wake_ticks;
  bit dm_active;

  constraint knobs_c {
    otp_writes inside {[2:8]};
    // Mostly the two steps in turn, so every run has both.
    otp_step dist {2 := 80, 0 := 10, 1 := 10};
    sleep_type inside {[0:1]};
    wake_ticks inside {[10:40]};
  }

  virtual function void write_params();
    set_param("operation", 1);
    set_param("otp_writes", otp_writes);
    set_param("otp_step", otp_step);
    set_param("sleep_type", sleep_type);
    set_param("wake_ticks", wake_ticks);
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  // Holds the OTP plug inside step `step` (0 data, 1 integrity) until the software's release line.
  protected task hold(int unsigned step, bit [31:0] flag);
    string name = step == 0 ? "StWrite (the data word)" : "StWriteIntg (the integrity word)";
    bit [11:0] target = step == 0 ? StWrite : StWriteIntg;
    bit released, was_reset, saw_sleep;
    sba_access_err_e st;
    string line;
    int unsigned t;
    // The state the plug enters at the next edge, so the hold starts before the step's first beat.
    for (t = 0; t < 400_000; t++) begin
      @(negedge cfg.chip_vif.cpu_clk_rst_if.clk);
      if (rd({Otp, ".state_d"}) == target) break;
    end
    `DV_CHECK_LT_FATAL(t, 400_000, $sformatf("the OTP plug never entered %s", name))
    `DV_CHECK_FATAL(uvm_hdl_force(OtpWready, 0), OtpWready)
    `uvm_info(`gfn, $sformatf("EG2 RRAM otp: holding %s", name), UVM_LOW)
    fork
      begin
        wait_sw_line("EG2 RRAM otp release", line, 50_000_000, 1);
        released = 1'b1;
      end
      begin
        // Time-based: the main clock stops if the chip does go down.
        while (!released) begin
          #10ns;
          if (rd(CoreSleeping) == 1) saw_sleep = 1'b1;
          if (rd({Otp, ".rst_ni"}) == 0) begin
            was_reset = 1'b1;
            `DV_CHECK_FATAL(uvm_hdl_release(OtpWready), OtpWready)
            break;
          end
        end
      end
      begin
        // The debug module made active once, at the first hold (the step waits for it).
        if (!dm_active) begin
          cfg.debugger.set_dmactive(1);
          dm_active = 1'b1;
        end
        // The software polls the flag: the model takes the chip's value for it (bench mailbox).
        eg2_iss_wrapper::backdoor_mailbox(flag, 4);
        sba_wr32(flag, step + 1, st);
        `DV_CHECK_EQ(st, SbaErrNone, "SBA write of the software's flag")
      end
    join
    if (was_reset) begin
      `uvm_error(`gfn, $sformatf("rram_ctrl reset while the OTP write was held in %s", name))
      return;
    end
    `DV_CHECK_EQ(rd(OtpState), target,
                 $sformatf("the OTP plug left %s while it was held", name))
    `DV_CHECK(saw_sleep, $sformatf("the core did not sleep while %s was held", name))
    `DV_CHECK_FATAL(uvm_hdl_release(OtpWready), OtpWready)
    `uvm_info(`gfn, $sformatf("EG2 RRAM otp: %s released after the low-power request", name),
              UVM_LOW)
  endtask

  virtual task body();
    string line;
    int unsigned step, n, hits[2];
    int got;
    bit [31:0] flag;
    super.body();
    forever begin
      wait_sw_line("EG2 RRAM otp sleep ", line, 200_000_000, n > 0);
      if (line == "") return;  // the software ended (an _inject_fail build stops at its check)
      if (line == "EG2 RRAM otp sleep done") break;
      got = $sscanf(line, "EG2 RRAM otp sleep step %d flag 0x%h", step, flag);
      `DV_CHECK_EQ_FATAL(got, 2, line)
      `DV_CHECK_LT_FATAL(step, 2, line)
      hold(step, flag);
      hits[step]++;
      n++;
    end
    `DV_CHECK_EQ(n, otp_writes, "OTP writes held")
    if (otp_step == 2) begin
      `DV_CHECK_GT(hits[0], 0, "no request in the data word's write")
      `DV_CHECK_GT(hits[1], 0, "no request in the integrity word's write")
    end
    `uvm_info(`gfn, $sformatf(
              "EG2 RRAM otp: %0d writes held, %0d in the data step, %0d in the integrity step",
              n, hits[0], hits[1]), UVM_LOW)
  endtask

endclass : chip_sw_eg2_rram_idle_low_power_otp_vseq
