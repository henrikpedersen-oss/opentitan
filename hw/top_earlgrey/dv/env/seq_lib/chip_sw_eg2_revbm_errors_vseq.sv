// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_cheriot_revbm_errors_<case> (REQ_CMS_08, REQ_CIB_03; spec "Revocation", "Corner cases";
// image //sw/device/tests/eg2_cheriot:eg2_revbm_errors_cheriot). The glitch part of the row is
// chip_sw_cheriot_trvk_fault.
//
// Bench steps (EG2 BENCH <n> <op> <a> <b>), added to chip_sw_eg2_revocation_bench_vseq:
//
//   revbm_err_arm <kind | field << 8> <addr>
//                           One fault on the response to the bitmap read of the word that covers
//                           heap granule <addr>: kind 0 d_error with correct integrity, on the
//                           core's bitmap port (rv_core_ibex_corerevbm_tl); kind 1 the same
//                           response with one bit of its response integrity (field 1), data
//                           integrity (2) or data (3) flipped; kind 2 the meta SRAM's response to
//                           cheriot_mem_sys for that read (cheriot_mem_sys_meta_sram_tl) with one
//                           data bit flipped, integrity unchanged (a memory bit error). A flipped
//                           data bit is never the bit <addr> selects. The fault is held until the
//                           host takes the response. Then the bench watches the core's load filter
//                           (ibex_trvk): the cycle its bitmap response is consumed with a data
//                           response (the verdict, revbm_rvalid_i && upstream_rvalid_o) must come
//                           within VerdictBound cycles (no hang), with upstream_tag_revoked_o set
//                           and upstream_tag_o clear (the failed check revokes); and the alert
//                           requests within AlertWindow cycles: rv_core_ibex's fatal_hw_err
//                           (fatal_intg_err | fatal_core_err) for kinds 0-1, cheriot_mem_sys's
//                           fatal_fault (cheriot_fatal_error) for kind 2.
//   revbm_err_end 0 0       Waits for the checks; answers {cms alert[4], core alert[3], verdict
//                           tagged[2], revoked[1], fault taken[0]}.
//   revbm_hold_reset <addr> <kind>
//                           Holds the response to the core's next bitmap read of <addr>'s word
//                           (d_valid 0 towards the core, d_ready 0 towards cheriot_mem_sys), then
//                           the reset: kind 1 power-on (after hold_cycles), 2 NDM over JTAG (after
//                           hold_cycles), 3 the software's watchdog bite (awaited). Once
//                           rv_core_ibex's rst_ni is low the hold is released and `reset_done` is
//                           written into the image, so its next boot knows.
//   revbm_hold_end 0 0      Answers {reset while the read was held[1], read held[0]}.
//
// Checks the bench makes are EG2CHK lines (UVM errors when they fail). If rv_core_ibex has turned
// the CPU's fetch off after the fault (rv_core_ibex theory_of_operation.md: fetch is disabled when
// fatal_hw_err is raised), the software cannot finish: the bench ends the run with its verdict.
// +eg2_inject_fail=1 inverts the revocation check: `EG2CHK FAIL failed check revoked`.
// Runs without the co-simulation (default build mode): it cannot know the injected outcome.
class chip_sw_eg2_revbm_errors_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_revbm_errors_vseq)

  localparam string RvCore = {PdMain, ".u_rv_core_ibex"};
  localparam string Cms = {PdMain, ".u_cheriot_mem_sys"};
  localparam string Trvk = {RvCore, ".u_core.gen_cheriot_trvk.i_ibex_trvk"};
  localparam string MetaPort = "cheriot_mem_sys_meta_sram_tl";
  localparam int unsigned ArmTimeout = 2_000_000;
  localparam int unsigned TakeTimeout = 1000;
  localparam int unsigned VerdictBound = 2000;
  localparam int unsigned AlertWindow = 2000;
  // Polls of 100 ns for the reset to reach rv_core_ibex (5 ms: the watchdog's bite included).
  localparam int unsigned ResetPolls = 50_000;

  function new(string name = "");
    super.new(name);
    use_jtag = 1'b1;  // the NDM reset
  endfunction

  rand int unsigned err_case, field, reset_kind, hold_cycles;
  constraint knobs_c {
    err_case inside {[1:4]};
    field inside {[1:3]};
    reset_kind inside {[1:3]};
    hold_cycles inside {[1:400]};
  }

  bit inject_fail;
  protected int unsigned bench_checks, bench_fails;
  // revbm_err
  protected bit [31:0] err_kind, err_field, err_addr;
  protected bit err_done, err_hit, err_revoked, err_tagged, err_core_alert, err_cms_alert;
  // revbm_hold
  protected bit [31:0] hold_addr, hold_kind;
  protected bit hold_done, hold_held, hold_reset_seen;

  virtual task pre_start();
    void'($value$plusargs("eg2_inject_fail=%d", inject_fail));
    super.pre_start();
  endtask

  virtual function void write_params();
    super.write_params();
    set_param("error", err_case);
    set_param("field", field);
    set_param("reset_kind", reset_kind);
    void'($value$plusargs("eg2_param_hold_cycles=%d", hold_cycles));
    `uvm_info(`gfn, $sformatf("EG2 revbm_errors: hold %0d cycles before a bench reset",
                              hold_cycles), UVM_LOW)
  endfunction

  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    result = 0;
    case (op)
      "revbm_err_arm": begin
        err_kind = a & 'hff;
        err_field = (a >> 8) & 'hff;
        err_addr = b;
        {err_done, err_hit, err_revoked, err_tagged, err_core_alert, err_cms_alert} = '0;
        fork err_inject(); join_none
      end
      "revbm_err_end": begin
        wait (err_done);
        result = 32'(err_hit) | (32'(err_revoked) << 1) | (32'(err_tagged) << 2) |
                 (32'(err_core_alert) << 3) | (32'(err_cms_alert) << 4);
      end
      "revbm_hold_reset": begin
        hold_addr = a;
        hold_kind = b;
        {hold_done, hold_held, hold_reset_seen} = '0;
        fork hold_and_reset(); join_none
      end
      "revbm_hold_end": begin
        wait (hold_done);
        result = 32'(hold_held) | (32'(hold_reset_seen) << 1);
      end
      default: super.bench_op(op, a, b, result);
    endcase
  endtask

  protected function bit bench_check(string what, bit cond, string detail = "");
    bench_checks++;
    if (cond) begin
      `uvm_info(`gfn, $sformatf("EG2CHK ok %s %s", what, detail), UVM_LOW)
    end else begin
      bench_fails++;
      `uvm_error(`gfn, $sformatf("EG2CHK FAIL %s %s", what, detail))
    end
    return cond;
  endfunction

  protected function bit [9:0] word_of(bit [31:0] heap_addr);
    return ((heap_addr - HeapBase) >> 8) & 'h3ff;
  endfunction

  // A bitmap read of `word` on `port`: the core's port carries the word's byte offset in the
  // bitmap (4 KiB aligned), the meta SRAM port its offset in the meta SRAM (the bitmap at 0).
  protected function bit is_bitmap_read(bit meta, tlul_pkg::tl_h2d_t h, bit [9:0] word);
    bit [31:0] off = h.a_address & (meta ? 32'hffff : 32'hfff);
    return h.a_opcode == tlul_pkg::Get && off < 32'hc00 && (off >> 2) == word;
  endfunction

  // --------------------------------------------------------------------------- revbm_err

  protected task err_inject();
    bit meta = err_kind == 2;
    string port = meta ? MetaPort : CoreRevbm;
    string rsp = {PdMain, ".", port, "_rsp"};
    bit [9:0] word = word_of(err_addr);
    int unsigned sel = ((err_addr - HeapBase) >> 3) % 32;
    int unsigned b;
    tlul_pkg::tl_h2d_t h;
    tlul_pkg::tl_d2h_t d, bad;
    bit found, have_rsp, verdict;
    bit [top_pkg::TL_AIW-1:0] src;
    for (int unsigned i = 0; i < ArmTimeout && !found; i++) begin
      cycle();
      h = rd_h2d(port);
      d = rd_d2h(port);
      if (h.a_valid && d.a_ready && is_bitmap_read(meta, h, word)) begin
        src = h.a_source;
        found = 1;
      end
    end
    if (!bench_check("bitmap read seen", found, $sformatf("(%s, word %0d)", port, word))) begin
      err_done = 1;
      return;
    end
    for (int unsigned i = 0; i < ArmTimeout && !have_rsp; i++) begin
      cycle();
      d = rd_d2h(port);
      if (d.d_valid && d.d_source == src) have_rsp = 1;
    end
    if (!bench_check("bitmap response seen", have_rsp, port)) begin
      err_done = 1;
      return;
    end
    bad = d;
    do b = $urandom_range(0, 31); while (b == sel);
    case (err_kind)
      0: begin
        bad.d_error = 1'b1;
        bad.d_user.rsp_intg = tlul_pkg::get_rsp_intg(bad);
      end
      1: begin
        case (err_field)
          1: bad.d_user.rsp_intg[$urandom_range(0, 6)] ^= 1'b1;
          2: bad.d_user.data_intg[$urandom_range(0, 6)] ^= 1'b1;
          default: bad.d_data[b] ^= 1'b1;
        endcase
      end
      default: bad.d_data[b] ^= 1'b1;
    endcase
    // TODO(inject-registry): announce this fault (source this vseq, target `rsp`, kind err_kind,
    // now, one response).
    `uvm_info("EG2_INJECT", $sformatf("EG2 fault: %s kind %0d field %0d on the read of bitmap word %0d (data bit %0d if flipped, selected bit %0d)",
                                      rsp, err_kind, err_field, word, b, sel), UVM_LOW)
    `DV_CHECK_FATAL(uvm_hdl_force(rsp, uvm_hdl_data_t'(bad)), rsp)
    #1ps;
    // Held until the host takes it.
    for (int unsigned i = 0; i < TakeTimeout && !err_hit; i++) begin
      h = rd_h2d(port);
      if (h.d_ready) err_hit = 1;
      sample_err(verdict);
      cpu_clk();
      #1ps;
    end
    `DV_CHECK_FATAL(uvm_hdl_release(rsp), rsp)
    void'(bench_check("fault taken", err_hit, $sformatf("(%s took the changed response)", port)));
    // The verdict and the alerts.
    for (int unsigned i = 0; i < VerdictBound + AlertWindow; i++) begin
      sample_err(verdict);
      if (verdict && (meta ? err_cms_alert : err_core_alert)) break;
      cycle();
    end
    void'(bench_check("verdict within the bound (no hang)", verdict,
                      $sformatf("(%0d cycles)", VerdictBound + AlertWindow)));
    void'(bench_check("failed check revoked", (verdict && err_revoked && !err_tagged) ^ inject_fail,
                      $sformatf("(upstream_tag_revoked_o %0d, upstream_tag_o %0d at the verdict)",
                                err_revoked, err_tagged)));
    if (meta) begin
      void'(bench_check("cheriot_mem_sys fatal_fault requested", err_cms_alert,
                        $sformatf("(rv_core_ibex fatal_hw_err: %0d)", err_core_alert)));
    end else begin
      void'(bench_check("rv_core_ibex fatal_hw_err requested", err_core_alert,
                        $sformatf("(cheriot_mem_sys fatal_fault: %0d)", err_cms_alert)));
    end
    err_done = 1;
    end_if_fetch_off();
  endtask

  // One cycle's observation: the load filter's verdict (the first only) and the alert requests.
  protected function void sample_err(inout bit verdict);
    uvm_hdl_data_t cms;
    if (!verdict && rd({Trvk, ".revbm_rvalid_i"}) != 0 && rd({Trvk, ".upstream_rvalid_o"}) != 0) begin
      verdict = 1;
      err_revoked = rd({Trvk, ".upstream_tag_revoked_o"}) != 0;
      err_tagged = rd({Trvk, ".upstream_tag_o"}) != 0;
    end
    if (rd({RvCore, ".fatal_intg_err"}) != 0 || rd({RvCore, ".fatal_core_err"}) != 0) begin
      err_core_alert = 1;
    end
    cms = rd({Cms, ".cheriot_fatal_error"});
    if (cms != 0) err_cms_alert = 1;
  endfunction

  // With the CPU's fetch off the software never finishes: the bench's verdict ends the run.
  protected task end_if_fetch_off();
    uvm_hdl_data_t fe;
    repeat (100) cycle();
    fe = rd({RvCore, ".local_fetch_enable_q"});
    if (fe[3:0] == 4'(lc_ctrl_pkg::On)) begin
      if (err_core_alert) begin
        `uvm_info(`gfn, {"EG2 note: fatal_hw_err requested and the CPU's fetch still on ",
                         "(rv_core_ibex theory_of_operation.md: fetch is disabled when ",
                         "fatal_hw_err is raised)"}, UVM_LOW)
      end
      return;
    end
    `uvm_info(`gfn, "EG2 bench: the CPU's fetch is off; the bench ends the run", UVM_LOW)
    fork : isolation_fork
      begin
        fork
          wait (cfg.sw_test_status_vif.sw_test_done);
          #(1ms);
        join_any
        disable fork;
      end
    join
    if (!cfg.sw_test_status_vif.sw_test_done) begin
      `uvm_info(`gfn, $sformatf("EG2 bench: %0d checks, %0d failed", bench_checks, bench_fails),
                UVM_LOW)
      override_test_status_and_finish(.passed(bench_fails == 0 && bench_checks > 0));
    end
  endtask

  // --------------------------------------------------------------------------- revbm_hold

  protected function bit rst_low();
    return rd({RvCore, ".rst_ni"}) == 0;
  endfunction

  protected task wait_rst_low(output bit seen);
    seen = 0;
    for (int unsigned i = 0; i < ResetPolls && !seen; i++) begin
      #100ns;
      seen = rst_low();
    end
  endtask

  protected task hold_and_reset();
    string req = {PdMain, ".", CoreRevbm, "_req"};
    string rsp = {PdMain, ".", CoreRevbm, "_rsp"};
    bit [9:0] word = word_of(hold_addr);
    tlul_pkg::tl_h2d_t h;
    tlul_pkg::tl_d2h_t d;
    bit seen, ndm_done;
    for (int unsigned i = 0; i < ArmTimeout && !hold_held; i++) begin
      cycle();
      h = rd_h2d(CoreRevbm);
      d = rd_d2h(CoreRevbm);
      if (h.a_valid && d.a_ready && is_bitmap_read(1'b0, h, word)) begin
        // The response comes in a later cycle: held from the next edge on.
        if (!uvm_hdl_force({rsp, ".d_valid"}, 0)) `uvm_fatal(`gfn, $sformatf("cannot force %s", rsp))
        if (!uvm_hdl_force({req, ".d_ready"}, 0)) `uvm_fatal(`gfn, $sformatf("cannot force %s", req))
        hold_held = 1;
      end
    end
    if (!bench_check("bitmap read held", hold_held, $sformatf("(word %0d)", word))) begin
      hold_done = 1;
      return;
    end
    `uvm_info(`gfn, $sformatf("EG2 bench: core bitmap read of word %0d held; reset kind %0d",
                              word, hold_kind), UVM_LOW)
    case (hold_kind)
      1: begin
        repeat (hold_cycles) cycle();
        `uvm_info(`gfn, "EG2 bench: power-on reset during the bitmap read", UVM_LOW)
        cfg.chip_vif.por_n_if.drive(0);
        repeat (6) @cfg.chip_vif.pwrmgr_low_power_if.cb;
        seen = rst_low();
        hold_release(req, rsp, seen);
        cfg.clk_rst_vif.wait_clks(10);
        cfg.chip_vif.por_n_if.drive(1);
      end
      2: begin
        repeat (hold_cycles) cycle();
        fork
          begin
            ndm_reset();
            ndm_done = 1;
          end
        join_none
        wait_rst_low(seen);
        hold_release(req, rsp, seen);
        wait (ndm_done);
      end
      default: begin
        // The software's watchdog bites.
        wait_rst_low(seen);
        hold_release(req, rsp, seen);
      end
    endcase
    hold_done = 1;
  endtask

  protected task hold_release(string req, string rsp, bit seen);
    hold_reset_seen = seen;
    void'(bench_check("reset while the bitmap read was held", seen,
                      $sformatf("(rv_core_ibex rst_ni, kind %0d)", hold_kind)));
    if (!uvm_hdl_release({req, ".d_ready"})) `uvm_fatal(`gfn, $sformatf("cannot release %s", req))
    if (!uvm_hdl_release({rsp, ".d_valid"})) `uvm_fatal(`gfn, $sformatf("cannot release %s", rsp))
    // Written during the reset, before any software reads it.
    set_param("reset_done", 1);
  endtask
endclass
