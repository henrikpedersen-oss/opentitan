// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Constrained-random traffic for pwrmgr against the power manager reference model
// (pwrmgr_cmodel_sb, +pwrmgr_cmodel=1). Each round is one of:
//   register traffic: random writes of the software fields (INTR_ENABLE / STATE / TEST, CONTROL
//     without the hint, WAKEUP_EN, RESET_EN, WAKE_INFO_CAPTURE_DIS, WAKE_INFO, CFG_CDC_SYNC) and
//     reads of every register, at random times;
//   wake-up levels changing while the device is active;
//   a low-power entry with random CONTROL enables (normal or deep sleep), ended by an enabled
//     wake-up, by an enabled peripheral reset request, by the software reset request or by
//     escalation, or interrupted by a fall through (WFI dropping during the entry) or an abort (an
//     idle input of otp_ctrl, lc_ctrl or the NVM controller low during the entry);
//   a peripheral reset request, the software reset request or escalation while active;
//   reads of every register.
// The REGWENs of WAKEUP_EN and RESET_EN are locked only in the last round (a locked WAKEUP_EN with
// no wake-up enabled could never wake the device). The checking is the model's (+en_scb=0 in the
// random tests); this sequence only checks that the device comes back to Active.
class pwrmgr_cmodel_random_vseq extends pwrmgr_base_vseq;
  `uvm_object_utils(pwrmgr_cmodel_random_vseq)

  rand int unsigned num_rounds;
  constraint num_rounds_c {num_rounds inside {[20:60]};}

  typedef enum int {
    EndWakeup, EndResetReq, EndSwReset, EndEscalation, EndFallThrough, EndAbort
  } lp_end_e;

  `uvm_object_new

  task body();
    wait_for_rom_and_active();
    set_nvms_idle();
    read_all();
    for (int r = 0; r < num_rounds; ++r) begin
      randcase
        3: reg_traffic();
        1: wake_levels();
        6: low_power_round();
        1: active_reset(0);
        1: active_reset(1);
        1: active_reset(2);
        1: read_all();
      endcase
    end
    // The last round: the REGWENs, then a final look at every register.
    if ($urandom_range(0, 1)) csr_wr(.ptr(ral.wakeup_en_regwen), .value(0));
    if ($urandom_range(0, 1)) csr_wr(.ptr(ral.reset_en_regwen), .value(0));
    csr_wr(.ptr(ral.wakeup_en[0]), .value($urandom));
    csr_wr(.ptr(ral.reset_en), .value($urandom));
    cfg.pwrmgr_vif.update_wakeups('0);
    cfg.slow_clk_rst_vif.wait_clks(20);
    read_all();
  endtask

  // Reads every register (at a random time: the model compares what it can).
  protected task read_all();
    uvm_reg csrs[$];
    bit [TL_DW-1:0] v;
    ral.get_registers(csrs);
    csrs.shuffle();
    foreach (csrs[i]) begin
      if (csrs[i].get_name() inside {"intr_test", "alert_test"}) continue;
      csr_rd(.ptr(csrs[i]), .value(v));
      if ($urandom_range(0, 3) == 0) cfg.slow_clk_rst_vif.wait_clks($urandom_range(1, 8));
    end
  endtask

  protected task reg_traffic();
    repeat ($urandom_range(3, 12)) begin
      randcase
        2: csr_wr(.ptr(ral.intr_enable), .value($urandom));
        1: csr_wr(.ptr(ral.intr_state), .value($urandom));
        1: csr_wr(.ptr(ral.intr_test), .value($urandom));
        2: write_control($urandom & ~32'h1);  // never the hint here
        2: csr_wr(.ptr(ral.wakeup_en[0]), .value($urandom));
        2: csr_wr(.ptr(ral.reset_en), .value($urandom));
        1: csr_wr(.ptr(ral.wake_info_capture_dis), .value($urandom));
        1: csr_wr(.ptr(ral.wake_info), .value($urandom));
        2: wait_for_csr_to_propagate_to_slow_domain();
        3: read_one();
        2: cfg.slow_clk_rst_vif.wait_clks($urandom_range(1, 20));
      endcase
    end
  endtask

  protected task read_one();
    uvm_reg csrs[$];
    bit [TL_DW-1:0] v;
    ral.get_registers(csrs);
    csrs.shuffle();
    if (csrs[0].get_name() inside {"intr_test", "alert_test"}) return;
    csr_rd(.ptr(csrs[0]), .value(v));
  endtask

  protected task write_control(bit [31:0] v);
    control_assertions(v[8]);
    csr_wr(.ptr(ral.control), .value(v));
  endtask

  // Wake-up levels while active: WAKE_STATUS, and WAKE_INFO capture if it runs.
  protected task wake_levels();
    repeat ($urandom_range(1, 4)) begin
      cfg.pwrmgr_vif.update_wakeups(wakeups_t'($urandom));
      cfg.slow_clk_rst_vif.wait_clks($urandom_range(1, 30));
      read_one();
    end
    cfg.pwrmgr_vif.update_wakeups('0);
  endtask

  // A reset request while active: 0 peripheral (enabled), 1 software, 2 escalation.
  protected task active_reset(int kind);
    resets_t rst = resets_t'($urandom_range(1, (1 << pwrmgr_reg_pkg::NumRstReqs) - 1));
    case (kind)
      0: begin
        if ($urandom_range(0, 3) != 0) begin
          csr_wr(.ptr(ral.reset_en), .value(rst));
          wait_for_csr_to_propagate_to_slow_domain();
        end
        cfg.pwrmgr_vif.update_resets(rst);
      end
      1: cfg.pwrmgr_vif.update_sw_rst_req(MuBi4True);
      default: send_escalation_reset();
    endcase
    // The fast responder drops the requests once the reset is under way; an ignored peripheral
    // request is dropped here.
    cfg.slow_clk_rst_vif.wait_clks(20);
    cfg.pwrmgr_vif.update_resets('0);
    cfg.pwrmgr_vif.update_sw_rst_req(MuBi4False);
    if (kind == 2) clear_escalation_reset();
    wait_for_rom_and_active();
    cfg.slow_clk_rst_vif.wait_clks($urandom_range(2, 20));
    read_one();
  endtask

  protected task low_power_round();
    lp_end_e how;
    wakeups_t wk, wk_en;
    resets_t rst;
    bit [31:0] ctl;
    randcase
      5: how = EndWakeup;
      2: how = EndResetReq;
      1: how = EndSwReset;
      1: how = EndEscalation;
      2: how = EndFallThrough;
      2: how = EndAbort;
    endcase
    wk_en = wakeups_t'($urandom);
    wk = wakeups_t'($urandom);
    if ((wk & wk_en) == 0) wk_en |= wk;
    if (wk == 0) begin
      wk = 1;
      wk_en |= 1;
    end
    rst = resets_t'($urandom_range(1, (1 << pwrmgr_reg_pkg::NumRstReqs) - 1));

    `uvm_info(`gfn, $sformatf("low-power round: %0s", how.name()), UVM_MEDIUM)
    csr_wr(.ptr(ral.wakeup_en[0]), .value(wk_en));
    if (how == EndResetReq) csr_wr(.ptr(ral.reset_en), .value(rst));
    if ($urandom_range(0, 2) == 0) csr_wr(.ptr(ral.wake_info_capture_dis), .value($urandom));
    if ($urandom_range(0, 2) == 0) csr_wr(.ptr(ral.intr_enable), .value($urandom));
    ctl = $urandom | 32'h1;
    // Escalation needs the escalation clock, which the bench stops with the IO clock.
    if (how == EndEscalation) ctl[5] = 1'b1;
    // An abort or a fall through needs the fast FSM to reach its checks with clocks it can use.
    write_control(ctl);
    wait_for_csr_to_propagate_to_slow_domain();

    case (how)
      EndFallThrough: begin
        cfg.pwrmgr_vif.update_cpu_sleeping(1'b1);
        wait (cfg.pwrmgr_vif.fetch_en != lc_ctrl_pkg::On);
        cfg.clk_rst_vif.wait_clks($urandom_range(0, 2));
        cfg.pwrmgr_vif.update_cpu_sleeping(1'b0);
      end
      EndAbort: begin
        bit [2:0] idle;
        `DV_CHECK_STD_RANDOMIZE_WITH_FATAL(idle, idle != 3'b111;)
        if ($urandom_range(0, 1)) begin
          set_nvms_idle(idle[0], idle[1], idle[2]);
          cfg.pwrmgr_vif.update_cpu_sleeping(1'b1);
        end else begin
          cfg.pwrmgr_vif.update_cpu_sleeping(1'b1);
          wait (cfg.pwrmgr_vif.fetch_en != lc_ctrl_pkg::On);
          set_nvms_idle(idle[0], idle[1], idle[2]);
        end
      end
      default: begin
        cfg.pwrmgr_vif.update_cpu_sleeping(1'b1);
        wait (cfg.pwrmgr_vif.fetch_en != lc_ctrl_pkg::On);
        if (!ctl[8]) wait_for_reset_cause(pwrmgr_pkg::LowPwrEntry);
        cfg.slow_clk_rst_vif.wait_clks($urandom_range(2, 12));
        case (how)
          EndWakeup: cfg.pwrmgr_vif.update_wakeups(wk);
          EndResetReq: cfg.pwrmgr_vif.update_resets(rst);
          EndSwReset: cfg.pwrmgr_vif.update_sw_rst_req(MuBi4True);
          default: send_escalation_reset();
        endcase
      end
    endcase

    come_back(wk);
    `uvm_info(`gfn, "active again", UVM_MEDIUM)
    cfg.slow_clk_rst_vif.wait_clks($urandom_range(0, 4));
    // Status as software would read it, settled or not.
    if ($urandom_range(0, 1)) begin
      csr_rd(.ptr(ral.wake_status[0]), .value(ctl));
      csr_rd(.ptr(ral.wake_info), .value(ctl));
    end
    cfg.pwrmgr_vif.update_wakeups('0);
    cfg.pwrmgr_vif.update_resets('0);
    cfg.pwrmgr_vif.update_sw_rst_req(MuBi4False);
    clear_escalation_reset();
    cfg.pwrmgr_vif.update_cpu_sleeping(1'b0);
    set_nvms_idle();
    cfg.slow_clk_rst_vif.wait_clks($urandom_range(4, 20));
    read_all();
    // Clear the round's record, as software does after a wake.
    if ($urandom_range(0, 3) != 0) begin
      clear_wake_info();
      csr_wr(.ptr(ral.intr_state), .value('1));
      csr_wr(.ptr(ral.wake_info_capture_dis), .value(0));
    end
  endtask
  // Waits for Active; if the device is still in low power after a while (an entry that neither fell
  // through nor aborted, a request it did not take), an enabled wake-up brings it back. The wait is
  // the sequence's own (1 ms), not wait_for_rom_and_active's 40 us: an escalation sent in low
  // power is taken only once the escalation clock runs again (the bench stops it with the IO
  // clock's second-level gating), so the wake-up, the power-up and the escalation reset follow one
  // another (2026-11-07, pwrmgr_cmodel_random seed 4). The ROM checks stay passed.
  protected task come_back(wakeups_t wk);
    fork
      begin : isolation_fork
        fork
          begin
            cfg.slow_clk_rst_vif.wait_clks(150);
            if (!cpu_is_enabled()) begin
              `uvm_info(`gfn, "still in low power: an enabled wake-up", UVM_MEDIUM)
              cfg.pwrmgr_vif.update_wakeups(wk);
            end
          end
        join_none
        `DV_SPINWAIT(wait (cfg.pwrmgr_vif.fetch_en == lc_ctrl_pkg::On);,
                     "pwrmgr did not return to Active", 1_000_000)
        disable fork;
      end
    join
  endtask
endclass : pwrmgr_cmodel_random_vseq
