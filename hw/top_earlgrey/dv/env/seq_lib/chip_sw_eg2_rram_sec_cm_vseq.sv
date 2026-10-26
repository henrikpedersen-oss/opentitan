// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_sec_cm_<countermeasure> (EG2 spec REQ_RRAM_08, REQ_RRAM_11, REQ_RRAM_24; software
// sw/device/tests/eg2_rram/eg2_rram_sec_cm.c, whose header holds the countermeasure table and the
// documents). The bench injects and judges, because every one of these faults stops RRAM (local
// escalation) and the software runs from it:
//   - `EG2 RRAM seccm swap <offA> <offB>`: the two physical RRAM rows (144 bits: data and ECC, each
//     still a valid codeword) swapped by backdoor. The requirement: the relocated word is
//     detected, an error and an alert. The bench waits for the software's report and checks that
//     an alert of rram_ctrl (fatal_err, fatal_std_err) or rv_core_ibex (fatal_hw_err) arrived and
//     that the host read did not return the word silently (`swap silent` is an error).
//   - `EG2 RRAM seccm verify arm`: on the next shadow (verify) read the macro's data into
//     rram_phy_rd (rd_rdata_i) gets one bit flipped for the cycle it is taken. Checks:
//     FAULT_STATUS.PHY_RD_INTG_ERR (through SBA), fatal_err at the alert handler, and no
//     read-buffer entry still Valid or Verified for that RRAM word (theory_of_operation.md: "If
//     the shadow read does not match the Valid entry, the entry is invalidated").
//   - `EG2 RRAM seccm fi arm <cm>`: the countermeasure's fault (one instance drawn among those
//     the countermeasure lists): a sparse FSM's state through its sec_cm proxy (the chip binds
//     sec_cm_prim_sparse_fsm_flop_bind), one copy of a redundant counter (prim_count cnt_q[k],
//     as prim_count_if does; the chip does not bind sec_cm_prim_count_bind), one copy of a
//     duplicated arbiter's requests, or the inconsistency a consistency check watches for.
//     Checks: the STD_FAULT_STATUS or FAULT_STATUS bit the register description gives the
//     countermeasure, and its alert (fatal_std_err for STD_FAULT_STATUS, fatal_err for
//     FAULT_STATUS); the TL-UL gate FSMs have no documented bit, so any fatal alert of rram_ctrl.
// Then the rows are repaired, boot_phase = 1, and a power-on reset; the second boot checks the
// clean state. Assertions inside rram_ctrl are off from the fault to the reset (the fault breaks
// their premises, as in the block bench's sec_cm tests); every other check stays on.
class chip_sw_eg2_rram_sec_cm_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_sec_cm_vseq)

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string Rc     = {PdMain, ".u_rram_ctrl"};
  localparam string Phy    = {Rc, ".u_rram_phy"};
  localparam string PhyRd  = {Phy, ".u_rram_phy_rd"};
  localparam bit [31:0] CoreBase = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR;
  localparam int unsigned NumRdBuf = rram_ctrl_pkg::NumRdBuf;

  // STD_FAULT_STATUS and FAULT_STATUS bits (rram_ctrl.hjson).
  localparam int StdLcmgrErr = 1, StdOtpErr = 3, StdPhyFifoErr = 6, StdPhyFsmErr = 7,
                 StdPhyCntErr = 8, StdPhyArbErr = 9, StdCtrlFsmErr = 10, StdCtrlCntErr = 11,
                 StdCtrlFifoErr = 12;
  localparam int FltPhyRdIntgErr = 10, FltPhyRdCtrlErr = 11, FltSpuriousDone = 12,
                 FltHostGntErr = 13;

  typedef enum int unsigned {ScSwap, ScVerify, ScFi} scenario_e;

  rand scenario_e   scenario;
  rand int unsigned cm;
  rand bit          scramble;
  rand int unsigned pick;  // which instance of the countermeasure
  rand int unsigned flip_bit;

  constraint knobs_c {
    cm inside {[0:9]};
    pick inside {[0:15]};
    flip_bit inside {[0:127]};
  }

  function new(string name = "");
    super.new(name);
    // The checks after the fault read rram_ctrl's registers through SBA (RRAM is off by then).
    select_rv_dm_tap = 1'b1;
  endfunction

  virtual function void write_params();
    set_param("scenario", scenario);
    set_param("cm", cm);
    set_param("scramble", scramble);
    set_param("boot_phase", 0);
  endfunction

  // The values the software runs with (a pinned plusarg wins over the draw).
  protected function int unsigned eff(string name, int unsigned v);
    void'($value$plusargs({"eg2_param_", name, "=%d"}, v));
    return v;
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  protected task neg_clk();
    cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
  endtask

  // Forces `path` to `value` from this falling edge to after the next rising edge.
  protected task force_one_cycle(string path, uvm_hdl_data_t value);
    `DV_CHECK_FATAL(uvm_hdl_force(path, value), path)
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    #1ps;
    `DV_CHECK_FATAL(uvm_hdl_release(path), path)
  endtask

  protected function void asserts_off();
    $assertoff(0, "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl");
  endfunction

  protected function void asserts_on();
    $asserton(0, "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl");
  endfunction

  // rram_ctrl's core registers through SBA.
  protected task sba_reg(bit [31:0] offset, output bit [31:0] value);
    sba_access_err_e st;
    sba_rd32(CoreBase + offset, value, st);
    `DV_CHECK_EQ(st, SbaErrNone, $sformatf("SBA read of rram_ctrl offset 0x%0h failed", offset))
  endtask

  protected task wait_alert(int unsigned alert, output bit seen, input int unsigned polls = 2000);
    uvm_reg_data_t cause;
    seen = 1'b0;
    for (int i = 0; i < polls && !seen; i++) begin
      csr_rd(.ptr(ral.alert_handler.alert_cause[alert]), .value(cause), .backdoor(1));
      seen = cause[0];
      if (!seen) cfg.chip_vif.cpu_clk_rst_if.wait_clks(10);
    end
  endtask

  // Waits until bit `bit_idx` of the register at `offset` is set (through SBA), then checks it.
  protected task check_status_bit(string reg_name, bit [31:0] offset, int bit_idx);
    bit [31:0] v;
    for (int i = 0; i < 50; i++) begin
      sba_reg(offset, v);
      if (v[bit_idx]) break;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(200);
    end
    `uvm_info(`gfn, $sformatf("EG2 RRAM seccm: %s = 0x%08x", reg_name, v), UVM_LOW)
    `DV_CHECK_EQ(v[bit_idx], 1'b1, $sformatf("%s bit %0d not set", reg_name, bit_idx))
  endtask

  // The next software line starting with `prefix` within `timeout_ns`, or "" (no failure: the
  // fault may stop the software before it reports).
  protected task wait_line_or_none(string prefix, output string line, input int unsigned timeout_ns);
    bit found;
    line = "";
    fork : isolation_fork
      begin
        fork
          forever begin
            string s;
            @(cfg.sw_logger_vif.printed_log_event);
            s = string'(cfg.sw_logger_vif.printed_log);
            if (s.len() >= prefix.len() && s.substr(0, prefix.len() - 1) == prefix) begin
              line = s;
              found = 1'b1;
              break;
            end
          end
          #(timeout_ns * 1ns);
        join_any
        disable fork;
      end
    join
    if (!found) `uvm_info(`gfn, $sformatf("no software line '%s' within %0d ns", prefix, timeout_ns),
                          UVM_LOW)
  endtask

  // ---- scenario 0: the row swap ----

  protected function string row_path(bit [31:0] off);
    return $sformatf("%0s[%0d]", cfg.mem_bkdr_util_h[RramData].get_full_path(0), off >> 4);
  endfunction

  protected function void swap_rows(bit [31:0] off_a, bit [31:0] off_b);
    uvm_hdl_data_t a = rd(row_path(off_a));
    uvm_hdl_data_t b = rd(row_path(off_b));
    `DV_CHECK_FATAL(uvm_hdl_deposit(row_path(off_a), b), row_path(off_a))
    `DV_CHECK_FATAL(uvm_hdl_deposit(row_path(off_b), a), row_path(off_b))
  endfunction

  protected task do_swap(bit [31:0] off_a, bit [31:0] off_b);
    string line;
    bit fatal, std, core;
    announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler",
             $sformatf("RRAM rows 0x%0h and 0x%0h swapped (sec_cm_mem_addr_infection)",
                       off_a, off_b));
    swap_rows(off_a, off_b);
    asserts_off();
    // The software's report of its host read, or nothing if RRAM stopped it.
    wait_line_or_none("EG2 RRAM seccm swap ", line, 2_000_000);
    `uvm_info(`gfn, $sformatf("EG2 RRAM seccm swap: software says '%s'", line), UVM_LOW)
    if (line.len() >= 24 && line.substr(0, 23) == "EG2 RRAM seccm swap sile") begin
      `uvm_error(`gfn, "EG2 RRAM seccm: the relocated word was returned silently (REQ_RRAM_08)")
    end
    wait_alert(TopEarlgreyAlertIdRramCtrlFatalErr, fatal, 200);
    wait_alert(TopEarlgreyAlertIdRramCtrlFatalStdErr, std, 10);
    wait_alert(TopEarlgreyAlertIdRvCoreIbexFatalHwErr, core, 10);
    `uvm_info(`gfn, $sformatf("EG2 RRAM seccm swap: alerts fatal_err %0d fatal_std_err %0d core %0d",
                              fatal, std, core), UVM_LOW)
    `DV_CHECK(fatal || std || core, "no alert for the relocated word (REQ_RRAM_08)")
    swap_rows(off_a, off_b);  // repaired before the second boot
  endtask

  // ---- scenario 1: the verify-read glitch ----

  protected task do_verify();
    bit [31:0] addr;
    bit hit, seen;
    uvm_hdl_data_t data;
    for (int cyc = 0; cyc < 2_000_000 && !hit; cyc++) begin
      neg_clk();
      if (rd({PhyRd, ".rd_busy"}) == 1 && rd({PhyRd, ".rd_done_i"}) == 1 &&
          rd({PhyRd, ".rd_pend_shadow_q"}) == 1) begin
        hit = 1'b1;
      end
    end
    `DV_CHECK_FATAL(hit, "no shadow (verify) read completed")
    addr = rd({PhyRd, ".rd_pend_addr_q"});
    data = rd({PhyRd, ".rd_rdata_i"});
    data[flip_bit % 128] ^= 1'b1;
    announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler",
             $sformatf("shadow read of RRAM word 0x%0h: data bit %0d flipped (sec_cm_mem_readback)",
                       addr, flip_bit % 128));
    asserts_off();
    force_one_cycle({PhyRd, ".rd_rdata_i"}, data);
    // The entry the shadow read verified is invalidated: none holds the word as Valid/Verified.
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(20);
    for (int i = 0; i < NumRdBuf; i++) begin
      rram_ctrl_pkg::rd_buf_t b;
      uvm_hdl_data_t v = rd($sformatf("%0s.rd_buf[%0d]", PhyRd, i));
      b = rram_ctrl_pkg::rd_buf_t'(v[$bits(rram_ctrl_pkg::rd_buf_t)-1:0]);
      if (b.addr == addr[rram_ctrl_pkg::AddrW-1:0] &&
          (b.attr == rram_ctrl_pkg::Valid || b.attr == rram_ctrl_pkg::Verified)) begin
        `uvm_error(`gfn, $sformatf("read-buffer entry %0d still holds RRAM word 0x%0h (attr %s)",
                                   i, addr, b.attr.name()))
      end
    end
    cfg.debugger.set_dmactive(1);
    check_status_bit("FAULT_STATUS", rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET,
                     FltPhyRdIntgErr);
    wait_alert(TopEarlgreyAlertIdRramCtrlFatalErr, seen);
    `DV_CHECK(seen, "rram_ctrl fatal_err not at the alert handler")
  endtask

  // ---- scenario 2: one countermeasure ----

  protected task do_fi(int unsigned which);
    string path, what;
    bit [31:0] offset = rram_ctrl_reg_pkg::RRAM_CTRL_STD_FAULT_STATUS_OFFSET;
    int bit_idx = -1;
    int unsigned alert = TopEarlgreyAlertIdRramCtrlFatalStdErr;
    bit seen, std_seen, fatal_seen;
    asserts_off();
    case (which)
      0, 1, 4, 5: begin  // sparse FSMs, through their sec_cm proxies
        sec_cm_pkg::sec_cm_base_if_proxy proxy;
        string fsms[$];
        int bits[$];
        case (which)
          0: begin
            fsms = '{".u_rram_ctrl_lcmgr.u_state_regs", ".u_rram_ctrl_lcmgr.u_rma_state_regs",
                     ".u_rram_ctrl_otp.u_state_regs", ".u_rram_ctrl_arb.u_state_regs",
                     ".u_rram_ctrl_rd.u_state_regs", ".u_rram_ctrl_wr.u_state_regs"};
            bits = '{StdLcmgrErr, StdLcmgrErr, StdOtpErr, StdCtrlFsmErr, StdCtrlFsmErr,
                     StdCtrlFsmErr};
          end
          1: begin
            fsms = '{".u_rram_phy.u_rram_phy_wr.u_state_regs"};
            bits = '{StdPhyFsmErr};
          end
          4: begin
            fsms = '{".u_tl_gate.u_state_regs"};
            bits = '{-1};
          end
          default: begin
            fsms = '{".u_wr_tl_gate.u_state_regs"};
            bits = '{-1};
          end
        endcase
        path = {Rc, fsms[pick % fsms.size()]};
        bit_idx = bits[pick % bits.size()];
        proxy = sec_cm_pkg::find_sec_cm_if_proxy(path);
        what = $sformatf("sparse FSM %s", path);
        announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler", what);
        proxy.inject_fault();
      end
      2, 3: begin  // redundant counters: one of the two copies of cnt_q
        string cnts[$];
        int bits[$];
        uvm_hdl_data_t v;
        if (which == 2) begin
          cnts = '{".u_rram_ctrl_lcmgr.u_seed_cnt", ".u_rram_ctrl_lcmgr.u_addr_cnt",
                   ".u_rram_ctrl_lcmgr.u_page_cnt", ".u_rram_ctrl_lcmgr.u_word_cnt",
                   ".u_rram_ctrl_lcmgr.u_wipe_idx_cnt", ".u_rram_ctrl_otp.u_bus_wcnt",
                   ".u_rram_ctrl_rd.u_cnt", ".u_rram_ctrl_wr.u_cnt",
                   ".u_rram_phy.u_rram_phy_wr.u_word_cnt"};
          bits = '{StdLcmgrErr, StdLcmgrErr, StdLcmgrErr, StdLcmgrErr, StdLcmgrErr, StdOtpErr,
                   StdCtrlCntErr, StdCtrlCntErr, StdPhyCntErr};
        end else begin
          string ptr = $urandom_range(0, 1) ? "u_wptr" : "u_rptr";
          string fifos[$] = '{".u_wr_fifo", ".u_rram_phy.u_meta_fifo",
                              ".u_rram_phy.u_rram_phy_rd.u_rd_fifo",
                              ".u_rram_phy.u_rram_phy_rd.u_mask_fifo",
                              ".u_rram_phy.u_rram_phy_rd.u_meta_fifo"};
          foreach (fifos[i]) begin
            cnts.push_back({fifos[i], ".gen_normal_fifo.u_fifo_cnt.gen_secure_ptrs.", ptr});
          end
          bits = '{StdCtrlFifoErr, StdPhyFifoErr, StdPhyFifoErr, StdPhyFifoErr, StdPhyFifoErr};
        end
        path = $sformatf("%0s%0s.cnt_q[%0d]", Rc, cnts[pick % cnts.size()], pick % 2);
        bit_idx = bits[pick % bits.size()];
        neg_clk();
        v = rd(path);
        what = $sformatf("redundant counter %s 0x%0h -> 0x%0h", path, v, v ^ 1);
        announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler", what);
        force_one_cycle(path, v ^ 1);
      end
      6: begin  // a duplicated arbiter: the two copies' requests made to differ
        string arbs[$] = '{".u_rram_phy.u_host_arb",
                           ".u_rram_phy.u_rram_scramble.u_prim_arbiter_tree_calc",
                           ".u_rram_phy.u_rram_scramble.u_prim_arbiter_tree_op"};
        string arb = {Rc, arbs[pick % arbs.size()]};
        string req0 = {arb, ".gen_input_bufs[0].u_req_buf.out_o"};
        string req1 = {arb, ".gen_input_bufs[1].u_req_buf.out_o"};
        bit which_copy = pick[0];
        bit_idx = StdPhyArbErr;
        what = $sformatf("duplicated arbiter %s: copy %0d requests all, the other none", arb,
                         which_copy);
        announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler", what);
        `DV_CHECK_FATAL(uvm_hdl_force(req0, which_copy ? '0 : '1), req0)
        `DV_CHECK_FATAL(uvm_hdl_force(req1, which_copy ? '1 : '0), req1)
        cfg.chip_vif.cpu_clk_rst_if.wait_clks($urandom_range(60, 90));
        `DV_CHECK_FATAL(uvm_hdl_release(req0), req0)
        `DV_CHECK_FATAL(uvm_hdl_release(req1), req1)
      end
      7, 8, 9: begin  // the consistency checks: the condition each watches for
        string cond, target;
        uvm_hdl_data_t value = 1;
        bit hit;
        offset = rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET;
        alert = TopEarlgreyAlertIdRramCtrlFatalErr;
        case (which)
          7: begin  // a host grant on the info partition
            cond = {Phy, ".host_gnt"};
            target = {Phy, ".u_prim_buf_muxed_part.out_o"};
            bit_idx = FltHostGntErr;
          end
          8: begin  // a host read done while the response belongs to the controller
            cond = {Phy, ".host_rd_done_o"};
            target = {Phy, ".ctrl_rsp"};
            bit_idx = FltSpuriousDone;
          end
          default: begin  // an RRAM read request with no request and no shadow read behind it
            cond = "";
            target = {PhyRd, ".rd_req_o"};
            bit_idx = FltPhyRdCtrlErr;
          end
        endcase
        for (int cyc = 0; cyc < 2_000_000 && !hit; cyc++) begin
          neg_clk();
          if (cond != "") begin
            hit = rd(cond) == 1;
          end else begin
            hit = rd({PhyRd, ".req_i"}) == 0 && rd({PhyRd, ".shadow_rd_req"}) == 0 &&
                  rd({PhyRd, ".rd_req_o"}) == 0;
          end
        end
        `DV_CHECK_FATAL(hit, $sformatf("the condition for countermeasure %0d never held", which))
        what = $sformatf("%s forced to %0d for one cycle", target, value);
        announce("chip_sw_eg2_rram_sec_cm_vseq", "rram_ctrl,alert_handler", what);
        force_one_cycle(target, value);
      end
      default: `uvm_fatal(`gfn, $sformatf("no countermeasure %0d", which))
    endcase
    `uvm_info(`gfn, $sformatf("EG2 RRAM seccm cm %0d: %s", which, what), UVM_LOW)
    cfg.debugger.set_dmactive(1);
    if (bit_idx >= 0) begin
      check_status_bit(offset == rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET ?
                       "FAULT_STATUS" : "STD_FAULT_STATUS", offset, bit_idx);
      wait_alert(alert, seen);
      `DV_CHECK(seen, $sformatf("rram_ctrl alert %0d not at the alert handler", alert))
    end else begin
      bit [31:0] s, f;
      wait_alert(TopEarlgreyAlertIdRramCtrlFatalStdErr, std_seen);
      wait_alert(TopEarlgreyAlertIdRramCtrlFatalErr, fatal_seen, 10);
      sba_reg(rram_ctrl_reg_pkg::RRAM_CTRL_STD_FAULT_STATUS_OFFSET, s);
      sba_reg(rram_ctrl_reg_pkg::RRAM_CTRL_FAULT_STATUS_OFFSET, f);
      `uvm_info(`gfn, $sformatf("EG2 RRAM seccm cm %0d: STD_FAULT_STATUS 0x%08x FAULT_STATUS 0x%08x",
                                which, s, f), UVM_LOW)
      `DV_CHECK(std_seen || fatal_seen, "no fatal alert of rram_ctrl for the TL-UL gate FSM")
    end
  endtask

  protected task second_boot();
    cfg.debugger.set_dmactive(0);
    set_param("boot_phase", 1);
    `uvm_info(`gfn, "EG2 RRAM seccm: second boot", UVM_LOW)
    apply_reset();
    asserts_on();
  endtask

  virtual task body();
    string line;
    bit [31:0] a, b, c;
    super.body();
    `uvm_info(`gfn, $sformatf("EG2 RRAM seccm: scenario %0d, countermeasure %0d, pick %0d",
                              eff("scenario", scenario), eff("cm", cm), pick), UVM_LOW)
    wait_sw_line("EG2 RRAM seccm ", line, 400_000_000, 1);
    if (line == "") return;
    if ($sscanf(line, "EG2 RRAM seccm swap 0x%h 0x%h", a, b) == 2) begin
      do_swap(a, b);
    end else if (line == "EG2 RRAM seccm verify arm") begin
      do_verify();
    end else if ($sscanf(line, "EG2 RRAM seccm fi arm 0x%h", c) == 1) begin
      do_fi(c);
    end else begin
      `uvm_fatal(`gfn, $sformatf("unexpected software line '%s'", line))
    end
    second_boot();
  endtask

endclass : chip_sw_eg2_rram_sec_cm_vseq
