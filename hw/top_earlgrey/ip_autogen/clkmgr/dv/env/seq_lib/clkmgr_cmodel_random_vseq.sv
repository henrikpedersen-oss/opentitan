// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Random stimulus for the clock manager reference model's check (clkmgr_cmodel_sb, run with
// +clkmgr_cmodel=1 +en_scb=0: the model is the only checker). In random order and with random
// gaps: register writes (the hints and enables, the external clock selection with every kind of
// mubi value, jitter, the measurement enables and thresholds with matched, single and mismatched
// shadowed writes, error clears, now and then a write-enable lock), reads of every register, and
// input changes: the idle indications, life-cycle debug enable, the life-cycle clock bypass
// request, ast's divider step-down request, calibration, the power manager's clock enables (with
// the bench's source clocks stopped as the power manager would), the source clock frequencies,
// and briefly scan mode. A small ast responder answers all_clk_byp_req_o and io_clk_byp_req_o.
// The always-on clock runs at 1 MHz so a measurement period holds about 100 io cycles.
class clkmgr_cmodel_random_vseq extends clkmgr_base_vseq;
  `uvm_object_utils(clkmgr_cmodel_random_vseq)
  `uvm_object_new

  rand int unsigned num_ops;
  constraint num_ops_c {num_ops inside {[80 : 200]};}

  protected bit     ast_on;
  protected real    f_io = 96.0, f_main = 100.0, f_usb = 48.0, f_aon = 1.0;  // MHz
  protected bit     locked_meas;

  // The always-on clock at 1 MHz (see above), the sources at their nominal frequencies.
  virtual task clkmgr_init();
    super.clkmgr_init();
    cfg.aon_clk_rst_vif.set_freq_mhz(f_aon);
  endtask

  task body();
    // clkmgr_sec_cm_checker_assert's AllClkBypReq*_A expect all_clk_byp_req_o one cycle after
    // lc_hw_debug_en_i, which passes a life-cycle synchronizer: they hold only when the debug enable
    // is set before EXTCLK_CTRL.SEL, as OpenTitan's sequences do. This sequence changes it at any
    // time; the tolerant clkmgr_extclk_sva_if checks and the model check that output instead.
    $assertoff(0, "tb.dut.clkmgr_sec_cm_checker_assert.AllClkBypReqTrue_A");
    $assertoff(0, "tb.dut.clkmgr_sec_cm_checker_assert.AllClkBypReqFalse_A");
    ast_on = 1'b1;
    fork ast_responder(); join_none
    cfg.aon_clk_rst_vif.wait_clks(20);
    read_all();
    for (int i = 0; i < num_ops; i++) begin
      randcase
        20: rand_write();
        12: rand_read();
        8: change_idle();
        4: change_debug();
        4: change_lc_byp();
        3: change_step_down();
        3: change_calib();
        3: change_power();
        4: change_freq();
        1: change_scan();
        10: settle_and_read();
      endcase
      cfg.clk_rst_vif.wait_clks($urandom_range(1, 40));
    end
    // Back to the documented quiet state, then everything read once more. Calibration is dropped
    // for a few always-on periods first: that clears every measurement enable (even behind a
    // cleared MEASURE_CTRL_REGWEN), so no measurement error raises recov_fault after the end
    // (cip_base_vseq's check that no alert fires after the sequence).
    cfg.clkmgr_vif.update_scanmode(MuBi4False);
    cfg.clkmgr_vif.update_lc_clk_byp_req(Off);
    cfg.clkmgr_vif.update_div_step_down_req(MuBi4False);
    io_ip_clk_en = 1'b1;
    main_ip_clk_en = 1'b1;
    usb_ip_clk_en = 1'b1;
    control_ip_clocks();
    set_freqs(96.0, 100.0, 48.0);
    cfg.clkmgr_vif.update_calib_rdy(MuBi4False);
    cfg.aon_clk_rst_vif.wait_clks(8);
    cfg.clkmgr_vif.update_calib_rdy(MuBi4True);
    settle_and_read();
    ast_on = 1'b0;
  endtask

  // ---- ast ----

  protected task ast_responder();
    fork
      forever begin
        @(cfg.clkmgr_vif.all_clk_byp_req);
        if (!ast_on) continue;
        cfg.clk_rst_vif.wait_clks($urandom_range(2, 12));
        cfg.clkmgr_vif.update_all_clk_byp_ack(
            cfg.clkmgr_vif.all_clk_byp_req == MuBi4True ? MuBi4True : MuBi4False);
      end
      forever begin
        @(cfg.clkmgr_vif.io_clk_byp_req);
        if (!ast_on) continue;
        cfg.clk_rst_vif.wait_clks($urandom_range(2, 12));
        cfg.clkmgr_vif.update_io_clk_byp_ack(
            cfg.clkmgr_vif.io_clk_byp_req == MuBi4True ? MuBi4True : MuBi4False);
      end
    join_none
  endtask

  // ---- Registers ----

  protected function bit all_on();
    return cfg.clkmgr_vif.pwr_i.io_ip_clk_en && cfg.clkmgr_vif.pwr_i.main_ip_clk_en &&
           cfg.clkmgr_vif.pwr_i.usb_ip_clk_en;
  endfunction

  // A register of a clock domain whose source is stopped cannot be reached (its CDC never answers).
  protected function bit reachable(int unsigned off);
    if (off inside {'h28, 'h2c, 'h30, 'h34}) return cfg.clkmgr_vif.pwr_i.io_ip_clk_en;
    if (off inside {'h38, 'h3c}) return cfg.clkmgr_vif.pwr_i.main_ip_clk_en;
    if (off inside {'h40, 'h44}) return cfg.clkmgr_vif.pwr_i.usb_ip_clk_en;
    return 1'b1;
  endfunction

  protected task wr(int unsigned off, int unsigned data);
    bit [TL_DW-1:0] d = data;
    if (!reachable(off)) return;
    tl_access(.addr(ral.get_addr_from_offset(off)), .write(1'b1), .data(d));
  endtask

  protected task rd(int unsigned off);
    bit [TL_DW-1:0] d;
    if (!reachable(off)) return;
    tl_access(.addr(ral.get_addr_from_offset(off)), .write(1'b0), .data(d));
  endtask

  protected task read_all();
    for (int unsigned off = 0; off <= 'h4c; off += 4) rd(off);
  endtask

  protected task settle_and_read();
    cfg.aon_clk_rst_vif.wait_clks($urandom_range(16, 40));
    read_all();
  endtask

  protected function mubi4_t rand_mubi(int t = 4, int f = 4, int o = 1);
    return get_rand_mubi4_val(t, f, o);
  endfunction

  protected function lc_tx_t rand_lc(int t = 4, int f = 4, int o = 1);
    return get_rand_lc_tx_val(t, f, o);
  endfunction

  protected task rand_read();
    rd($urandom_range(0, 'h13) * 4);
  endtask

  protected task rand_write();
    randcase
      4: wr('h1c, $urandom_range(0, 'hf));                      // CLK_HINTS
      3: wr('h18, $urandom_range(0, 'h1f));                     // CLK_ENABLES
      3: wr('h08, {rand_mubi(3, 3, 2), rand_mubi(3, 3, 2)});    // EXTCLK_CTRL
      2: wr('h14, rand_mubi(3, 3, 2));                          // JITTER_ENABLE
      4: meas_write();
      2: wr('h48, $urandom_range(0, 1) ? 'h1ff : $urandom_range(0, 'h1ff));  // RECOV_ERR_CODE
      1: lock_one();
    endcase
  endtask

  // Late in a run, now and then, one of the write enables cleared.
  protected task lock_one();
    if ($urandom_range(0, 3) != 0) return;
    randcase
      1: wr('h04, 0);
      1: wr('h10, 0);
      1: begin wr('h24, 0); locked_meas = 1'b1; end
    endcase
  endtask

  // A measurement: enable with a mubi value, or thresholds around the count the always-on period
  // holds now (in range, too fast, too slow, close), written twice (commit), once (then read: back
  // to the first phase), or twice with different data (update error).
  protected task meas_write();
    int unsigned m = $urandom_range(0, 3);
    int unsigned en_off = 'h28 + 8 * m, ctrl_off = en_off + 4;
    real f = m == 0 ? f_io : m == 1 ? f_io / 4.0 : m == 2 ? f_main : f_usb;
    int exp_cnt = int'(f / f_aon);
    int lo, hi, hi_bits, lo_shift;
    int unsigned v;
    hi_bits = m == 1 ? 8 : m == 3 ? 9 : 10;
    lo_shift = hi_bits;
    if ($urandom_range(0, 1)) begin
      wr(en_off, rand_mubi(5, 4, 1));
      return;
    end
    randcase
      4: begin lo = exp_cnt - 8; hi = exp_cnt + 8; end
      2: begin lo = exp_cnt - 20; hi = exp_cnt - 8; end
      2: begin lo = exp_cnt + 8; hi = exp_cnt + 20; end
      1: begin lo = exp_cnt - 1; hi = exp_cnt + 1; end
    endcase
    if (lo < 0) lo = 0;
    if (hi < 0) hi = 0;
    v = ((lo & ((1 << hi_bits) - 1)) << lo_shift) | (hi & ((1 << hi_bits) - 1));
    randcase
      8: begin wr(ctrl_off, v); wr(ctrl_off, v); end
      1: begin wr(ctrl_off, v); rd(ctrl_off); end
      1: begin wr(ctrl_off, v); wr(ctrl_off, v ^ (1 << $urandom_range(0, 2 * hi_bits - 1))); end
    endcase
  endtask

  // ---- Inputs ----

  protected task change_idle();
    mubi_hintables_t idle_now = cfg.clkmgr_vif.idle_i;
    idle_now[$urandom_range(0, NUM_TRANS - 1)] = rand_mubi(5, 5, 1);
    cfg.clkmgr_vif.update_idle(idle_now);
  endtask

  protected task change_debug();
    cfg.clkmgr_vif.update_lc_debug_en(rand_lc(5, 4, 1));
  endtask

  protected task change_lc_byp();
    if (!all_on()) return;
    cfg.clkmgr_vif.update_lc_clk_byp_req(rand_lc(4, 5, 1));
  endtask

  protected task change_step_down();
    cfg.clkmgr_vif.update_div_step_down_req(rand_mubi(3, 6, 1));
  endtask

  // calib_rdy_i is a level from ast: changed only with every source clock running (a measurement
  // enable in a stopped domain cannot be cleared) and held for at least 60 clk_i cycles
  // (clkmgr_lost_calib_ctrl_en_sva_if: the enables clear within 45 cycles of losing calibration).
  protected task change_calib();
    if (!all_on()) return;
    cfg.clkmgr_vif.update_calib_rdy(rand_mubi(7, 3, 1));
    cfg.clk_rst_vif.wait_clks(60);
  endtask

  protected task change_scan();
    cfg.clkmgr_vif.update_scanmode(MuBi4True);
    cfg.aon_clk_rst_vif.wait_clks($urandom_range(2, 20));
    cfg.clkmgr_vif.update_scanmode(MuBi4False);
  endtask

  // The power manager's clock enables, the bench's source clocks stopped and started with them.
  protected task change_power();
    if (cfg.clkmgr_vif.lc_clk_byp_req != Off || cfg.clkmgr_vif.all_clk_byp_req != MuBi4False ||
        cfg.clkmgr_vif.calib_rdy != MuBi4True)
      return;
    io_ip_clk_en = $urandom_range(0, 3) != 0;
    main_ip_clk_en = $urandom_range(0, 3) != 0;
    usb_ip_clk_en = $urandom_range(0, 3) != 0;
    control_ip_clocks();
  endtask

  protected task set_freqs(real io, real main, real usb);
    f_io = io;
    f_main = main;
    f_usb = usb;
    cfg.io_clk_rst_vif.set_freq_mhz(io);
    cfg.main_clk_rst_vif.set_freq_mhz(main);
    cfg.usb_clk_rst_vif.set_freq_mhz(usb);
  endtask

  // The source clocks off their nominal frequency by up to 15%, or back.
  protected task change_freq();
    real k = 1.0 + (int'($urandom_range(0, 30)) - 15) / 100.0;
    if ($urandom_range(0, 2) == 0) k = 1.0;
    case ($urandom_range(0, 2))
      0: set_freqs(96.0 * k, f_main, f_usb);
      1: set_freqs(f_io, 100.0 * k, f_usb);
      default: set_freqs(f_io, f_main, 48.0 * k);
    endcase
  endtask
endclass
