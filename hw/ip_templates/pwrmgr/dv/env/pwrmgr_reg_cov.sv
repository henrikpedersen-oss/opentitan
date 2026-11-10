// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of pwrmgr's registers, field by field, and of the power transitions as the
// block's ports show them. One covergroup per register or register group, each reported per
// instance. Sampled by pwrmgr_cmodel_sb (on with +pwrmgr_cmodel=1 and coverage enabled), which sees
// every TL-UL access and the block's ports; nothing inside pwrmgr is sampled.
//
// Field positions are pwrmgr.hjson's (top_earlgrey: 8 wake-ups, 2 reset requests). The transitions
// are those of theory_of_operation.md ("Overall Sequencing", "Reset Request Handling", "Fall
// Through Handling", "Abort Handling"), seen at the ports: the fast FSM leaving and re-entering
// Active (fetch_en_o), the low-power trigger (core_sleeping with the hint), the wake-up and reset
// requests that end a low-power state.
class pwrmgr_reg_cov extends uvm_object;
  `uvm_object_utils(pwrmgr_reg_cov)

  // How the fast FSM left Active, at fetch_en_o's fall.
  typedef enum bit [1:0] { LeaveLowPower = 0, LeaveReset = 1, LeaveOther = 2 } leave_e;
  // How a low-power entry ended, at fetch_en_o's rise (or settled in Active).
  typedef enum bit [2:0] {
    EndWakeup = 0, EndResetReq = 1, EndSwReset = 2, EndEscalation = 3, EndNoCause = 4,
    EndNeverLeft = 5
  } end_e;
  typedef enum bit [2:0] { ReqHw0 = 0, ReqHw1 = 1, ReqSw = 2, ReqEsc = 3, ReqGlitch = 4 } req_e;

  // ---- Context ----
  protected bit [8:0] control_w = 9'h180;  // the last value written to CONTROL
  protected bit [7:0] wakeup_en_w;
  protected bit [1:0] reset_en_w;
  protected bit       capture_dis_w;
  protected bit       in_entry;            // a low-power trigger seen, Active not yet re-entered
  protected bit       entry_deep, entry_idle;
  protected bit       left_active;
  protected bit [2:0] cause;               // what was seen during the entry (end_e)
  protected bit       cause_seen;

  // ---- Register writes ----
  covergroup control_cg with function sample(bit [8:0] v, bit regwen);
    option.per_instance = 1;
    cp_hint:       coverpoint v[0];
    cp_core_clk:   coverpoint v[4];
    cp_io_clk:     coverpoint v[5];
    cp_usb_lp:     coverpoint v[6];
    cp_usb_active: coverpoint v[7];
    cp_main_pd_n:  coverpoint v[8];
    cp_regwen:     coverpoint regwen;  // write taken / refused (CTRL_CFG_REGWEN)
    x_hint_pd:     cross cp_hint, cp_main_pd_n;
    x_hint_clks:   cross cp_hint, cp_core_clk, cp_io_clk;
    x_usb:         cross cp_usb_lp, cp_usb_active;
    x_regwen_hint: cross cp_regwen, cp_hint;
  endgroup

  covergroup wakeup_en_cg with function sample(int unsigned bit_i, bit v, bit locked);
    option.per_instance = 1;
    cp_bit:    coverpoint bit_i { bins b[] = {[0:7]}; }
    cp_v:      coverpoint v;
    cp_locked: coverpoint locked;
    x_bit_v:   cross cp_bit, cp_v;
    x_locked:  cross cp_locked, cp_v;
  endgroup

  covergroup reset_en_cg with function sample(int unsigned bit_i, bit v, bit locked);
    option.per_instance = 1;
    cp_bit:    coverpoint bit_i { bins b[] = {[0:1]}; }
    cp_v:      coverpoint v;
    cp_locked: coverpoint locked;
    x_bit_v:   cross cp_bit, cp_v;
    x_locked:  cross cp_locked, cp_v;
  endgroup

  covergroup misc_write_cg with function sample(bit [6:0] addr, bit [31:0] v);
    option.per_instance = 1;
    cp_intr_enable:      coverpoint v[0] iff (addr == 'h04);
    cp_intr_state_w1c:   coverpoint v[0] iff (addr == 'h00);
    cp_intr_test:        coverpoint v[0] iff (addr == 'h08);
    cp_cdc_sync:         coverpoint v[0] iff (addr == 'h18);
    cp_wakeup_regwen:    coverpoint v[0] iff (addr == 'h1c);
    cp_reset_regwen:     coverpoint v[0] iff (addr == 'h28);
    cp_capture_dis:      coverpoint v[0] iff (addr == 'h38);
    cp_wake_info_w1c:    coverpoint v[9:8] iff (addr == 'h3c);
    cp_wake_info_rsn:    coverpoint (v[7:0] != 0) iff (addr == 'h3c);
    cp_alert_test:       coverpoint v[0] iff (addr == 'h0c);
  endgroup

  // ---- Register reads ----
  covergroup status_read_cg with function sample(bit [6:0] addr, bit [31:0] v, bit quiet);
    option.per_instance = 1;
    cp_quiet:          coverpoint quiet;
    cp_intr_state:     coverpoint v[0] iff (addr == 'h00);
    cp_regwen:         coverpoint v[0] iff (addr == 'h10);
    cp_hint:           coverpoint v[0] iff (addr == 'h14);
    cp_cdc_busy:       coverpoint v[0] iff (addr == 'h18);
    cp_wake_status:    coverpoint v[7:0] iff (addr == 'h24) {
      bins none = {0};
      bins one[] = {1, 2, 4, 8, 16, 32, 64, 128};
      bins several = default;
    }
    cp_reset_status:   coverpoint v[1:0] iff (addr == 'h30);
    cp_esc_status:     coverpoint v[0] iff (addr == 'h34);
    cp_fault:          coverpoint v[2:0] iff (addr == 'h40) {
      bins none = {0};
      bins main_pd_glitch = {4};
      bins other = default;
    }
  endgroup

  covergroup wake_info_cg with function sample(bit [9:0] v, bit capture_dis);
    option.per_instance = 1;
    cp_reasons:      coverpoint v[7:0] {
      bins none = {0};
      bins one[] = {1, 2, 4, 8, 16, 32, 64, 128};
      bins several = default;
    }
    cp_fall_through: coverpoint v[8];
    cp_abort:        coverpoint v[9];
    cp_capture_dis:  coverpoint capture_dis;
    x_capture:       cross cp_capture_dis, cp_fall_through, cp_abort;
  endgroup

  // ---- Transitions at the ports ----
  covergroup leave_active_cg with function sample(leave_e why, bit deep);
    option.per_instance = 1;
    cp_why:  coverpoint why;
    cp_deep: coverpoint deep;
    x:       cross cp_why, cp_deep { ignore_bins reset_deep = binsof(cp_why) intersect {LeaveReset} &&
                                                              binsof(cp_deep) intersect {1}; }
  endgroup

  covergroup entry_end_cg with function sample(end_e how, bit deep, bit idle);
    option.per_instance = 1;
    cp_how:  coverpoint how;
    cp_deep: coverpoint deep;
    cp_idle: coverpoint idle;  // all idle at the trigger
    x_how_deep: cross cp_how, cp_deep;
    x_how_idle: cross cp_how, cp_idle;
  endgroup

  covergroup request_cg with function sample(req_e req, bit active, bit in_entry);
    option.per_instance = 1;
    cp_req:      coverpoint req;
    cp_active:   coverpoint active;
    cp_in_entry: coverpoint in_entry;
    x_req_state: cross cp_req, cp_active;
    x_req_entry: cross cp_req, cp_in_entry;
  endgroup

  covergroup wakeup_req_cg with function sample(int unsigned src, bit enabled, bit in_entry);
    option.per_instance = 1;
    cp_src:      coverpoint src { bins s[] = {[0:7]}; }
    cp_enabled:  coverpoint enabled;
    cp_in_entry: coverpoint in_entry;
    x_src_en:    cross cp_src, cp_enabled;
    x_en_entry:  cross cp_enabled, cp_in_entry;
  endgroup

  function new(string name = "pwrmgr_reg_cov");
    super.new(name);
    control_cg      = new();
    wakeup_en_cg    = new();
    reset_en_cg     = new();
    misc_write_cg   = new();
    status_read_cg  = new();
    wake_info_cg    = new();
    leave_active_cg = new();
    entry_end_cg    = new();
    request_cg      = new();
    wakeup_req_cg   = new();
    control_cg.set_inst_name({name, ".control_cg"});
    wakeup_en_cg.set_inst_name({name, ".wakeup_en_cg"});
    reset_en_cg.set_inst_name({name, ".reset_en_cg"});
    misc_write_cg.set_inst_name({name, ".misc_write_cg"});
    status_read_cg.set_inst_name({name, ".status_read_cg"});
    wake_info_cg.set_inst_name({name, ".wake_info_cg"});
    leave_active_cg.set_inst_name({name, ".leave_active_cg"});
    entry_end_cg.set_inst_name({name, ".entry_end_cg"});
    request_cg.set_inst_name({name, ".request_cg"});
    wakeup_req_cg.set_inst_name({name, ".wakeup_req_cg"});
  endfunction

  function void reset();
    control_w = 9'h180;
    wakeup_en_w = '0;
    reset_en_w = '0;
    capture_dis_w = 1'b0;
    in_entry = 1'b0;
    left_active = 1'b0;
    cause_seen = 1'b0;
  endfunction

  // The REGWENs as the model has them: CTRL_CFG_REGWEN, WAKEUP_EN_REGWEN, RESET_EN_REGWEN.
  function void sample_write(bit [6:0] addr, bit [31:0] v, bit [31:0] regwen, bit [31:0] wake_regwen,
                             bit [31:0] rst_regwen);
    misc_write_cg.sample(addr, v);
    case (addr)
      'h14: begin
        control_cg.sample(v[8:0], regwen[0]);
        if (regwen[0]) control_w = v[8:0];
      end
      'h20: for (int i = 0; i < 8; i++) wakeup_en_cg.sample(i, v[i], 1'b0);
      'h2c: for (int i = 0; i < 2; i++) reset_en_cg.sample(i, v[i], 1'b0);
      'h38: capture_dis_w = v[0];
      default: ;
    endcase
  endfunction

  function void sample_read(bit [6:0] addr, bit [31:0] v, bit quiet);
    status_read_cg.sample(addr, v, quiet);
    if (addr == 'h3c) wake_info_cg.sample(v[9:0], capture_dis_w);
    if (addr == 'h20) wakeup_en_w = v[7:0];
    if (addr == 'h2c) reset_en_w = v[1:0];
  endfunction

  // The ports changed from `p` to `n` (layout: pwrmgr_cmodel_sb::sample).
  function void sample_ports(bit [63:0] p, bit [63:0] n);
    bit [7:0] wake_rose = n[7:0] & ~p[7:0];
    bit [1:0] rst_rose  = n[9:8] & ~p[9:8];
    bit active = n[15];
    for (int i = 0; i < 8; i++) begin
      if (wake_rose[i]) begin
        wakeup_req_cg.sample(i, wakeup_en_w[i], in_entry);
        if (in_entry && wakeup_en_w[i] && !cause_seen) begin
          cause = EndWakeup;
          cause_seen = 1'b1;
        end
      end
    end
    for (int i = 0; i < 2; i++) begin
      if (rst_rose[i]) begin
        request_cg.sample(req_e'(i), active, in_entry);
        if (in_entry && !cause_seen) begin
          cause = EndResetReq;
          cause_seen = 1'b1;
        end
      end
    end
    if (n[12] && !p[12]) begin
      request_cg.sample(ReqSw, active, in_entry);
      if (in_entry && !cause_seen) begin
        cause = EndSwReset;
        cause_seen = 1'b1;
      end
    end
    if (n[10] && !p[10]) begin
      request_cg.sample(ReqEsc, active, in_entry);
      if (in_entry && !cause_seen) begin
        cause = EndEscalation;
        cause_seen = 1'b1;
      end
    end
    if (!n[13] && p[13] && n[16]) request_cg.sample(ReqGlitch, active, in_entry);
    // The low-power trigger: WFI with the hint, in Active.
    if (n[11] && !p[11] && p[15] && control_w[0]) begin
      in_entry = 1'b1;
      entry_deep = !control_w[8];
      entry_idle = n[14];
      left_active = 1'b0;
      cause_seen = 1'b0;
      control_w[0] = 1'b0;
    end
    if (!n[15] && p[15]) begin
      leave_active_cg.sample(in_entry ? LeaveLowPower : (n[9:8] != 0 || n[12] || n[10]) ?
                             LeaveReset : LeaveOther, in_entry && entry_deep);
      if (in_entry) left_active = 1'b1;
    end
    if (n[15] && !p[15] && in_entry) begin
      entry_end_cg.sample(cause_seen ? end_e'(cause) : EndNoCause, entry_deep, entry_idle);
      in_entry = 1'b0;
    end
  endfunction

  // The design settled in Active after a trigger without leaving it (an abort or fall through).
  function void sample_settled_active();
    if (in_entry && !left_active) begin
      entry_end_cg.sample(EndNeverLeft, entry_deep, entry_idle);
      in_entry = 1'b0;
    end
  endfunction
endclass
