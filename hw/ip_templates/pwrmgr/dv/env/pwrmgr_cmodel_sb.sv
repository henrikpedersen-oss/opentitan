// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// pwrmgr against the power manager reference model (hw/top_earlgrey/dv/eg2_model/ip/
// eg2_pwrmgr_core.h, through eg2_pwrmgr_cmodel_pkg). On with +pwrmgr_cmodel=1 (cfg.en_cmodel), next
// to pwrmgr_scoreboard.
//
// The model sees only the block's ports and its TL-UL accesses, never a signal inside pwrmgr:
//   the inputs as levels: wakeups_i, rstreqs_i, sw_rst_req_i (MuBi4True), the escalation request
//     (esc_rst_tx_i held at esc_p 1 / esc_n 0), pwr_cpu_i.core_sleeping, the idle indications of
//     otp_ctrl, lc_ctrl and the NVM controller, pwr_ast_i.main_pok;
//   two of the documented handshakes: fetch_en_o (the fast FSM in Active) and pwr_ast_o.main_pd_n
//     (the power-ok checks apply while main power is requested on);
//   every TL-UL write as it completes, every read's data.
// A read is compared against the model's bounds (eg2_pwrmgr_dpi.cc: the bits that must read 1, the
// bits that may read 1). The documents give no cycle timing for the clock-domain crossings and the
// FSM steps, so the design counts as settled ("quiet") only when no input, no handshake (pwr_ast,
// pwr_clk, pwr_rst, pwr_otp, pwr_lc requests and responses, fetch_en, low_power, rom_ctrl) and no
// register write has changed for settle_slow slow-clock cycles; until then the fields hardware
// updates are not compared. The wakeup interrupt output is compared each slow-clock cycle while
// quiet.
class pwrmgr_cmodel_sb extends uvm_component;
  `uvm_component_utils(pwrmgr_cmodel_sb)

  pwrmgr_env_cfg cfg;
  int unsigned   settle_slow = 12;  // +pwrmgr_cmodel_settle: slow-clock cycles without a change
  // +pwrmgr_cmodel_corrupt=1: the first read the model predicts exactly is expected with bit 0
  // inverted, so the run must fail.
  bit            corrupt;
  int unsigned   hist_len = 24;     // +pwrmgr_cmodel_history: model inputs kept for a mismatch report

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_a_fifo;  // requests: a write in flight holds the checks
  uvm_tlm_analysis_fifo #(tl_seq_item) tl_d_fifo;

  // Field and transition coverage (pwrmgr_reg_cov), with coverage enabled (cfg.en_cov).
  pwrmgr_reg_cov cov;

  protected chandle      h;
  protected int unsigned quiet_cnt;        // slow-clock cycles since the last change
  protected bit          changed;          // something changed since the last slow-clock edge
  protected bit [63:0]   in_last;          // the model's inputs at the last sample
  protected bit [255:0]  hs_last;          // the handshakes at the last sample
  protected bit          esc_prev;         // esc_p 1 / esc_n 0 at the previous fast-clock sample
  protected bit          sampled;
  protected int          tl_outstanding;   // requests seen, their responses not yet

  // Counts for the report.
  protected int unsigned n_rd_cmp, n_rd_exact, n_rd_open, n_intr_cmp, n_mismatch, n_writes;
  protected int unsigned n_inputs, n_settled;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_a_fifo = new("tl_a_fifo", this);
    tl_d_fifo = new("tl_d_fifo", this);
    if (cfg.en_cov) cov = pwrmgr_reg_cov::type_id::create("pwrmgr_reg_cov");
    void'($value$plusargs("pwrmgr_cmodel_settle=%d", settle_slow));
    void'($value$plusargs("pwrmgr_cmodel_corrupt=%b", corrupt));
    void'($value$plusargs("pwrmgr_cmodel_history=%d", hist_len));
    h = eg2_pwrmgr_cmodel_pkg::eg2_pwr_create();
  endfunction

  task run_phase(uvm_phase phase);
    fork
      watch_reset();
      forever begin
        @(posedge cfg.pwrmgr_vif.clk);
        sample(1'b1);
      end
      forever begin
        @(posedge cfg.pwrmgr_vif.clk_slow);
        sample(1'b0);
        slow_tick();
      end
      forever process_tl();
      forever process_tl_a();
    join_none
  endtask

  protected task watch_reset();
    forever begin
      @(negedge cfg.clk_rst_vif.rst_n);
      eg2_pwrmgr_cmodel_pkg::eg2_pwr_reset(h);
      tl_a_fifo.flush();
      tl_d_fifo.flush();
      tl_outstanding = 0;
      sampled = 1'b0;
      quiet_cnt = 0;
      note("block reset");
      if (cov != null) cov.reset();
    end
  endtask

  protected function bit quiet();
    return quiet_cnt >= settle_slow;
  endfunction

  // ---- Ports ----

  protected function void sample(bit fast);
    bit [7:0] wake;
    bit [1:0] rst;
    bit esc_now, esc, sleeping, sw_rst, pok, idle, active, pd_n;
    bit [63:0] in_now;
    bit [255:0] hs_now;
    if (cfg.clk_rst_vif.rst_n !== 1'b1) return;
    wake     = cfg.pwrmgr_vif.wakeups_i;
    rst      = cfg.pwrmgr_vif.rstreqs_i;
    sleeping = cfg.pwrmgr_vif.pwr_cpu.core_sleeping === 1'b1;
    sw_rst   = cfg.pwrmgr_vif.sw_rst_req_i == prim_mubi_pkg::MuBi4True;
    pok      = cfg.pwrmgr_vif.pwr_ast_rsp.main_pok === 1'b1;
    idle     = cfg.pwrmgr_vif.pwr_otp_rsp.otp_idle === 1'b1 &&
               cfg.pwrmgr_vif.pwr_lc_rsp.lc_idle === 1'b1 &&
               cfg.pwrmgr_vif.pwr_nvm.nvm_idle === 1'b1;
    active   = cfg.pwrmgr_vif.fetch_en == lc_ctrl_pkg::On;
    pd_n     = cfg.pwrmgr_vif.pwr_ast_req.main_pd_n === 1'b1;
    // The escalation request is the esc_p 1 / esc_n 0 level; a ping is a one-cycle pulse.
    esc_now  = cfg.m_esc_agent_cfg.vif.esc_tx.esc_p === 1'b1 &&
               cfg.m_esc_agent_cfg.vif.esc_tx.esc_n === 1'b0;
    if (fast) begin
      esc = esc_now && esc_prev;
      esc_prev = esc_now;
    end else begin
      esc = in_last[10];  // held: the escalation clock is the fast clock
    end
    // Layout (pwrmgr_reg_cov::sample_ports decodes it): wake 7:0, rst 9:8, esc 10, sleeping 11,
    // sw_rst 12, main_pok 13, idle 14, active 15, main_pd_n request 16.
    in_now = 64'({pd_n, active, idle, pok, sw_rst, sleeping, esc, rst, wake});
    hs_now = {cfg.pwrmgr_vif.pwr_ast_req, cfg.pwrmgr_vif.pwr_ast_rsp, cfg.pwrmgr_vif.pwr_clk_req,
              cfg.pwrmgr_vif.pwr_clk_rsp, cfg.pwrmgr_vif.pwr_rst_req, cfg.pwrmgr_vif.pwr_rst_rsp,
              cfg.pwrmgr_vif.pwr_otp_req, cfg.pwrmgr_vif.pwr_otp_rsp, cfg.pwrmgr_vif.pwr_lc_req,
              cfg.pwrmgr_vif.pwr_lc_rsp, cfg.pwrmgr_vif.fetch_en, cfg.pwrmgr_vif.low_power,
              cfg.pwrmgr_vif.rom_ctrl_i};
    if (sampled && hs_now !== hs_last) changed = 1'b1;
    hs_last = hs_now;
    if (!sampled || in_now !== in_last) begin
      changed = 1'b1;
      n_inputs++;
      if (cov != null && sampled) cov.sample_ports(in_last, in_now);
      note($sformatf("inputs wake %02h rst %0b esc %0b sleeping %0b sw_rst %0b main_pok %0b idle %0b active %0b main_pd_n_req %0b",
                     wake, rst, esc, sleeping, sw_rst, pok, idle, active, pd_n));
      eg2_pwrmgr_cmodel_pkg::eg2_pwr_inputs(h, wake, rst, esc, sleeping, sw_rst, pok, idle, active,
                                            pd_n);
      in_last = in_now;
      sampled = 1'b1;
    end
  endfunction

  protected function void slow_tick();
    if (cfg.clk_rst_vif.rst_n !== 1'b1) return;
    if (changed) begin
      quiet_cnt = 0;
      changed = 1'b0;
      return;
    end
    if (quiet_cnt < settle_slow) begin
      quiet_cnt++;
      if (quiet_cnt == settle_slow) begin
        eg2_pwrmgr_cmodel_pkg::eg2_pwr_settled(h);
        n_settled++;
        if (cov != null && in_last[15]) cov.sample_settled_active();
      end
    end
    // Not while a request is in flight: pwrmgr acts on a write at its request, the model at the
    // response (2026-11-07, pwrmgr_cmodel_smoke seed 2: INTR_STATE cleared between the two).
    if (quiet() && tl_outstanding == 0) check_intr();
  endfunction

  protected function void check_intr();
    int unsigned lo, hi;
    bit pin = cfg.intr_vif.pins[0] === 1'b1;
    eg2_pwrmgr_cmodel_pkg::eg2_pwr_intr(h, 1, lo, hi);
    n_intr_cmp++;
    if ((pin & ~hi[0]) || (lo[0] & ~pin)) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf("intr_wakeup_o: pwrmgr %0b, model must %0b may %0b%0s", pin, lo[0],
                                 hi[0], n_mismatch <= 5 ? history() : ""))
    end
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
      if (cov != null) cov.sample_write(addr, it.a_data, read_now('h10), read_now('h1c), read_now('h28));
      eg2_pwrmgr_cmodel_pkg::eg2_pwr_csr_write(h, addr, it.a_data, q);
      note($sformatf("write 0x%02h = 0x%08h%0s", addr, it.a_data, q ? "" : " (settling)"));
      // A configuration write crosses into the slow clock domain: the design settles again.
      changed = 1'b1;
      return;
    end
    d = it.d_data;
    eg2_pwrmgr_cmodel_pkg::eg2_pwr_csr_read(h, addr, q, lo, hi);
    note($sformatf("read 0x%02h: pwrmgr 0x%08h, model must 0x%08h may 0x%08h%0s", addr, d, lo, hi,
                   q ? "" : " (settling)"));
    if (cov != null) cov.sample_read(addr, d, q);
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
      `uvm_error(`gfn, $sformatf("read 0x%02h (%0s): pwrmgr 0x%08h, model must 0x%08h may 0x%08h%0s",
                                 addr, reg_name(addr), d, lo, hi, n_mismatch <= 5 ? history() : ""))
    end
  endtask

  protected task process_tl_a();
    tl_seq_item it;
    tl_a_fifo.get(it);
    tl_outstanding++;
    if (it.is_write()) changed = 1'b1;
  endtask

  // The model's value of a register (for coverage context).
  protected function int unsigned read_now(int unsigned addr);
    return eg2_pwrmgr_cmodel_pkg::eg2_pwr_csr_value(h, addr);
  endfunction

  protected static function string reg_name(int unsigned addr);
    case (addr)
      'h00: return "INTR_STATE";
      'h04: return "INTR_ENABLE";
      'h08: return "INTR_TEST";
      'h0c: return "ALERT_TEST";
      'h10: return "CTRL_CFG_REGWEN";
      'h14: return "CONTROL";
      'h18: return "CFG_CDC_SYNC";
      'h1c: return "WAKEUP_EN_REGWEN";
      'h20: return "WAKEUP_EN";
      'h24: return "WAKE_STATUS";
      'h28: return "RESET_EN_REGWEN";
      'h2c: return "RESET_EN";
      'h30: return "RESET_STATUS";
      'h34: return "ESCALATE_RESET_STATUS";
      'h38: return "WAKE_INFO_CAPTURE_DIS";
      'h3c: return "WAKE_INFO";
      'h40: return "FAULT_STATUS";
      default: return "?";
    endcase
  endfunction

  // The last inputs the model was given, for the mismatch reports.
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
    if (n_rd_cmp == 0) `uvm_error(`gfn, "the model compared no register read")
    if (n_settled == 0) `uvm_error(`gfn, "the design never settled: the model compared no quiet state")
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"pwrmgr cmodel: compared reads %0d (exact %0d, with open bits %0d), ",
                               "interrupt output %0d; mismatches %0d; writes %0d, input changes %0d, ",
                               "settled %0d times, low-power entries %0d, reset requests %0d"},
                              n_rd_cmp, n_rd_exact, n_rd_open, n_intr_cmp, n_mismatch, n_writes,
                              n_inputs, n_settled, eg2_pwrmgr_cmodel_pkg::eg2_pwr_entries(h),
                              eg2_pwrmgr_cmodel_pkg::eg2_pwr_resets(h)), UVM_NONE)
    if (h != null) eg2_pwrmgr_cmodel_pkg::eg2_pwr_release(h);
    h = null;
  endfunction
endclass
