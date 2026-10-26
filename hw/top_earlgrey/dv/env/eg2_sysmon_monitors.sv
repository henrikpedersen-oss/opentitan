// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 system monitor (status/decisions.md 19: a monitor and a scoreboard per block, as the
// per-peripheral scoreboards). Compiled with EG2_PERIPH_SB only. One monitor per block:
//
//   eg2_sysrst_ctrl_monitor   sysrst_ctrl   its output pins and reset request; its status registers
//   eg2_adc_ctrl_monitor      adc_ctrl      its interrupt and wake-up lines; its status and values
//   eg2_sensor_ctrl_monitor   sensor_ctrl   the alerts it forwards; its alert and status registers
//   eg2_wkup_monitor          wkup          pinmux's wake-up request; WKUP_CAUSE, pwrmgr WAKE_INFO
//
// Each takes its block's TL-UL port through its tap (tb/eg2_periph_tl.svh) and the inputs the bench
// and the AST drive into the block (eg2_sysmon_if), runs the block's documented behaviour on the
// block's own always-on clock, and writes:
//   pin_port      actual: the block's outputs as one state item (eg2_sysmon_item SysState), a new
//                 item when they have changed and stayed for `settle_cycles` always-on cycles; or an
//                 event (SysEvent, sensor_ctrl's alerts)
//   tl_exp_port   expected: the same, from the model
//   rx_exp_port   the expected value of each status register the core reads, with the bits the
//                 model fixes (SysRead `care`)
//   rx_rd_port    that read, from the TL-UL read response
// Both state streams are filtered alike (a value must stay `settle_cycles` cycles), so a difference
// of a few cycles in when an output changes is not a mismatch; the order and the values are.
//
// Where the model cannot know (a timer that ends within `race_cycles` of an input change, a
// register write that crosses into the always-on domain while an event happens), it says so: the
// output bit or status bit is not predicted (`care` cleared) for as long as the doubt lasts, and the
// scoreboard compares the other bits. The model is configured from each block's documentation
// (README, doc/theory_of_operation.md, doc/registers.md, data/<ip>.hjson), never from its RTL;
// where it ports the prediction of an OpenTitan block-level scoreboard, the class says which.

