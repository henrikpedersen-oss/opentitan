// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// clkmgr against the clock manager reference model (hw/top_earlgrey/dv/eg2_model/ip/
// eg2_clkmgr_core.h, through eg2_clkmgr_cmodel_pkg). On with +clkmgr_cmodel=1 (cfg.en_cmodel),
// next to clkmgr_scoreboard.
//
// The model sees only the block's ports, the bench's source clocks and the TL-UL accesses, never a
// signal inside clkmgr:
//   the inputs as levels: idle_i, scanmode_i, lc_hw_debug_en_i, lc_clk_byp_req_i,
//     io_clk_byp_ack_i, all_clk_byp_ack_i, div_step_down_req_i, calib_rdy_i, pwr_i's clock enables,
//     and whether each source clock (io, main, usb) toggles;
//   per always-on clock period, the rising edges of each source clock (the measurements);
//   every TL-UL write as it completes, every read's data.
// Compared: every register read against the model's bounds (the bits that must read 1, the bits
// that may read 1), and each always-on period while the design is settled the outputs:
// all_clk_byp_req_o, io_clk_byp_req_o, lc_clk_byp_ack_o, jitter_en_o, pwr_o's clock status,
// whether each software-gated and transactional clock of clocks_o toggled in the period, and the
// io dividers' ratio (clk_io_div4_powerup and clk_io_div2_powerup edges against clk_io's, over at
// least 8 periods). The documents give no cycle timing for the clock-domain crossings, so the
// design counts as settled ("quiet") only after settle_aon always-on periods with no input, no
// source clock and no register write changing; until then the fields hardware updates and the
// outputs are not compared.
class clkmgr_cmodel_sb extends uvm_component;
  `uvm_component_utils(clkmgr_cmodel_sb)

  clkmgr_env_cfg cfg;
  int unsigned   settle_aon = 12;  // +clkmgr_cmodel_settle: always-on periods without a change
  // +clkmgr_cmodel_corrupt=1: the first read the model predicts exactly is expected with bit 0
  // inverted, so the run must fail.
  bit            corrupt;
  // +clkmgr_cmodel_mutate=N: a mutated model (eg2_clkmgr_core.h), so the run must fail.
  int            mutate;
  int unsigned   hist_len = 24;    // +clkmgr_cmodel_history: model inputs kept for a mismatch report
  // +clkmgr_cmodel_mask_known=1: the known RTL finding is counted, not failed: with EXTCLK_CTRL.SEL
  // holding a value other than kMultiBitBool4True/False ("Writing any other value has no impact"),
  // all_clk_byp_req_o carries that value instead of keeping the selection (status/rtl_tickets.md,
  // clkmgr: "EXTCLK_CTRL.SEL: another value reaches all_clk_byp_req_o").
  bit            mask_known;

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_a_fifo;  // requests: a write in flight holds the checks
  uvm_tlm_analysis_fifo #(tl_seq_item) tl_d_fifo;

  clkmgr_cmodel_cov cov;  // with coverage enabled (cfg.en_cov)

  protected chandle      h;
  protected int unsigned quiet_cnt;
  protected bit          changed;
  protected bit [95:0]   in_last;
  protected bit          sampled;
  protected int          tl_outstanding;

  // Edges in the current always-on period: the sources, the gated clocks, the dividers.
  protected int unsigned cnt_src[3];   // io, main, usb
  protected int unsigned prev_src[3];
  protected bit [2:0]    run_last;
  protected int unsigned cnt_gated[9];  // peri io_div4, io_div2, io, usb, spi_host1; trans x4
  protected int unsigned acc_io, acc_div4, acc_div2, acc_periods;

  // Counts for the report.
  protected int unsigned n_rd_cmp, n_rd_exact, n_rd_open, n_out_cmp, n_out_bits, n_mismatch;
  protected int unsigned n_writes, n_inputs, n_settled, n_div_cmp, n_known_sel;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_a_fifo = new("tl_a_fifo", this);
    tl_d_fifo = new("tl_d_fifo", this);
    if (cfg.en_cov) cov = clkmgr_cmodel_cov::type_id::create("clkmgr_cmodel_cov");
    void'($value$plusargs("clkmgr_cmodel_settle=%d", settle_aon));
    void'($value$plusargs("clkmgr_cmodel_corrupt=%b", corrupt));
    void'($value$plusargs("clkmgr_cmodel_mutate=%d", mutate));
    void'($value$plusargs("clkmgr_cmodel_history=%d", hist_len));
    void'($value$plusargs("clkmgr_cmodel_mask_known=%b", mask_known));
    h = eg2_clkmgr_cmodel_pkg::eg2_clk_create(mutate);
  endfunction

  task run_phase(uvm_phase phase);
    fork
      watch_reset();
      forever begin
        @(posedge cfg.clk_rst_vif.clk);
        sample();
      end
      forever begin
        @(posedge cfg.aon_clk_rst_vif.clk);
        aon_tick();
      end
      forever begin @(posedge cfg.io_clk_rst_vif.clk); cnt_src[0]++; acc_io++; end
      forever begin @(posedge cfg.main_clk_rst_vif.clk); cnt_src[1]++; end
      forever begin @(posedge cfg.usb_clk_rst_vif.clk); cnt_src[2]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_div4_powerup); acc_div4++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_div2_powerup); acc_div2++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_div4_peri); cnt_gated[0]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_div2_peri); cnt_gated[1]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_peri); cnt_gated[2]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_usb_peri); cnt_gated[3]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_io_spi_host1); cnt_gated[4]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_main_aes); cnt_gated[5]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_main_hmac); cnt_gated[6]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_main_kmac); cnt_gated[7]++; end
      forever begin @(posedge cfg.clkmgr_vif.clocks_o.clk_main_otbn); cnt_gated[8]++; end
      forever process_tl();
      forever process_tl_a();
    join_none
  endtask

  protected task watch_reset();
    forever begin
      @(negedge cfg.clk_rst_vif.rst_n);
      eg2_clkmgr_cmodel_pkg::eg2_clk_reset(h);
      tl_a_fifo.flush();
      tl_d_fifo.flush();
      tl_outstanding = 0;
      sampled = 1'b0;
      quiet_cnt = 0;
      changed = 1'b1;
      note("block reset");
      if (cov != null) cov.reset();
    end
  endtask

  protected function bit quiet();
    return quiet_cnt >= settle_aon;
  endfunction

  // ---- Ports ----

  protected function void sample();
    bit [95:0] in_now;
    bit [15:0] idle;
    bit [2:0]  en;
    if (cfg.clk_rst_vif.rst_n !== 1'b1) return;
    idle = cfg.clkmgr_vif.idle_i;
    en = {cfg.clkmgr_vif.pwr_i.usb_ip_clk_en === 1'b1, cfg.clkmgr_vif.pwr_i.io_ip_clk_en === 1'b1,
          cfg.clkmgr_vif.pwr_i.main_ip_clk_en === 1'b1};
    // Layout (clkmgr_cmodel_cov::sample_inputs decodes it): idle 15:0, scanmode 19:16,
    // lc_debug_en 23:20, lc_byp_req 27:24, io_byp_ack 31:28, all_byp_ack 35:32, step_down 39:36,
    // calib 43:40, en 46:44 (main, io, usb), run 49:47.
    in_now = 96'({run_last, en, 4'(cfg.clkmgr_vif.calib_rdy), 4'(cfg.clkmgr_vif.div_step_down_req),
                  4'(cfg.clkmgr_vif.all_clk_byp_ack), 4'(cfg.clkmgr_vif.io_clk_byp_ack),
                  4'(cfg.clkmgr_vif.lc_clk_byp_req), 4'(cfg.clkmgr_vif.lc_hw_debug_en_i),
                  4'(cfg.clkmgr_vif.scanmode_i), idle});
    if (!sampled || in_now !== in_last) give_inputs(in_now);
  endfunction

  protected function void give_inputs(bit [95:0] in_now);
    changed = 1'b1;
    n_inputs++;
    if (cov != null && sampled) cov.sample_inputs(in_now);
    note($sformatf({"inputs idle %04h scanmode %h lc_debug_en %h lc_byp_req %h io_byp_ack %h ",
                    "all_byp_ack %h step_down %h calib %h en %03b run %03b"},
                   in_now[15:0], in_now[19:16], in_now[23:20], in_now[27:24], in_now[31:28],
                   in_now[35:32], in_now[39:36], in_now[43:40], in_now[46:44], in_now[49:47]));
    eg2_clkmgr_cmodel_pkg::eg2_clk_inputs(h, in_now[15:0], in_now[19:16], in_now[23:20],
                                          in_now[27:24], in_now[31:28], in_now[35:32],
                                          in_now[39:36], in_now[43:40], in_now[46:44],
                                          in_now[49:47]);
    in_last = in_now;
    sampled = 1'b1;
  endfunction

  protected function void aon_tick();
    bit [2:0] run, steady;
    int unsigned c[3];
    // The inputs as they are now: a change since the last clk_i edge (scan mode opens the gates at
    // once) must keep this period from being checked (2026-11-07, seed 3: scanmode_i rose 4 ns
    // before the always-on edge).
    sample();
    c = cnt_src;
    cnt_src = '{default: 0};
    if (cfg.clk_rst_vif.rst_n !== 1'b1 || cfg.aon_clk_rst_vif.rst_n !== 1'b1) begin
      prev_src = c;
      cnt_gated = '{default: 0};
      acc_io = 0; acc_div4 = 0; acc_div2 = 0; acc_periods = 0;
      return;
    end
    for (int i = 0; i < 3; i++) begin
      int d = int'(c[i]) - int'(prev_src[i]);
      run[i] = c[i] > 0;
      steady[i] = c[i] > 0 && d >= -1 && d <= 1;
      // A source clock that changes frequency is a stimulus: the design settles again.
      if (!steady[i] && (c[i] > 0 || prev_src[i] > 0)) changed = 1'b1;
    end
    prev_src = c;
    // The model's source clock order is main, io, usb for the run levels.
    if ({run[2], run[0], run[1]} != run_last) begin
      run_last = {run[2], run[0], run[1]};
      if (sampled) give_inputs({in_last[95:50], run_last, in_last[46:0]});
    end
    eg2_clkmgr_cmodel_pkg::eg2_clk_aon_period(h, c[0], c[1], c[2], 32'(steady));
    if (changed) begin
      quiet_cnt = 0;
      changed = 1'b0;
      cnt_gated = '{default: 0};
      acc_io = 0; acc_div4 = 0; acc_div2 = 0; acc_periods = 0;
      return;
    end
    if (quiet_cnt < settle_aon) begin
      quiet_cnt++;
      if (quiet_cnt == settle_aon) begin
        eg2_clkmgr_cmodel_pkg::eg2_clk_settled(h);
        n_settled++;
      end
    end
    // Not while a request is in flight: clkmgr acts on a write at its request, the model at the
    // response.
    if (quiet() && tl_outstanding == 0) check_outputs();
    cnt_gated = '{default: 0};
  endfunction

  protected function void check_outputs();
    int unsigned lo, hi, obs, mask, bad;
    bit div_ok;
    obs = 0;
    obs[ClkOAllBypTrue]   = cfg.clkmgr_vif.all_clk_byp_req == prim_mubi_pkg::MuBi4True;
    obs[ClkOAllBypFalse]  = cfg.clkmgr_vif.all_clk_byp_req == prim_mubi_pkg::MuBi4False;
    obs[ClkOIoBypTrue]    = cfg.clkmgr_vif.io_clk_byp_req == prim_mubi_pkg::MuBi4True;
    obs[ClkOIoBypFalse]   = cfg.clkmgr_vif.io_clk_byp_req == prim_mubi_pkg::MuBi4False;
    obs[ClkOLcAckOn]      = cfg.clkmgr_vif.lc_clk_byp_ack == lc_ctrl_pkg::On;
    obs[ClkOLcAckOff]     = cfg.clkmgr_vif.lc_clk_byp_ack == lc_ctrl_pkg::Off;
    // jitter_en_o read as JITTER_ENABLE.VAL is described: "kMultiBitBool4False disables the jittery
    // clock, while all other values enable" (the value it carries to ast is not documented).
    obs[ClkOJitterTrue]   = cfg.clkmgr_vif.jitter_en_o != prim_mubi_pkg::MuBi4False;
    obs[ClkOJitterFalse]  = cfg.clkmgr_vif.jitter_en_o == prim_mubi_pkg::MuBi4False;
    obs[ClkOHiSpeedTrue]  = cfg.clkmgr_vif.hi_speed_sel == prim_mubi_pkg::MuBi4True;
    obs[ClkOHiSpeedFalse] = cfg.clkmgr_vif.hi_speed_sel == prim_mubi_pkg::MuBi4False;
    obs[ClkOPwrMain]      = cfg.clkmgr_vif.pwr_o.main_status === 1'b1;
    obs[ClkOPwrIo]        = cfg.clkmgr_vif.pwr_o.io_status === 1'b1;
    obs[ClkOPwrUsb]       = cfg.clkmgr_vif.pwr_o.usb_status === 1'b1;
    for (int i = 0; i < 9; i++) obs[ClkOPeriIoDiv4 + i] = cnt_gated[i] > 0;
    // The dividers, over at least 8 periods and 40 io edges: div4 x4 and div2 x2 equal io
    // (normal), or div4 x2 and div2 equal io (stepped down), each within an eighth.
    acc_periods++;
    div_ok = acc_periods >= 8 && acc_io >= 40;
    if (div_ok) begin
      obs[ClkODivNormal]  = near(acc_div4 * 4, acc_io) && near(acc_div2 * 2, acc_io);
      obs[ClkODivStepped] = near(acc_div4 * 2, acc_io) && near(acc_div2, acc_io);
      n_div_cmp++;
    end
    mask = (1 << ClkONumOutputs) - 1;
    if (!div_ok) mask &= ~((1 << ClkODivNormal) | (1 << ClkODivStepped));
    eg2_clkmgr_cmodel_pkg::eg2_clk_outputs(h, 1, lo, hi);
    n_out_cmp++;
    n_out_bits += $countones(mask & ~(hi & ~lo));
    if (cov != null) cov.sample_outputs(lo, hi, obs, mask);
    bad = ((obs & ~hi) | (lo & ~obs)) & mask;
    if (mask_known && bad != 0 && eg2_clkmgr_cmodel_pkg::eg2_clk_sel_other(h) &&
        (bad & ~((1 << ClkOAllBypTrue) | (1 << ClkOAllBypFalse))) == 0) begin
      if (n_known_sel == 0) begin
        `uvm_info(`gfn, $sformatf({"known finding (counted, not failed): EXTCLK_CTRL.SEL holds a ",
                                   "value other than true/false, all_clk_byp_req_o is 0x%0h"},
                                  cfg.clkmgr_vif.all_clk_byp_req), UVM_LOW)
      end
      n_known_sel++;
      bad = 0;
    end
    if (bad != 0) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf(
                 "outputs: clkmgr %06h, model must %06h may %06h (differing: %0s)%0s",
                 obs & mask, lo & mask, hi & mask, out_names(bad),
                 n_mismatch <= 5 ? history() : ""))
    end
    if (div_ok) begin
      acc_io = 0; acc_div4 = 0; acc_div2 = 0; acc_periods = 0;
    end
  endfunction

  protected static function bit near(int unsigned a, int unsigned b);
    int d = int'(a) - int'(b);
    return (d < 0 ? -d : d) <= int'(b / 8) + 2;
  endfunction

  protected static function string out_names(int unsigned bits);
    string s;
    for (int i = 0; i < ClkONumOutputs; i++) begin
      if (bits[i]) begin
        clk_out_e e = clk_out_e'(i);
        s = {s, s == "" ? "" : " ", e.name()};
      end
    end
    return s;
  endfunction

  // ---- Registers ----

  protected task process_tl();
    tl_seq_item  it;
    int unsigned addr, lo, hi, d;
    bit          q;
    tl_d_fifo.get(it);
    if (tl_outstanding > 0) tl_outstanding--;
    if (cfg.clk_rst_vif.rst_n !== 1'b1 || it.d_error) return;
    addr = it.a_addr & 'h7c;  // the TL-UL agent sets random low address bits on word reads
    q = quiet();
    if (it.is_write()) begin
      n_writes++;
      if (cov != null) cov.sample_write(addr, it.a_data, read_now('h04), read_now('h10),
                                        read_now('h24), eg2_clkmgr_cmodel_pkg::eg2_clk_sel(h));
      eg2_clkmgr_cmodel_pkg::eg2_clk_csr_write(h, addr, it.a_data, q);
      note($sformatf("write 0x%02h = 0x%08h%0s", addr, it.a_data, q ? "" : " (settling)"));
      changed = 1'b1;
      return;
    end
    d = it.d_data;
    eg2_clkmgr_cmodel_pkg::eg2_clk_csr_read(h, addr, q, lo, hi);
    note($sformatf("read 0x%02h: clkmgr 0x%08h, model must 0x%08h may 0x%08h%0s", addr, d, lo, hi,
                   q ? "" : " (settling)"));
    if (cov != null) cov.sample_read(addr, d, lo, hi);
    n_rd_cmp++;
    if (lo == hi) n_rd_exact++;
    else n_rd_open++;
    if (corrupt && lo == hi) begin
      lo ^= 1;
      hi ^= 1;
      corrupt = 1'b0;
    end
    if ((d & ~hi) != 0 || (lo & ~d) != 0) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf("read 0x%02h (%0s): clkmgr 0x%08h, model must 0x%08h may 0x%08h%0s",
                                 addr, reg_name(addr), d, lo, hi, n_mismatch <= 5 ? history() : ""))
    end
  endtask

  protected task process_tl_a();
    tl_seq_item it;
    tl_a_fifo.get(it);
    tl_outstanding++;
    if (it.is_write()) changed = 1'b1;
  endtask

  // The model's value of a register now (coverage context; a read of a shadowed register would
  // change its phase, so only the plain registers are asked).
  protected function int unsigned read_now(int unsigned addr);
    int unsigned lo, hi;
    eg2_clkmgr_cmodel_pkg::eg2_clk_csr_read(h, addr, 1, lo, hi);
    return lo;
  endfunction

  protected static function string reg_name(int unsigned addr);
    case (addr)
      'h00: return "ALERT_TEST";
      'h04: return "EXTCLK_CTRL_REGWEN";
      'h08: return "EXTCLK_CTRL";
      'h0c: return "EXTCLK_STATUS";
      'h10: return "JITTER_REGWEN";
      'h14: return "JITTER_ENABLE";
      'h18: return "CLK_ENABLES";
      'h1c: return "CLK_HINTS";
      'h20: return "CLK_HINTS_STATUS";
      'h24: return "MEASURE_CTRL_REGWEN";
      'h28: return "IO_MEAS_CTRL_EN";
      'h2c: return "IO_MEAS_CTRL_SHADOWED";
      'h30: return "IO_DIV4_MEAS_CTRL_EN";
      'h34: return "IO_DIV4_MEAS_CTRL_SHADOWED";
      'h38: return "MAIN_MEAS_CTRL_EN";
      'h3c: return "MAIN_MEAS_CTRL_SHADOWED";
      'h40: return "USB_MEAS_CTRL_EN";
      'h44: return "USB_MEAS_CTRL_SHADOWED";
      'h48: return "RECOV_ERR_CODE";
      'h4c: return "FATAL_ERR_CODE";
      default: return "?";
    endcase
  endfunction

  protected string hist[$];
  protected function void note(string s);
    hist.push_back($sformatf("%t %0s", $time, s));
    if (hist_len == 0) `uvm_info(`gfn, $sformatf("model input: %0s", s), UVM_LOW)
    if (hist.size() > (hist_len == 0 ? 24 : hist_len)) void'(hist.pop_front());
  endfunction
  protected function string history();
    string s = "\n  model inputs before it (oldest first):";
    foreach (hist[i]) s = {s, "\n    ", hist[i]};
    return s;
  endfunction

  function void check_phase(uvm_phase phase);
    super.check_phase(phase);
    // Some of OpenTitan's sequences read no register (clkmgr_peri checks the clocks only) and some
    // never let the design settle (clkmgr_frequency moves the clocks all the time): either check is
    // enough, none is a failure.
    if (n_rd_cmp == 0 && n_out_cmp == 0) begin
      `uvm_error(`gfn, "the model compared nothing: no register read, and the design never settled")
    end
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"clkmgr cmodel: compared reads %0d (exact %0d, with open bits %0d), ",
                               "outputs %0d times (%0d predicted bits, divider ratio %0d times); ",
                               "mismatches %0d; writes %0d, input changes %0d, settled %0d times; ",
                               "measurement periods %0d (errors predicted %0d, in range %0d); ",
                               "shadow commits %0d, update errors %0d; known finding (SEL value) %0d"},
                              n_rd_cmp, n_rd_exact, n_rd_open, n_out_cmp, n_out_bits, n_div_cmp,
                              n_mismatch, n_writes, n_inputs, n_settled,
                              eg2_clkmgr_cmodel_pkg::eg2_clk_stat(h, 4),
                              eg2_clkmgr_cmodel_pkg::eg2_clk_stat(h, 0),
                              eg2_clkmgr_cmodel_pkg::eg2_clk_stat(h, 1),
                              eg2_clkmgr_cmodel_pkg::eg2_clk_stat(h, 3),
                              eg2_clkmgr_cmodel_pkg::eg2_clk_stat(h, 2), n_known_sel), UVM_NONE)
    if (h != null) eg2_clkmgr_cmodel_pkg::eg2_clk_release(h);
    h = null;
  endfunction
endclass