class eg2_sysmon_monitor extends uvm_monitor;
  `uvm_component_utils(eg2_sysmon_monitor)

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_fifo;
  uvm_analysis_port #(eg2_sysmon_item) pin_port;
  uvm_analysis_port #(eg2_sysmon_item) tl_exp_port;
  uvm_analysis_port #(eg2_sysmon_item) rx_exp_port;
  uvm_analysis_port #(eg2_sysmon_item) rx_rd_port;

  virtual eg2_sysmon_if vif;
  string        inst = "sysmon";
  string        tap = "";           // the TL-UL tap this monitor converts
  bit [31:0]    tap_size = 32'h100;
  string        state_name = "outputs";
  string        state_fields = "";
  realtime      aon_period = 5us;   // measured at the start (5 us at 200 kHz)
  int unsigned  settle_cycles = 10;
  int unsigned  race_cycles = 6;
  bit [31:0]    state_mask = '1;    // the bits of the state item that exist
  bit           has_state = 1'b1;   // 0: the block's outputs are events only

  // The model's output bits it predicts now.
  bit [31:0]              care = '1;
  protected bit [31:0]    act_raw, exp_raw, act_last, exp_last;
  protected bit           act_emitted, exp_emitted;
  protected int unsigned  act_gen, exp_gen;
  protected event         act_ev, exp_ev;
  protected string        exp_why;
  protected longint unsigned cycle;   // always-on cycles since the block's reset

  // Status registers the model keeps (by name), the bits that changed lately (a read close to a
  // change compares the other bits) and the bits it is unsure of.
  protected bit [31:0] st[string];
  protected bit [31:0] st_recent[string];
  protected realtime   st_t[string];
  protected bit [31:0] st_maybe[string];
  int unsigned         n_reads, n_events;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_fifo     = new("tl_fifo", this);
    pin_port    = new("pin_port", this);
    tl_exp_port = new("tl_exp_port", this);
    rx_exp_port = new("rx_exp_port", this);
    rx_rd_port  = new("rx_rd_port", this);
  endfunction

  // ----- What each block's monitor provides -----

  virtual task wait_aon();
    #(aon_period);
  endtask

  virtual function bit in_reset();
    return 1'b0;
  endfunction

  // Wait for the block's reset to be asserted, and released (wait() needs the signals themselves).
  virtual task wait_reset_on();
    wait (0);
  endtask

  virtual task wait_reset_off();
  endtask

  // The block's outputs as the state item's value.
  virtual function bit [31:0] sample_outputs();
    return '0;
  endfunction

  // A register write (the access was accepted) and a read with its response.
  virtual function void tl_write(bit [31:0] off, tl_seq_item t);
  endfunction

  virtual function void tl_read(bit [31:0] off, tl_seq_item t);
  endfunction

  // One always-on cycle of the model; and its reset.
  virtual function void step();
  endfunction

  virtual function void model_reset();
  endfunction

  // The model's outputs (calls set_exp).
  virtual function void refresh();
  endfunction

  virtual task watch_outputs();
  endtask

  // ----- The run -----

  virtual task run_phase(uvm_phase phase);
    if (vif == null) return;
    fork
      measure_aon();
      tl_thread();
      settle_act();
      settle_exp();
      model_thread();
      reset_thread();
      watch_outputs();
    join
  endtask

  protected task measure_aon();
    realtime t0;
    wait_reset_off();
    wait_aon();
    t0 = $realtime;
    wait_aon();
    if ($realtime > t0) aon_period = $realtime - t0;
  endtask

  protected function realtime race_window();
    return race_cycles * aon_period;
  endfunction

  protected task model_thread();
    forever begin
      wait_aon();
      if (in_reset()) continue;
      cycle++;
      step();
      refresh();
    end
  endtask

  protected task reset_thread();
    forever begin
      wait_reset_on();
      model_reset();
      cycle = 0;
      st.delete();
      st_recent.delete();
      st_t.delete();
      st_maybe.delete();
      care = '1;
      marker(tl_exp_port, eg2_periph_marker::MarkFlush, 0, {inst, " reset"});
      marker(rx_exp_port, eg2_periph_marker::MarkFlush, 0, {inst, " reset"});
      // Both sides give their state again once out of reset.
      act_emitted = 1'b0;
      exp_emitted = 1'b0;
      act_gen++;
      exp_gen++;
      wait_reset_off();
      refresh();
      act_raw = sample_outputs();
      act_gen++;
      exp_gen++;
      -> act_ev;
      -> exp_ev;
    end
  endtask

  protected task tl_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      tl_fifo.get(t);
      // An access the port refused (d_error) or the documents refuse changes nothing.
      if (t.d_error || (tap != "" && eg2_tl_err::refused(tap, t))) continue;
      if (in_reset()) continue;
      off = eg2_tl_offset(t, tap_size);
      if (t.is_write()) tl_write(off, t);
      else tl_read(off, t);
      refresh();
    end
  endtask

  protected function void marker(uvm_analysis_port #(eg2_sysmon_item) port,
                                 eg2_periph_marker::kind_e kind, int keep, string why);
    eg2_sysmon_item m = eg2_sysmon_item::type_id::create("marker");
    eg2_periph_marker::mark(m, kind, keep, why);
    port.write(m);
  endfunction

  // ----- The state items -----

  // The model's outputs changed (or its care mask did).
  protected function void set_exp(bit [31:0] v, string why = "");
    if (v == exp_raw && why == exp_why) return;
    exp_raw = v;
    exp_why = why;
    exp_gen++;
    -> exp_ev;
  endfunction

  // The care mask changes: both sides look again.
  protected function void set_care(bit [31:0] c);
    if (c == care) return;
    care = c;
    act_gen++;
    exp_gen++;
    -> act_ev;
    -> exp_ev;
  endfunction

  protected function void set_act(bit [31:0] v);
    if (v == act_raw) return;
    act_raw = v;
    act_gen++;
    -> act_ev;
  endfunction

  protected function realtime settle_time();
    return settle_cycles * aon_period;
  endfunction

  protected function eg2_sysmon_item state_item(bit [31:0] v, bit [31:0] c, string why);
    eg2_sysmon_item it = eg2_sysmon_item::type_id::create("state");
    it.kind   = eg2_sysmon_item::SysState;
    it.name   = state_name;
    it.value  = v & c;
    it.care   = c;
    it.fields = state_fields;
    it.why    = why;
    return it;
  endfunction

  protected task settle_act();
    forever begin
      int unsigned g;
      bit [31:0]   v;
      @(act_ev);
      do begin
        g = act_gen;
        #(settle_time());
      end while (g != act_gen);
      if (in_reset() || !has_state) continue;
      v = act_raw & care & state_mask;
      if (act_emitted && v == act_last) continue;
      act_emitted = 1'b1;
      act_last = v;
      pin_port.write(state_item(v, '1, ""));
    end
  endtask

  protected task settle_exp();
    forever begin
      int unsigned g;
      bit [31:0]   v;
      @(exp_ev);
      do begin
        g = exp_gen;
        #(settle_time());
      end while (g != exp_gen);
      if (in_reset() || !has_state) continue;
      v = exp_raw & care & state_mask;
      if (exp_emitted && v == exp_last) continue;
      exp_emitted = 1'b1;
      exp_last = v;
      tl_exp_port.write(state_item(v, care & state_mask, exp_why));
    end
  endtask

  // ----- Status registers and the core's reads of them -----

  protected function bit [31:0] get_st(string n);
    return st.exists(n) ? st[n] : '0;
  endfunction

  protected function void set_st(string n, bit [31:0] v);
    bit [31:0] old = get_st(n);
    if (v == old && st.exists(n)) return;
    if (!st_t.exists(n) || $realtime - st_t[n] > race_window()) st_recent[n] = '0;
    st_recent[n] |= v ^ old;
    st_t[n] = $realtime;
    st[n] = v;
  endfunction

  // Bits the model is unsure of (until clear_maybe).
  protected function void set_maybe(string n, bit [31:0] bits);
    if (!st_maybe.exists(n)) st_maybe[n] = '0;
    st_maybe[n] |= bits;
  endfunction

  protected function void clear_maybe(string n, bit [31:0] bits);
    if (st_maybe.exists(n)) st_maybe[n] &= ~bits;
  endfunction

  protected function bit [31:0] st_care(string n);
    bit [31:0] c = '1;
    if (st_t.exists(n) && $realtime - st_t[n] <= race_window()) c &= ~st_recent[n];
    if (st_maybe.exists(n)) c &= ~st_maybe[n];
    return c;
  endfunction

  // A read of status register `n`: its expected value (the model's, on the bits `mask` holds and
  // the model fixes) and the value the core read.
  protected function void read_check(string n, bit [31:0] actual, bit [31:0] mask,
                                     bit [31:0] expected, bit [31:0] extra_care = '1);
    eg2_sysmon_item e = eg2_sysmon_item::type_id::create("rx_exp");
    eg2_sysmon_item a = eg2_sysmon_item::type_id::create("rx_rd");
    e.kind  = eg2_sysmon_item::SysRead;
    e.name  = n;
    e.care  = mask & extra_care & st_care(n);
    e.value = expected & e.care;
    a.kind  = eg2_sysmon_item::SysRead;
    a.name  = n;
    a.value = actual & mask;
    a.care  = mask;
    n_reads++;
    rx_exp_port.write(e);
    rx_rd_port.write(a);
  endfunction

  protected function void read_st(string n, bit [31:0] actual, bit [31:0] mask,
                                  bit [31:0] extra_care = '1);
    read_check(n, actual, mask, get_st(n), extra_care);
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// sysrst_ctrl (hw/ip/sysrst_ctrl: README.md, doc/theory_of_operation.md, doc/registers.md,
// data/sysrst_ctrl.hjson). OpenTitan's block scoreboard (hw/ip/sysrst_ctrl/dv/env/
// sysrst_ctrl_scoreboard.sv) predicts only INTR_STATE and leaves the pins to its sequences; the
// output rule here is the one its sequences check (sysrst_ctrl_pin_override_vseq::perform_checks:
// an enabled override drives VALUE if the matching ALLOWED bit is set, else the pin passes through;
// sysrst_ctrl_in_out_inverted_vseq: out = in ^ in_invert ^ out_invert), with the doc's timed
// features added.
//
// Item: the eight outputs and the reset request, bit 0 bat_disable, 1 ec_rst_l, 2 pwrb_out,
// 3 key0_out, 4 key1_out, 5 key2_out, 6 z3_wakeup, 7 flash_wp_l (the open-drain ec_rst_l and
// flash_wp_l as the level the block leaves on the pin: 0 when it drives low), 8 rst_req.
// Expected, per output: the base value (pwrb_out and key<n>_out pass the input through after
// KEY_INVERT_CTL's input inversion, the auto-block value while AUTO_BLOCK has fired; bat_disable 0
// until a combo with the bat_disable action fires; z3_wakeup the ULP detection; ec_rst_l low for
// EC_RST_CTL always-on cycles after ec_rst_l_i falls or a combo with the ec_rst action fires;
// flash_wp_l 0), then PIN_OUT_CTL/PIN_OUT_VALUE/PIN_ALLOWED_CTL's override, then the output
// inversion; rst_req once a combo with the rst_req action fires.
// RX: KEY_INTR_STATUS (edges per KEY_INTR_CTL that hold past KEY_INTR_DEBOUNCE_CTL), COMBO_INTR_STATUS
// (combos with the interrupt action), ULP_STATUS, WKUP_STATUS (any of those), PIN_IN_VALUE (the raw
// inputs). Each timer counts always-on cycles and must be exceeded by one cycle (registers.md).
// Not predicted: KEY_INTR_STATUS bits of an input whose input inversion is set (the doc does not
// say whether edge detection sees the inverted input); the interrupt and wake-up lines themselves
// (their sources are, through the status reads); INTR_STATE; anything within `race_cycles` of a
// timer's threshold.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_sysrst_ctrl_monitor extends eg2_sysmon_monitor;
  `uvm_component_utils(eg2_sysrst_ctrl_monitor)

  // Registers (as written; REGWEN locks the configuration).
  protected bit        regwen = 1'b1;
  protected bit [15:0] ec_rst_pulse = 16'h7d0;
  protected bit [15:0] ulp_ac_db = 16'h1f40, ulp_lid_db = 16'h1f40, ulp_pwrb_db = 16'h1f40;
  protected bit        ulp_en;
  protected bit [11:0] key_invert;
  protected bit [15:0] pin_allowed = 16'h0082;
  protected bit [7:0]  pin_out_ctl = 8'h82;
  protected bit [7:0]  pin_out_value;
  protected bit [13:0] key_intr_ctl;
  protected bit [15:0] key_intr_db = 16'h7d0;
  protected bit [15:0] auto_db = 16'h7d0;
  protected bit        auto_en;
  protected bit [2:0]  auto_sel, auto_val;
  protected bit [4:0]  com_pre_sel[4], com_sel[4];
  protected bit [31:0] com_pre_det[4], com_det[4];
  protected bit [3:0]  com_out[4];
  protected longint unsigned cfg_cycle;   // the last configuration write

  // Model state.
  protected bit [7:0]  prev_in;
  protected longint unsigned last_chg[8];
  protected bit        bat_latched, rst_latched, ulp_det, ab_active;
  protected longint unsigned ec_pulse;
  // Key interrupt debouncers: per KEY_INTR_STATUS bit.
  protected bit        kd_active[14];
  protected longint unsigned kd_cnt[14];
  // Combos.
  protected longint unsigned pre_cnt[4], trig_cnt[4];
  protected bit        trig_counting[4], fired[4], prev_trig_low[4], edge_unsure[4];
  // ULP and auto-block.
  protected longint unsigned ulp_ac_cnt, ulp_pwrb_cnt, ulp_lid_cnt, ab_cnt;
  protected bit        ulp_pwrb_on, ulp_lid_on, ab_counting, prev_ipwrb = 1'b1, prev_ilid;
  // Doubts: an output bit not predicted until that cycle.
  protected longint unsigned care_off_until[9];
  protected bit        care_off_until_reset[9];
  eg2_sysrst_cg        sysrst_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    state_name = "outputs";
    state_fields = "(bat_disable ec_rst_l pwrb_out key0..2_out z3_wakeup flash_wp_l rst_req = bits 0..8)";
    state_mask = 32'h1ff;
    tap = "sysrst_ctrl";
    tap_size = 32'h100;
  endfunction

  function void create_cov(string path);
    sysrst_cg = new({path, "::eg2_sysrst_cg"});
  endfunction

  virtual task wait_aon();
    @(posedge vif.sysrst_clk_aon);
  endtask

  virtual function bit in_reset();
    return vif.sysrst_rst_aon_n !== 1'b1 || vif.sysrst_rst_n !== 1'b1;
  endfunction

  virtual task wait_reset_on();
    wait (vif.sysrst_rst_aon_n !== 1'b1 || vif.sysrst_rst_n !== 1'b1);
  endtask

  virtual task wait_reset_off();
    wait (vif.sysrst_rst_aon_n === 1'b1 && vif.sysrst_rst_n === 1'b1);
  endtask

  protected function bit lvl(logic v);
    return v === 1'b1;
  endfunction

  virtual function bit [31:0] sample_outputs();
    bit [31:0] v;
    for (int b = 0; b < 8; b++) begin
      if (b == 1 || b == 7) v[b] = !(vif.sysrst_oe[b] === 1'b1 && vif.sysrst_out[b] === 1'b0);
      else v[b] = lvl(vif.sysrst_out[b]);
    end
    v[8] = lvl(vif.sysrst_rst_req);
    return v;
  endfunction

  virtual task watch_outputs();
    forever begin
      @(vif.sysrst_out or vif.sysrst_oe or vif.sysrst_rst_req);
      if (!in_reset()) set_act(sample_outputs());
    end
  endtask

  protected function bit [7:0] sample_in();
    bit [7:0] v;
    for (int b = 0; b < 8; b++) v[b] = lvl(vif.sysrst_in[b]);
    return v;
  endfunction

  virtual function void model_reset();
    regwen = 1'b1;
    ec_rst_pulse = 16'h7d0;
    ulp_ac_db = 16'h1f40;
    ulp_lid_db = 16'h1f40;
    ulp_pwrb_db = 16'h1f40;
    ulp_en = 1'b0;
    key_invert = '0;
    pin_allowed = 16'h0082;
    pin_out_ctl = 8'h82;
    pin_out_value = '0;
    key_intr_ctl = '0;
    key_intr_db = 16'h7d0;
    auto_db = 16'h7d0;
    auto_en = 1'b0;
    auto_sel = '0;
    auto_val = '0;
    foreach (com_sel[i]) begin
      com_pre_sel[i] = '0;
      com_sel[i] = '0;
      com_pre_det[i] = '0;
      com_det[i] = '0;
      com_out[i] = '0;
      pre_cnt[i] = 0;
      trig_cnt[i] = 0;
      trig_counting[i] = 1'b0;
      fired[i] = 1'b0;
      prev_trig_low[i] = 1'b0;
      edge_unsure[i] = 1'b0;
    end
    bat_latched = 1'b0;
    rst_latched = 1'b0;
    ulp_det = 1'b0;
    ab_active = 1'b0;
    ec_pulse = 0;
    foreach (kd_active[i]) begin
      kd_active[i] = 1'b0;
      kd_cnt[i] = 0;
    end
    ulp_ac_cnt = 0;
    ulp_pwrb_cnt = 0;
    ulp_lid_cnt = 0;
    ulp_pwrb_on = 1'b0;
    ulp_lid_on = 1'b0;
    ab_cnt = 0;
    ab_counting = 1'b0;
    foreach (care_off_until[i]) begin
      care_off_until[i] = 0;
      care_off_until_reset[i] = 1'b0;
    end
    foreach (last_chg[i]) last_chg[i] = 0;
    prev_in = sample_in();
    prev_ipwrb = prev_in[0];
    prev_ilid = prev_in[5];
    cfg_cycle = 0;
  endfunction

  // The inputs after KEY_INVERT_CTL's input inversion: {ac_present, pwrb, key2, key1, key0} as
  // COM_SEL_CTL orders them, and lid_open.
  protected function bit [4:0] inv_keys(bit [7:0] in);
    return {in[4] ^ key_invert[8], in[0] ^ key_invert[6], in[3] ^ key_invert[4],
            in[2] ^ key_invert[2], in[1] ^ key_invert[0]};
  endfunction

  protected function void doubt(int b, longint unsigned cycles);
    if (cycle + cycles > care_off_until[b]) care_off_until[b] = cycle + cycles;
  endfunction

  protected function void cov(int unsigned kind, int unsigned which);
    if (sysrst_cg != null) sysrst_cg.sample(kind, which);
  endfunction

  // The events every timed feature raises.
  protected function void raise_status(string n, bit [31:0] bits, bit unsure);
    set_st(n, get_st(n) | bits);
    if (unsure) set_maybe(n, bits);
    set_st("WKUP_STATUS", 1);
    if (unsure) set_maybe("WKUP_STATUS", 1);
  endfunction

  protected function bit near(longint unsigned a, longint unsigned b);
    return (a > b ? a - b : b - a) <= race_cycles;
  endfunction

  virtual function void step();
    bit [7:0]  in = sample_in();
    bit [4:0]  ik = inv_keys(in);
    bit        ipwrb = ik[3], iac = ik[4];
    bit        ilid = in[5] ^ key_invert[10];
    bit [6:0]  ks = {in[7], in[6], in[4], in[3], in[2], in[1], in[0]};
    bit [6:0]  pks = {prev_in[7], prev_in[6], prev_in[4], prev_in[3], prev_in[2], prev_in[1],
                      prev_in[0]};
    bit        cfg_recent = cycle - cfg_cycle <= race_cycles;
    for (int b = 0; b < 8; b++) if (in[b] != prev_in[b]) last_chg[b] = cycle;

    // EC reset stretching: ec_rst_l_i falling.
    if (prev_in[6] && !in[6]) begin
      ec_pulse = ec_rst_pulse;
      cov(6, 0);
    end else if (ec_pulse > 0) begin
      ec_pulse--;
    end

    // Key interrupts: each enabled edge that holds past the debounce timer (sampled at its start
    // and at its end).
    for (int k = 0; k < 14; k++) begin
      int  s = k % 7;
      bit  l2h = k >= 7;
      bit  edge_seen = l2h ? (!pks[s] && ks[s]) : (pks[s] && !ks[s]);
      if (!kd_active[k] && key_intr_ctl[k] && edge_seen) begin
        kd_active[k] = 1'b1;
        kd_cnt[k] = 0;
      end else if (kd_active[k]) begin
        kd_cnt[k]++;
        if (kd_cnt[k] > key_intr_db) begin
          longint unsigned lc = last_chg[s == 4 ? 4 : s == 5 ? 6 : s == 6 ? 7 : s];
          bit unsure = near(cycle, lc) || cfg_recent;
          // An input with its input inversion set: not predicted.
          if (s <= 4 && key_invert[s == 0 ? 6 : s == 4 ? 8 : 2 * (s - 1)]) unsure = 1'b1;
          kd_active[k] = 1'b0;
          if (ks[s] == l2h) begin
            raise_status("KEY_INTR_STATUS", 1 << k, unsure);
            cov(3, k);
          end else if (unsure) begin
            set_maybe("KEY_INTR_STATUS", 1 << k);
            set_maybe("WKUP_STATUS", 1);
          end
        end
      end
    end

    // Combos: the trigger keys all pressed (low after inversion), debounced, then held for the
    // detection time, while the precondition keys have been held for theirs.
    for (int i = 0; i < 4; i++) begin
      bit trig_low, pre_low, pre_ok, pre_near;
      longint unsigned th, pre_th;
      if (com_sel[i] == 0) continue;
      trig_low = (ik & com_sel[i]) == 0;
      pre_low = (ik & com_pre_sel[i]) == 0;
      pre_th = longint'(key_intr_db) + com_pre_det[i];
      if (com_pre_sel[i] == 0) begin
        pre_ok = 1'b1;
        pre_near = 1'b0;
      end else begin
        if (pre_low) pre_cnt[i]++;
        else pre_cnt[i] = 0;
        pre_ok = pre_cnt[i] > pre_th;
        // The precondition completes within a few cycles of now (either side).
        pre_near = pre_low && near(pre_cnt[i], pre_th);
      end
      th = longint'(key_intr_db) + com_det[i];
      if (!trig_low) begin
        // Released: a press that ended close to the threshold may or may not have fired.
        if (trig_counting[i] && !fired[i] && trig_cnt[i] + race_cycles >= th) combo_fire(i, 1'b1);
        else if (trig_counting[i] && fired[i] && trig_cnt[i] <= th + race_cycles) combo_doubt(i);
        trig_counting[i] = 1'b0;
        fired[i] = 1'b0;
      end else if (!prev_trig_low[i] && (pre_ok || pre_near)) begin
        // The trigger's high-to-low edge with the precondition met (or about to be: unsure).
        trig_counting[i] = 1'b1;
        trig_cnt[i] = 0;
        fired[i] = 1'b0;
        edge_unsure[i] = pre_near;
      end else if (trig_counting[i] && com_pre_sel[i] != 0 && !pre_low) begin
        // The precondition keys were released: the detection stops.
        if (!fired[i] && trig_cnt[i] + race_cycles >= th) combo_fire(i, 1'b1);
        trig_counting[i] = 1'b0;
      end else if (trig_counting[i]) begin
        trig_cnt[i]++;
        if (!fired[i] && trig_cnt[i] > th) begin
          fired[i] = 1'b1;
          combo_fire(i, cfg_recent || edge_unsure[i]);
        end
      end
      prev_trig_low[i] = trig_low;
    end

    // Ultra-low-power wake-up: AC present high, pwrb high to low, lid_open low to high, each held
    // past its debounce timer.
    if (!ulp_en) begin
      ulp_ac_cnt = 0;
      ulp_pwrb_on = 1'b0;
      ulp_lid_on = 1'b0;
    end else if (!ulp_det) begin
      if (iac) ulp_ac_cnt++;
      else begin
        if (ulp_ac_cnt > 0 && ulp_ac_cnt + race_cycles > ulp_ac_db) ulp_fire(1'b1);
        ulp_ac_cnt = 0;
      end
      if (iac && ulp_ac_cnt > ulp_ac_db) ulp_fire(cfg_recent);
      if (prev_ipwrb && !ipwrb) begin
        ulp_pwrb_on = 1'b1;
        ulp_pwrb_cnt = 0;
      end else if (ulp_pwrb_on && !ipwrb) begin
        ulp_pwrb_cnt++;
        if (ulp_pwrb_cnt > ulp_pwrb_db) ulp_fire(cfg_recent);
      end else if (ulp_pwrb_on) begin
        if (ulp_pwrb_cnt + race_cycles > ulp_pwrb_db) ulp_fire(1'b1);
        ulp_pwrb_on = 1'b0;
      end
      if (!prev_ilid && ilid) begin
        ulp_lid_on = 1'b1;
        ulp_lid_cnt = 0;
      end else if (ulp_lid_on && ilid) begin
        ulp_lid_cnt++;
        if (ulp_lid_cnt > ulp_lid_db) ulp_fire(cfg_recent);
      end else if (ulp_lid_on) begin
        if (ulp_lid_cnt + race_cycles > ulp_lid_db) ulp_fire(1'b1);
        ulp_lid_on = 1'b0;
      end
    end

    // Auto-block: pwrb pressed past its debounce timer overrides the selected key outputs while it
    // stays pressed.
    if (!auto_en || ipwrb) begin
      if (ab_counting && auto_en && near(ab_cnt, auto_db)) begin
        doubt(3, settle_cycles + race_cycles);
        doubt(4, settle_cycles + race_cycles);
        doubt(5, settle_cycles + race_cycles);
      end
      ab_active = 1'b0;
      ab_counting = 1'b0;
      ab_cnt = 0;
    end else if (prev_ipwrb && !ipwrb) begin
      ab_counting = 1'b1;
      ab_cnt = 0;
    end else if (ab_counting && !ab_active) begin
      ab_cnt++;
      if (ab_cnt > auto_db) begin
        ab_active = 1'b1;
        cov(7, 0);
        if (cfg_recent) for (int b = 3; b <= 5; b++) doubt(b, settle_cycles + race_cycles);
      end
    end

    prev_in = in;
    prev_ipwrb = ipwrb;
    prev_ilid = ilid;
  endfunction

  protected function void combo_fire(int i, bit unsure);
    cov(4, i);
    if (com_out[i][0]) begin
      if (unsure) care_off_until_reset[0] = 1'b1;
      bat_latched = 1'b1;
    end
    if (com_out[i][1]) raise_status("COMBO_INTR_STATUS", 1 << i, unsure);
    if (com_out[i][2]) begin
      if (unsure) doubt(1, longint'(ec_rst_pulse) + settle_cycles + race_cycles);
      ec_pulse = ec_rst_pulse;
    end
    if (com_out[i][3]) begin
      if (unsure) care_off_until_reset[8] = 1'b1;
      rst_latched = 1'b1;
    end
  endfunction

  // A combo the model fired may not have fired (released right after the threshold).
  protected function void combo_doubt(int i);
    if (com_out[i][0]) care_off_until_reset[0] = 1'b1;
    if (com_out[i][1]) begin
      set_maybe("COMBO_INTR_STATUS", 1 << i);
      set_maybe("WKUP_STATUS", 1);
    end
    if (com_out[i][2]) doubt(1, longint'(ec_rst_pulse) + settle_cycles + race_cycles);
    if (com_out[i][3]) care_off_until_reset[8] = 1'b1;
  endfunction

  protected function void ulp_fire(bit unsure);
    cov(5, 0);
    ulp_det = 1'b1;
    if (unsure) care_off_until_reset[6] = 1'b1;
    raise_status("ULP_STATUS", 1, unsure);
  endfunction

  virtual function void refresh();
    bit [7:0]  in = sample_in();
    bit [4:0]  ik = inv_keys(in);
    bit [7:0]  base, out;
    bit [31:0] c = '1;
    base[0] = bat_latched;
    base[1] = ec_pulse == 0;
    base[2] = ik[3];
    base[3] = ab_active && auto_sel[0] ? auto_val[0] : ik[0];
    base[4] = ab_active && auto_sel[1] ? auto_val[1] : ik[1];
    base[5] = ab_active && auto_sel[2] ? auto_val[2] : ik[2];
    base[6] = ulp_det;
    base[7] = 1'b0;
    for (int b = 0; b < 8; b++) begin
      out[b] = base[b];
      if (pin_out_ctl[b]) begin
        if (pin_out_value[b] && pin_allowed[8 + b]) out[b] = 1'b1;
        else if (!pin_out_value[b] && pin_allowed[b]) out[b] = 1'b0;
      end
    end
    out[0] ^= key_invert[9];
    out[2] ^= key_invert[7];
    out[3] ^= key_invert[1];
    out[4] ^= key_invert[3];
    out[5] ^= key_invert[5];
    out[6] ^= key_invert[11];
    for (int b = 0; b < 9; b++) begin
      if (care_off_until_reset[b] || cycle < care_off_until[b]) c[b] = 1'b0;
    end
    set_care(c);
    set_exp({rst_latched, out});
  endfunction

  protected function bit locked();
    return !regwen;
  endfunction

  virtual function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    // A configuration write takes a few always-on cycles to arrive: events close to it are unsure.
    if (!(off inside {sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_STATUS_OFFSET,
                      sysrst_ctrl_reg_pkg::SYSRST_CTRL_WKUP_STATUS_OFFSET,
                      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INTR_STATUS_OFFSET,
                      sysrst_ctrl_reg_pkg::SYSRST_CTRL_COMBO_INTR_STATUS_OFFSET})) begin
      cfg_cycle = cycle;
    end
    case (off)
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_REGWEN_OFFSET: if (!d[0]) regwen = 1'b0;
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_EC_RST_CTL_OFFSET: if (!locked()) ec_rst_pulse = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_AC_DEBOUNCE_CTL_OFFSET: if (!locked()) ulp_ac_db = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_LID_DEBOUNCE_CTL_OFFSET: if (!locked()) ulp_lid_db = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_PWRB_DEBOUNCE_CTL_OFFSET: if (!locked()) ulp_pwrb_db = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_CTL_OFFSET: begin
        ulp_en = d[0];
        // Disabling resets the ULP state (z3_wakeup returns to 0).
        if (!ulp_en) begin
          ulp_det = 1'b0;
          care_off_until_reset[6] = 1'b0;
          doubt(6, settle_cycles + race_cycles);
        end
      end
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_STATUS_OFFSET: w1c("ULP_STATUS", d[0]);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_WKUP_STATUS_OFFSET: w1c("WKUP_STATUS", d[0]);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INVERT_CTL_OFFSET: if (!locked()) begin
        key_invert = d[11:0];
        cov(2, d[11:0] != 0);
      end
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_PIN_ALLOWED_CTL_OFFSET: if (!locked()) pin_allowed = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_PIN_OUT_CTL_OFFSET: begin
        pin_out_ctl = d[7:0];
        for (int b = 0; b < 8; b++) if (d[b]) cov(1, b);
      end
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_PIN_OUT_VALUE_OFFSET: pin_out_value = d[7:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INTR_CTL_OFFSET: if (!locked()) key_intr_ctl = d[13:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INTR_DEBOUNCE_CTL_OFFSET: if (!locked()) key_intr_db = d[15:0];
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_AUTO_BLOCK_DEBOUNCE_CTL_OFFSET: if (!locked()) begin
        auto_db = d[15:0];
        auto_en = d[16];
      end
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_AUTO_BLOCK_OUT_CTL_OFFSET: if (!locked()) begin
        auto_sel = d[2:0];
        auto_val = d[6:4];
      end
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INTR_STATUS_OFFSET: w1c("KEY_INTR_STATUS", d[13:0]);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_COMBO_INTR_STATUS_OFFSET: w1c("COMBO_INTR_STATUS", d[3:0]);
      default: begin
        for (int i = 0; i < 4; i++) begin
          if (locked()) break;
          if (off == sysrst_ctrl_reg_pkg::SYSRST_CTRL_COM_PRE_SEL_CTL_0_OFFSET + 4 * i) com_pre_sel[i] = d[4:0];
          if (off == sysrst_ctrl_reg_pkg::SYSRST_CTRL_COM_PRE_DET_CTL_0_OFFSET + 4 * i) com_pre_det[i] = d;
          if (off == sysrst_ctrl_reg_pkg::SYSRST_CTRL_COM_SEL_CTL_0_OFFSET + 4 * i) begin
            com_sel[i] = d[4:0];
            // Keys already pressed when the combo is configured are not an edge.
            prev_trig_low[i] = (inv_keys(sample_in()) & com_sel[i]) == 0;
            trig_counting[i] = 1'b0;
          end
          if (off == sysrst_ctrl_reg_pkg::SYSRST_CTRL_COM_DET_CTL_0_OFFSET + 4 * i) com_det[i] = d;
          if (off == sysrst_ctrl_reg_pkg::SYSRST_CTRL_COM_OUT_CTL_0_OFFSET + 4 * i) com_out[i] = d[3:0];
        end
      end
    endcase
  endfunction

  protected function void w1c(string n, bit [31:0] bits);
    set_st(n, get_st(n) & ~bits);
    clear_maybe(n, bits);
  endfunction

  virtual function void tl_read(bit [31:0] off, tl_seq_item t);
    case (off)
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_KEY_INTR_STATUS_OFFSET:
        read_st("KEY_INTR_STATUS", t.d_data, 32'h3fff);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_COMBO_INTR_STATUS_OFFSET:
        read_st("COMBO_INTR_STATUS", t.d_data, 32'hf);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_ULP_STATUS_OFFSET: read_st("ULP_STATUS", t.d_data, 32'h1);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_WKUP_STATUS_OFFSET: read_st("WKUP_STATUS", t.d_data, 32'h1);
      sysrst_ctrl_reg_pkg::SYSRST_CTRL_PIN_IN_VALUE_OFFSET: begin
        // PIN_IN_VALUE: pwrb, key0, key1, key2, lid_open, ac_present, ec_rst_l, flash_wp_l; the
        // raw inputs, synchronised: an input that changed in the last cycles is not compared.
        bit [7:0]  in = sample_in();
        bit [31:0] e = {in[7], in[6], in[4], in[5], in[3], in[2], in[1], in[0]};
        bit [31:0] c = '1;
        for (int b = 0; b < 8; b++) begin
          int src = b == 4 ? 5 : b == 5 ? 4 : b;
          if (cycle - last_chg[src] <= race_cycles) c[b] = 1'b0;
        end
        read_check("PIN_IN_VALUE", t.d_data, 32'hff, e, c);
      end
      default: ;
    endcase
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// adc_ctrl (hw/ip/adc_ctrl: README.md, doc/theory_of_operation.md "Filters and debounce",
// doc/registers.md). The filter and debounce model is ported from OpenTitan's block scoreboard
// hw/ip/adc_ctrl/dv/env/adc_ctrl_scoreboard.sv (process_filter_data, process_debounce,
// process_np_debounce, process_lp_debounce, the adc_en_ctl / adc_fsm_rst / filter_status /
// adc_intr_status / adc_wakeup_ctl updates and the interrupt and wake-up line equations), driven by
// the samples the AST ADC returns on adc_i (data_valid rising, the channel adc_o.channel_sel
// selects) instead of that bench's push-pull agent.
//
// Item: bit 0 the interrupt line (match_pending), bit 1 the wake-up request.
// Expected: intr = (INTR_TEST || |ADC_INTR_STATUS) && INTR_ENABLE; wkup = (FILTER_STATUS.TRANS &&
// ADC_WAKEUP_CTL.TRANS_EN) || |(FILTER_STATUS.MATCH & ADC_WAKEUP_CTL.MATCH_EN).
// RX: ADC_INTR_STATUS, FILTER_STATUS, ADC_CHN_VAL_0/1 (the latest value and the value at the first
// debounced match), INTR_STATE.
// Not predicted: a read within `race_cycles` of a sample that changes it; the ADC_FSM_STATE
// register.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_adc_ctrl_monitor extends eg2_sysmon_monitor;
  `uvm_component_utils(eg2_adc_ctrl_monitor)

  localparam int unsigned NFilters = 8;

  // Registers.
  protected bit        intr_enable, intr_test;
  protected bit        adc_en, oneshot;
  protected bit        lp_mode_cfg;
  protected bit [7:0]  lp_sample_cnt = 8'h4;
  protected bit [15:0] np_sample_cnt = 16'h9b;
  protected bit        fsm_rst;
  protected bit [9:0]  f_min[2][NFilters], f_max[2][NFilters];
  protected bit        f_cond[2][NFilters], f_en[2][NFilters];
  protected bit [8:0]  wakeup_ctl;
  protected bit [9:0]  intr_ctl;

  // Model (OpenTitan's names).
  protected bit [9:0]  latest[2], intr_val[2];
  protected bit [NFilters-1:0] chn_match[2], match, match_prev;
  protected int unsigned np_counter, lp_counter;
  protected bit        debounced, lp_mode, lp_to_np;
  protected bit [NFilters-1:0] filter_status;
  protected bit [NFilters+1:0] adc_intr_status;
  protected bit        prev_valid;
  eg2_adc_cg           adc_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    state_name = "lines";
    state_fields = "(intr wkup = bits 0..1)";
    state_mask = 32'h3;
    tap = "adc_ctrl";
    tap_size = 32'h80;
  endfunction

  function void create_cov(string path);
    adc_cg = new({path, "::eg2_adc_cg"});
  endfunction

  virtual task wait_aon();
    @(posedge vif.adc_clk_aon);
  endtask

  virtual function bit in_reset();
    return vif.adc_rst_aon_n !== 1'b1 || vif.adc_rst_n !== 1'b1;
  endfunction

  virtual task wait_reset_on();
    wait (vif.adc_rst_aon_n !== 1'b1 || vif.adc_rst_n !== 1'b1);
  endtask

  virtual task wait_reset_off();
    wait (vif.adc_rst_aon_n === 1'b1 && vif.adc_rst_n === 1'b1);
  endtask

  virtual function bit [31:0] sample_outputs();
    return {vif.adc_wkup_req === 1'b1, vif.adc_intr === 1'b1};
  endfunction

  virtual task watch_outputs();
    forever begin
      @(vif.adc_wkup_req or vif.adc_intr);
      if (!in_reset()) set_act(sample_outputs());
    end
  endtask

  virtual function void model_reset();
    intr_enable = 1'b0;
    intr_test = 1'b0;
    adc_en = 1'b0;
    oneshot = 1'b0;
    lp_mode_cfg = 1'b0;
    lp_sample_cnt = 8'h4;
    np_sample_cnt = 16'h9b;
    fsm_rst = 1'b0;
    foreach (f_min[c, f]) begin
      f_min[c][f] = '0;
      f_max[c][f] = '0;
      f_cond[c][f] = 1'b0;
      f_en[c][f] = 1'b0;
    end
    wakeup_ctl = '0;
    intr_ctl = '0;
    latest = '{default: '0};
    intr_val = '{default: '0};
    chn_match = '{default: '0};
    match = '0;
    match_prev = '0;
    np_counter = 0;
    lp_counter = 0;
    debounced = 1'b0;
    lp_mode = 1'b0;
    lp_to_np = 1'b0;
    filter_status = '0;
    adc_intr_status = '0;
    prev_valid = 1'b0;
    publish();
  endfunction

  protected function void publish();
    set_st("ADC_INTR_STATUS", adc_intr_status);
    set_st("FILTER_STATUS", {lp_to_np, filter_status});
    set_st("ADC_CHN_VAL_0", {intr_val[0], 8'h0, latest[0], 2'b00});
    set_st("ADC_CHN_VAL_1", {intr_val[1], 8'h0, latest[1], 2'b00});
    set_st("INTR_STATE", intr_test || |adc_intr_status);
  endfunction

  // adc_ctrl_scoreboard::process_filter_data.
  protected function void filter_data(int channel, bit [9:0] val, bit call_debounce = 1'b1);
    for (int f = 0; f < NFilters; f++) begin
      bit en_somewhere = 1'b0, mismatch_somewhere = 1'b0;
      bit inside_range = val >= f_min[channel][f] && val <= f_max[channel][f];
      chn_match[channel][f] = f_cond[channel][f] ^ inside_range;
      for (int c = 0; c < 2; c++) begin
        if (f_en[c][f]) begin
          en_somewhere = 1'b1;
          mismatch_somewhere |= ~chn_match[c][f];
        end
      end
      match[f] = en_somewhere & ~mismatch_somewhere;
    end
    if (call_debounce && channel == 1) debounce();
  endfunction

  // adc_ctrl_scoreboard::process_debounce.
  protected function void debounce();
    if (!oneshot) begin
      if (!lp_mode) np_debounce();
      else lp_debounce();
    end else begin
      adc_intr_status[NFilters + 1] |= intr_ctl[NFilters + 1];
      if (adc_cg != null) adc_cg.sample(2, 0, 0);
    end
    match_prev = match;
  endfunction

  // adc_ctrl_scoreboard::process_np_debounce.
  protected function void np_debounce();
    if (match != 0 && !fsm_rst) begin
      if (match == match_prev || match_prev == 0) begin
        if (np_counter < np_sample_cnt) begin
          np_counter++;
          if (np_counter == np_sample_cnt && !debounced) begin
            debounced = 1'b1;
            filter_status |= match;
            adc_intr_status[NFilters-1:0] |= match & intr_ctl[NFilters-1:0];
            intr_val = latest;
            if (adc_cg != null) adc_cg.sample(lp_mode_cfg, $countones(match), 0);
          end
        end
      end else begin
        np_counter = 0;
        debounced = 1'b0;
        if (lp_mode_cfg) lp_mode = 1'b1;
      end
    end else begin
      np_counter = 0;
      debounced = 1'b0;
      if (lp_mode_cfg) lp_mode = 1'b1;
    end
  endfunction

  // adc_ctrl_scoreboard::process_lp_debounce.
  protected function void lp_debounce();
    if (match != 0 && !fsm_rst) begin
      if (match == match_prev || match_prev == 0) begin
        if (lp_counter < lp_sample_cnt) begin
          lp_counter++;
          if (lp_counter == lp_sample_cnt) begin
            lp_mode = 1'b0;
            lp_to_np = 1'b1;
            adc_intr_status[NFilters] |= intr_ctl[NFilters];
            lp_counter = 0;
            if (adc_cg != null) adc_cg.sample(1, $countones(match), 1);
          end
        end
      end else begin
        lp_counter = 0;
      end
    end else begin
      lp_counter = 0;
    end
  endfunction

  // A sample from the AST ADC: data_valid rising, on the selected channel.
  virtual function void step();
    bit valid = vif.adc_data_valid === 1'b1;
    if (valid && !prev_valid && !$isunknown(vif.adc_data)) begin
      int ch = -1;
      if (vif.adc_channel_sel === 2'b01) ch = 0;
      else if (vif.adc_channel_sel === 2'b10) ch = 1;
      if (ch >= 0) begin
        latest[ch] = vif.adc_data;
        if (adc_en) filter_data(ch, vif.adc_data);
        n_events++;
      end
    end
    prev_valid = valid;
    publish();
  endfunction

  virtual function void refresh();
    bit intr = (intr_test || |adc_intr_status) && intr_enable;
    bit wkup = (lp_to_np && wakeup_ctl[NFilters]) || |(filter_status & wakeup_ctl[NFilters-1:0]);
    set_exp({wkup, intr});
  endfunction

  virtual function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    case (off)
      adc_ctrl_reg_pkg::ADC_CTRL_INTR_ENABLE_OFFSET: intr_enable = d[0];
      adc_ctrl_reg_pkg::ADC_CTRL_INTR_TEST_OFFSET: intr_test = d[0];
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_EN_CTL_OFFSET: begin
        adc_en = d[0];
        oneshot = d[1];
        if (adc_en) begin
          lp_mode = oneshot ? 1'b0 : lp_mode_cfg;
          for (int c = 0; c < 2; c++) filter_data(c, latest[c], 1'b0);
          match_prev = '0;
        end else begin
          lp_counter = 0;
          np_counter = 0;
          debounced = 1'b0;
          lp_mode = 1'b0;
        end
      end
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_PD_CTL_OFFSET: lp_mode_cfg = d[0];
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_LP_SAMPLE_CTL_OFFSET: lp_sample_cnt = d[7:0];
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_SAMPLE_CTL_OFFSET: np_sample_cnt = d[15:0];
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_FSM_RST_OFFSET: begin
        fsm_rst = d[0];
        if (fsm_rst) begin
          np_counter = 0;
          lp_counter = 0;
          debounced = 1'b0;
          lp_mode = 1'b0;
        end
      end
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_WAKEUP_CTL_OFFSET: wakeup_ctl = d[8:0];
      adc_ctrl_reg_pkg::ADC_CTRL_FILTER_STATUS_OFFSET: begin
        filter_status &= ~d[NFilters-1:0];
        if (d[NFilters]) lp_to_np = 1'b0;
      end
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_INTR_CTL_OFFSET: intr_ctl = d[9:0];
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_INTR_STATUS_OFFSET: adc_intr_status &= ~d[9:0];
      default: begin
        for (int f = 0; f < NFilters; f++) begin
          for (int c = 0; c < 2; c++) begin
            bit [31:0] o = (c == 0 ? adc_ctrl_reg_pkg::ADC_CTRL_ADC_CHN0_FILTER_CTL_0_OFFSET :
                                     adc_ctrl_reg_pkg::ADC_CTRL_ADC_CHN1_FILTER_CTL_0_OFFSET) + 4 * f;
            if (off == o) begin
              f_min[c][f]  = d[11:2];
              f_cond[c][f] = d[12];
              f_max[c][f]  = d[27:18];
              f_en[c][f]   = d[31];
            end
          end
        end
      end
    endcase
    publish();
  endfunction

  virtual function void tl_read(bit [31:0] off, tl_seq_item t);
    case (off)
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_INTR_STATUS_OFFSET: read_st("ADC_INTR_STATUS", t.d_data, 32'h3ff);
      adc_ctrl_reg_pkg::ADC_CTRL_FILTER_STATUS_OFFSET: read_st("FILTER_STATUS", t.d_data, 32'h1ff);
      // In oneshot mode the value at the interrupt is the latest one (OpenTitan's scoreboard).
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_CHN_VAL_0_OFFSET:
        read_check("ADC_CHN_VAL_0", t.d_data, 32'h0ffc_0ffc,
                   oneshot ? {latest[0], 8'h0, latest[0], 2'b00} : get_st("ADC_CHN_VAL_0"));
      adc_ctrl_reg_pkg::ADC_CTRL_ADC_CHN_VAL_1_OFFSET:
        read_check("ADC_CHN_VAL_1", t.d_data, 32'h0ffc_0ffc,
                   oneshot ? {latest[1], 8'h0, latest[1], 2'b00} : get_st("ADC_CHN_VAL_1"));
      adc_ctrl_reg_pkg::ADC_CTRL_INTR_STATE_OFFSET: read_st("INTR_STATE", t.d_data, 32'h1);
      default: ;
    endcase
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// sensor_ctrl (hw/top_earlgrey/ip/sensor_ctrl: README.md, doc/theory_of_operation.md,
// doc/programmers_guide.md, doc/registers.md). No OpenTitan block scoreboard exists for it.
//
// An AST event is active while either line of its pair is active (p high or n low). An enabled
// event (ALERT_EN_<i> = MuBi4True, the reset value 0x6) is recoverable or fatal by
// FATAL_ALERT_EN<i>: recoverable sets RECOV_ALERT<i> and is acknowledged to the AST; fatal sets
// FATAL_ALERT<i> (sticky) and is not acknowledged. Each becomes an alert to the alert handler.
// Item (events): `recov_alert` and `fatal_alert`, one item per episode: the alert agent's first
// alert of that class after `quiet_cycles` without one (actual), and an enabled event of that class
// becoming active, or an ALERT_TEST write, after `quiet_cycles` without one (expected).
// RX: RECOV_ALERT, FATAL_ALERT (bits 10:0), STATUS (ast_init_done, io_pok from the inputs),
// INTR_STATE (io_status_change when io_pok changes, init_status_change when ast_init_done does).
// Not predicted: FATAL_ALERT bit 11 and the alerts of the block's own faults; the wake-up request
// (the documentation leaves open which events aggregate into it); a read within `race_cycles`.
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_sensor_ctrl_monitor extends eg2_sysmon_monitor;
  `uvm_component_utils(eg2_sensor_ctrl_monitor)

  localparam int unsigned NEvents = 11;

  uvm_tlm_analysis_fifo #(alert_agent_pkg::alert_seq_item) alert_fifo[2];  // recov, fatal
  int unsigned quiet_cycles = 20;

  protected bit [3:0]  alert_en[NEvents];
  protected bit [NEvents-1:0] fatal_en, prev_active;
  protected bit        cfg_regwen = 1'b1;
  protected bit [1:0]  prev_io_pok;
  protected bit [3:0]  prev_init;
  protected realtime   last_act[2], last_exp[2];
  protected bit        seen_act[2], seen_exp[2];
  eg2_sensor_cg        sensor_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    state_name = "alerts";
    state_mask = '0;
    has_state = 1'b0;
    tap = "sensor_ctrl";
    tap_size = 32'h80;
    foreach (alert_fifo[i]) alert_fifo[i] = new($sformatf("alert_fifo%0d", i), this);
  endfunction

  function void create_cov(string path);
    sensor_cg = new({path, "::eg2_sensor_cg"});
  endfunction

  virtual task wait_aon();
    @(posedge vif.sensor_clk_aon);
  endtask

  virtual function bit in_reset();
    return vif.sensor_rst_aon_n !== 1'b1 || vif.sensor_rst_n !== 1'b1;
  endfunction

  virtual task wait_reset_on();
    wait (vif.sensor_rst_aon_n !== 1'b1 || vif.sensor_rst_n !== 1'b1);
  endtask

  virtual task wait_reset_off();
    wait (vif.sensor_rst_aon_n === 1'b1 && vif.sensor_rst_n === 1'b1);
  endtask

  // The alert agents' items: the first alert of a class after a quiet time is one actual item.
  virtual task watch_outputs();
    for (int i = 0; i < 2; i++) begin
      automatic int k = i;
      fork
        forever begin
          alert_agent_pkg::alert_seq_item a;
          alert_fifo[k].get(a);
          if (a.m_trans_type != alert_agent_pkg::AlertSigTrans) continue;
          if (a.m_alert_handshake_sta != alert_agent_pkg::AlertReceived) continue;
          if (!seen_act[k] || $realtime - last_act[k] > quiet_cycles * aon_period) begin
            pin_port.write(event_item(k, ""));
          end
          seen_act[k] = 1'b1;
          last_act[k] = $realtime;
        end
      join_none
    end
  endtask

  protected function eg2_sysmon_item event_item(int k, string why);
    eg2_sysmon_item it = eg2_sysmon_item::type_id::create("event");
    it.kind = eg2_sysmon_item::SysEvent;
    it.name = k == 0 ? "recov_alert" : "fatal_alert";
    it.why  = why;
    return it;
  endfunction

  protected function void expect_alert(int k, string why);
    if (!seen_exp[k] || $realtime - last_exp[k] > quiet_cycles * aon_period) begin
      tl_exp_port.write(event_item(k, why));
      n_events++;
    end
    seen_exp[k] = 1'b1;
    last_exp[k] = $realtime;
  endfunction

  virtual function void model_reset();
    foreach (alert_en[i]) alert_en[i] = prim_mubi_pkg::MuBi4True;
    fatal_en = '0;
    prev_active = '0;
    cfg_regwen = 1'b1;
    prev_io_pok = vif.sensor_io_pok;
    prev_init = vif.sensor_init_done;
    seen_exp = '{default: 1'b0};
    seen_act = '{default: 1'b0};
  endfunction

  virtual function void step();
    bit [NEvents-1:0] active;
    for (int i = 0; i < NEvents; i++) begin
      active[i] = vif.sensor_alert_p[i] === 1'b1 || vif.sensor_alert_n[i] === 1'b0;
    end
    for (int i = 0; i < NEvents; i++) begin
      if (active[i] && alert_en[i] == prim_mubi_pkg::MuBi4True) begin
        if (fatal_en[i]) begin
          set_st("FATAL_ALERT", get_st("FATAL_ALERT") | (1 << i));
          // Fatal: never acknowledged, the alert repeats; one episode while it lasts.
          expect_alert(1, $sformatf("event %0d, fatal", i));
        end else begin
          set_st("RECOV_ALERT", get_st("RECOV_ALERT") | (1 << i));
          if (!prev_active[i]) expect_alert(0, $sformatf("event %0d, recoverable", i));
        end
        if (!prev_active[i] && sensor_cg != null) sensor_cg.sample(i, fatal_en[i], 1);
      end else if (active[i] && !prev_active[i] && sensor_cg != null) begin
        sensor_cg.sample(i, fatal_en[i], 0);
      end
    end
    prev_active = active;
    // Status changes raise the event interrupts.
    begin
      bit [1:0] pok  = vif.sensor_io_pok;
      bit [3:0] init = vif.sensor_init_done;
      if (pok != prev_io_pok) set_st("INTR_STATE", get_st("INTR_STATE") | 1);
      if (init != prev_init) set_st("INTR_STATE", get_st("INTR_STATE") | 2);
      prev_io_pok = pok;
      prev_init = init;
      set_st("STATUS", {pok, init == prim_mubi_pkg::MuBi4True});
    end
  endfunction

  virtual function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    case (off)
      sensor_ctrl_reg_pkg::SENSOR_CTRL_CFG_REGWEN_OFFSET: if (!d[0]) cfg_regwen = 1'b0;
      sensor_ctrl_reg_pkg::SENSOR_CTRL_FATAL_ALERT_EN_OFFSET: if (cfg_regwen) fatal_en = d[NEvents-1:0];
      sensor_ctrl_reg_pkg::SENSOR_CTRL_RECOV_ALERT_OFFSET: set_st("RECOV_ALERT", get_st("RECOV_ALERT") & ~d[NEvents-1:0]);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_INTR_STATE_OFFSET: set_st("INTR_STATE", get_st("INTR_STATE") & ~d[1:0]);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_INTR_TEST_OFFSET: set_st("INTR_STATE", get_st("INTR_STATE") | d[1:0]);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_ALERT_TEST_OFFSET: begin
        if (d[0]) expect_alert(0, "ALERT_TEST");
        if (d[1]) expect_alert(1, "ALERT_TEST");
      end
      default: begin
        for (int i = 0; i < NEvents; i++) begin
          if (off == sensor_ctrl_reg_pkg::SENSOR_CTRL_ALERT_EN_0_OFFSET + 4 * i && cfg_regwen) begin
            alert_en[i] = d[3:0];
          end
        end
      end
    endcase
  endfunction

  virtual function void tl_read(bit [31:0] off, tl_seq_item t);
    case (off)
      sensor_ctrl_reg_pkg::SENSOR_CTRL_RECOV_ALERT_OFFSET: read_st("RECOV_ALERT", t.d_data, 32'h7ff);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_FATAL_ALERT_OFFSET: read_st("FATAL_ALERT", t.d_data, 32'h7ff);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_STATUS_OFFSET: read_st("STATUS", t.d_data, 32'h7);
      sensor_ctrl_reg_pkg::SENSOR_CTRL_INTR_STATE_OFFSET: read_st("INTR_STATE", t.d_data, 32'h3);
      default: ;
    endcase
  endfunction
endclass

//////////////////////////////////////////////////////////////////////////////////////////////////
// The wake-up pads: pinmux's wake-up detectors (hw/top_earlgrey/ip_autogen/pinmux:
// doc/theory_of_operation.md "Wakeup Detectors", doc/registers.md WKUP_DETECTOR*, WKUP_CAUSE) and
// the wake reason pwrmgr records (ip_autogen/pwrmgr: doc/theory_of_operation.md "Wakeup
// Recording", doc/registers.md WAKEUP_EN, WAKE_INFO, WAKE_INFO_CAPTURE_DIS). No OpenTitan block
// scoreboard predicts them at chip level.
//
// Each enabled detector (WKUP_DETECTOR_EN_<i>) watches the pad WKUP_DETECTOR_PADSEL_<i> selects
// (MIO: 0 and 1 the constants, the pads from 2; DIO: the pad itself), sampled on the always-on
// clock, through the 4-cycle filter if FILTER is set, for its MODE: Posedge, Negedge, Edge,
// TimedHigh / TimedLow (the level held for WKUP_DETECTOR_CNT_TH_<i> cycles); out-of-range modes
// are Posedge. A detection sets WKUP_CAUSE<i> (cleared by writing 0) and requests a wake-up.
// Item: bit 0 pinmux's wake-up request to pwrmgr (wakeup input 2), expected |WKUP_CAUSE.
// RX: WKUP_CAUSE (pinmux); pwrmgr WAKE_INFO.REASONS[2]: 1 if the request was high while the chip
// was in low power with WAKEUP_EN[2] set and capture not disabled, 0 if it was never high since
// WAKE_INFO was last cleared, not predicted otherwise; the other reasons are not this block's.
// Not predicted: a detection within `race_cycles` of an enable, a mode change or a timed threshold
// (the cause bit and the request are then not compared until cleared).
//////////////////////////////////////////////////////////////////////////////////////////////////
class eg2_wkup_monitor extends eg2_sysmon_monitor;
  `uvm_component_utils(eg2_wkup_monitor)

  localparam int unsigned NDet = 8;

  uvm_tlm_analysis_fifo #(tl_seq_item) pwrmgr_fifo;

  protected bit [NDet-1:0] det_en, det_filter, det_miodio, det_regwen;
  protected bit [2:0]  det_mode[NDet];
  protected bit [7:0]  det_th[NDet];
  protected bit [5:0]  det_sel[NDet];
  protected longint unsigned det_cfg_cycle[NDet];
  protected bit        raw_prev[NDet], filt[NDet], filt_prev[NDet];
  protected int unsigned stable_cnt[NDet], level_cnt[NDet];
  protected bit [NDet-1:0] cause;
  // pwrmgr
  protected bit [5:0]  wakeup_en;
  protected bit        capture_dis;
  protected bit        req_seen, req_in_lp, req_unsure;
  eg2_wkup_cg          wkup_cg;

  `uvm_component_new

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    state_name = "pin_wkup_req";
    state_mask = 32'h1;
    tap = "pinmux";
    tap_size = 32'h1000;
    pwrmgr_fifo = new("pwrmgr_fifo", this);
  endfunction

  function void create_cov(string path);
    wkup_cg = new({path, "::eg2_wkup_cg"});
  endfunction

  virtual task wait_aon();
    @(posedge vif.pinmux_clk_aon);
  endtask

  virtual function bit in_reset();
    return vif.pinmux_rst_aon_n !== 1'b1;
  endfunction

  virtual task wait_reset_on();
    wait (vif.pinmux_rst_aon_n !== 1'b1);
  endtask

  virtual task wait_reset_off();
    wait (vif.pinmux_rst_aon_n === 1'b1);
  endtask

  virtual function bit [31:0] sample_outputs();
    return vif.pin_wkup_req === 1'b1;
  endfunction

  virtual task watch_outputs();
    fork
      forever begin
        @(vif.pin_wkup_req);
        if (!in_reset()) set_act(sample_outputs());
      end
      pwrmgr_thread();
    join
  endtask

  virtual function void model_reset();
    det_en = '0;
    det_filter = '0;
    det_miodio = '0;
    det_regwen = '1;
    foreach (det_mode[i]) begin
      det_mode[i] = '0;
      det_th[i] = '0;
      det_sel[i] = '0;
      det_cfg_cycle[i] = 0;
      raw_prev[i] = 1'b0;
      filt[i] = 1'b0;
      filt_prev[i] = 1'b0;
      stable_cnt[i] = 0;
      level_cnt[i] = 0;
    end
    cause = '0;
    wakeup_en = '0;
    capture_dis = 1'b0;
    req_seen = 1'b0;
    req_in_lp = 1'b0;
    req_unsure = 1'b0;
  endfunction

  protected function bit pad(int i);
    if (det_miodio[i]) return det_sel[i] < pinmux_reg_pkg::NDioPads ? vif.dio_in[det_sel[i]] === 1'b1 : 1'b0;
    if (det_sel[i] == 0) return 1'b0;
    if (det_sel[i] == 1) return 1'b1;
    return det_sel[i] - 2 < pinmux_reg_pkg::NMioPads ? vif.mio_in[det_sel[i] - 2] === 1'b1 : 1'b0;
  endfunction

  protected function void detect(int i, bit unsure);
    cause[i] = 1'b1;
    if (unsure) set_maybe("WKUP_CAUSE", 1 << i);
    if (wkup_cg != null) wkup_cg.sample(det_mode[i] > 4 ? 0 : det_mode[i], det_filter[i], det_miodio[i]);
  endfunction

  virtual function void step();
    for (int i = 0; i < NDet; i++) begin
      bit raw = pad(i);
      bit recent = cycle - det_cfg_cycle[i] <= race_cycles;
      bit [2:0] mode = det_mode[i] > 4 ? 3'd0 : det_mode[i];
      if (!det_en[i]) begin
        raw_prev[i] = raw;
        filt[i] = raw;
        filt_prev[i] = raw;
        stable_cnt[i] = 0;
        level_cnt[i] = 0;
        continue;
      end
      // The filter forwards a level once it has been stable for 4 cycles. A level that lasted
      // about that long, or a one-cycle pulse without the filter, may or may not get through.
      if (raw == raw_prev[i]) begin
        stable_cnt[i]++;
      end else begin
        if (det_filter[i] && stable_cnt[i] >= 2 && stable_cnt[i] <= 4 && mode != 3'd3 &&
            mode != 3'd4) begin
          set_maybe("WKUP_CAUSE", 1 << i);
        end
        if (!det_filter[i] && stable_cnt[i] == 0) set_maybe("WKUP_CAUSE", 1 << i);
        stable_cnt[i] = 0;
      end
      if (!det_filter[i]) filt[i] = raw;
      else if (stable_cnt[i] >= 3) filt[i] = raw;
      case (mode)
        3'd0: if (!filt_prev[i] && filt[i]) detect(i, recent);
        3'd1: if (filt_prev[i] && !filt[i]) detect(i, recent);
        3'd2: if (filt_prev[i] != filt[i]) detect(i, recent);
        3'd3, 3'd4: begin
          bit want = mode == 3'd3;
          if (filt[i] == want) begin
            level_cnt[i]++;
            if (level_cnt[i] == det_th[i]) detect(i, recent || det_th[i] <= 2);
          end else begin
            // A level that ended just short of the threshold may have been counted.
            if (level_cnt[i] > 0 && level_cnt[i] < det_th[i] && level_cnt[i] + 2 >= det_th[i]) begin
              set_maybe("WKUP_CAUSE", 1 << i);
            end
            level_cnt[i] = 0;
          end
        end
        default: ;
      endcase
      raw_prev[i] = raw;
      filt_prev[i] = filt[i];
    end
    publish();
  endfunction

  // The request is certainly high when a cause bit the model is sure of is set.
  protected function bit [NDet-1:0] maybe_bits();
    return st_maybe.exists("WKUP_CAUSE") ? st_maybe["WKUP_CAUSE"][NDet-1:0] : '0;
  endfunction

  protected function void publish();
    bit [NDet-1:0] maybe = maybe_bits();
    set_st("WKUP_CAUSE", cause);
    if (|cause) req_seen = 1'b1;
    if (|maybe) req_unsure = 1'b1;
    if (|(cause & ~maybe) && vif.pwrmgr_low_power === 1'b1 && wakeup_en[2] && !capture_dis) begin
      req_in_lp = 1'b1;
    end
  endfunction

  virtual function void refresh();
    bit [NDet-1:0] maybe = maybe_bits();
    set_care((maybe == 0 || |(cause & ~maybe)) ? 32'h1 : 32'h0);
    set_exp(|cause);
  endfunction

  virtual function void tl_write(bit [31:0] off, tl_seq_item t);
    bit [31:0] d = t.a_data;
    for (int i = 0; i < NDet; i++) begin
      if (off == pinmux_reg_pkg::PINMUX_WKUP_DETECTOR_REGWEN_0_OFFSET + 4 * i && !d[0]) det_regwen[i] = 1'b0;
      if (!det_regwen[i]) continue;
      if (off == pinmux_reg_pkg::PINMUX_WKUP_DETECTOR_EN_0_OFFSET + 4 * i) begin
        det_en[i] = d[0];
        det_cfg_cycle[i] = cycle;
      end
      if (off == pinmux_reg_pkg::PINMUX_WKUP_DETECTOR_0_OFFSET + 4 * i) begin
        det_mode[i] = d[2:0];
        det_filter[i] = d[3];
        det_miodio[i] = d[4];
        det_cfg_cycle[i] = cycle;
      end
      if (off == pinmux_reg_pkg::PINMUX_WKUP_DETECTOR_CNT_TH_0_OFFSET + 4 * i) det_th[i] = d[7:0];
      if (off == pinmux_reg_pkg::PINMUX_WKUP_DETECTOR_PADSEL_0_OFFSET + 4 * i) begin
        det_sel[i] = d[5:0];
        det_cfg_cycle[i] = cycle;
      end
    end
    if (off == pinmux_reg_pkg::PINMUX_WKUP_CAUSE_OFFSET) begin
      // rw0c: a 0 clears the cause.
      cause &= d[NDet-1:0];
      clear_maybe("WKUP_CAUSE", ~d[NDet-1:0]);
      publish();
    end
  endfunction

  virtual function void tl_read(bit [31:0] off, tl_seq_item t);
    if (off == pinmux_reg_pkg::PINMUX_WKUP_CAUSE_OFFSET) read_st("WKUP_CAUSE", t.d_data, 32'hff);
  endfunction

  // pwrmgr's port: WAKEUP_EN, WAKE_INFO_CAPTURE_DIS, WAKE_INFO.
  protected task pwrmgr_thread();
    forever begin
      tl_seq_item t;
      bit [31:0]  off;
      pwrmgr_fifo.get(t);
      if (t.d_error || eg2_tl_err::refused("pwrmgr", t) || in_reset()) continue;
      off = eg2_tl_offset(t, 32'h80);
      if (t.is_write()) begin
        case (off)
          pwrmgr_reg_pkg::PWRMGR_WAKEUP_EN_OFFSET: wakeup_en = t.a_data[5:0];
          pwrmgr_reg_pkg::PWRMGR_WAKE_INFO_CAPTURE_DIS_OFFSET: capture_dis = t.a_data[0];
          pwrmgr_reg_pkg::PWRMGR_WAKE_INFO_OFFSET: if (t.a_data[2]) begin
            // Cleared: what counts from here is whether the request is (still) high.
            req_seen = |cause;
            req_in_lp = 1'b0;
            req_unsure = |maybe_bits();
          end
          default: ;
        endcase
      end else if (off == pwrmgr_reg_pkg::PWRMGR_WAKE_INFO_OFFSET) begin
        bit [31:0] e = '0, c = '0;
        if (!req_seen && !req_unsure) begin
          c[2] = 1'b1;
        end else if (req_in_lp && !req_unsure) begin
          c[2] = 1'b1;
          e[2] = 1'b1;
        end
        read_check("WAKE_INFO", t.d_data, 32'h4, e, c);
      end
    end
  endtask
endclass
