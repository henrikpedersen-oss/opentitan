// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_cheriot_trvk_fault (REQ_CIB_03; the glitch part of chip_sw_cheriot_revbm_errors, spec
// "Revocation", "Corner cases"; image //sw/device/tests/eg2_cheriot:eg2_trvk_fault_cheriot).
//
// One single-cycle fault in the core's load filter (ibex_trvk, rv_core_ibex.u_core
// .gen_cheriot_trvk.i_ibex_trvk), at the site named by +eg2_trvk_fault_site (random otherwise),
// injected only in a cycle where it makes a revoked capability reach the core tagged. The sites
// are those of the core bench's core_ibex_trvk_fault_test:
//   verdict       revbm_revoked forced 0 on a revoked bitmap response
//   tag_out       upstream_tag_o forced 1 on a revoked response
//   bit_select    one bit of revbm_bit_select flipped to a clear bit of the bitmap word
//   req_required  revbm_req_required forced 0 in a lookup's first cycle: no lookup, the tag passes
//   out_of_range  revbm_out_of_range forced 1 in a lookup's first cycle
//   sealing       is_sealing_cap forced 1 in a lookup's first cycle
//   align         misalign_flag_out forced 0 in a lookup's first cycle
// Only capabilities based in the range the software names in `trvk_fault_arm` (its arena A) count
// as opportunities; for the last four sites the software marks that whole range (`mark_all`), so
// the skipped lookup would have revoked. The (skip+1)-th opportunity is taken
// (+eg2_trvk_fault_skip, random 0-15 otherwise), judged on the falling edge before the rising edge
// that samples it.
//
// Checks (EG2CHK, as chip_sw_eg2_boot_sec_base_vseq reports them):
//   - the injection took effect: in the forced cycle the filter hands the core a response with
//     tag 1 (upstream_rvalid_o && upstream_tag_o);
//   - detected: rv_core_ibex's fatal_core_err (alert_major_internal: the fatal_hw_err alert)
//     within the lockstep offset + 4 cycles (REQ_CIB_03: by the lockstep comparison if the TRVK
//     is inside the pair, by its own countermeasure otherwise).
// Detected, the CPU's fetch is off and the software cannot finish: the bench ends the run with its
// verdict. Not detected (the RTL of 2026-10-25: the TRVK is outside the pair, ibex_cheriot
// verification status/rtl_tickets.md "ibex_trvk sits outside the lockstep pair"), the check fails
// and the software, which also counts the tagged CLCs, finishes and fails as well.
// `trvk_fault_end` answers {detected[31], effective injections[15:0]}.
//
// +eg2_inject_fail=1 inverts the effectiveness check: `EG2CHK FAIL TRVK fault took effect`.
// Runs without the co-simulation (default build mode): it would rightly report the tagged
// capability, as the core bench's +disable_cosim=1 run does.
class chip_sw_eg2_trvk_fault_vseq extends chip_sw_eg2_revocation_bench_vseq;
  `uvm_object_utils(chip_sw_eg2_trvk_fault_vseq)
  `uvm_object_new

  localparam string RvCore = {PdMain, ".u_rv_core_ibex"};
  localparam string Trvk = {RvCore, ".u_core.gen_cheriot_trvk.i_ibex_trvk"};
  localparam string Lockstep = {RvCore, ".u_core.gen_lockstep.u_ibex_lockstep"};
  localparam int unsigned NumSites = 7;
  localparam string SiteName[NumSites] = '{"verdict", "tag_out", "bit_select", "req_required",
                                           "out_of_range", "sealing", "align"};
  localparam string SiteSignal[NumSites] = '{"revbm_revoked", "upstream_tag_o", "revbm_bit_select",
                                             "revbm_req_required", "revbm_out_of_range",
                                             "is_sealing_cap", "misalign_flag_out"};

  rand int unsigned site, skip, n_slots, n_rounds;
  constraint knobs_c {
    site < NumSites;
    skip inside {[0:15]};
    n_slots inside {[32:96]};
    n_rounds inside {[2:4]};
  }

  bit inject_fail;
  protected bit armed, stop, done, effective, detected;
  protected bit [31:0] range_lo, range_hi;
  protected int unsigned bench_checks, bench_fails;

  virtual task pre_start();
    void'($value$plusargs("eg2_inject_fail=%d", inject_fail));
    super.pre_start();
  endtask

  virtual function void write_params();
    string name;
    super.write_params();
    if ($value$plusargs("eg2_trvk_fault_site=%s", name)) begin
      site = NumSites;
      foreach (SiteName[i]) if (SiteName[i] == name) site = i;
      if (site == NumSites) `uvm_fatal(`gfn, $sformatf("unknown +eg2_trvk_fault_site=%s", name))
    end
    void'($value$plusargs("eg2_trvk_fault_skip=%d", skip));
    set_param("site", site);
    set_param("mark_all", site >= 3);
    set_param("n_slots", n_slots);
    set_param("n_rounds", n_rounds);
    `uvm_info(`gfn, $sformatf("EG2 TRVK fault: site %s (%s), opportunity %0d", SiteName[site],
                              SiteSignal[site], skip), UVM_LOW)
  endfunction

  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    result = 0;
    case (op)
      "trvk_fault_arm": begin
        range_lo = a;
        range_hi = b;
        armed = 1;
        fork injector(); join_none
      end
      "trvk_fault_end": begin
        stop = 1;
        if (!armed) done = 1;
        wait (done);
        if (!effective) `uvm_error(`gfn, "EG2CHK FAIL TRVK fault: no effective injection")
        result = (32'(detected) << 31) | 32'(effective);
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

  protected function uvm_hdl_data_t trvk(string sig);
    return rd({Trvk, ".", sig});
  endfunction

  // In the cycle about to be sampled: is the fault at the site effective, and with which value?
  protected function bit opportunity(bit prev_req, output uvm_hdl_data_t value);
    uvm_hdl_data_t base = trvk("cap_base");
    bit in_range = base >= range_lo && base < range_hi;
    bit first_req = trvk("revbm_req_o") && !trvk("revbm_outstanding_q") && !prev_req;
    value = 0;
    if (!in_range) return 0;
    case (site)
      0: begin value = 0; return trvk("upstream_tag_revoked_o"); end
      1: begin value = 1; return trvk("upstream_tag_revoked_o"); end
      2: begin
        uvm_hdl_data_t rdata, bs;
        if (!trvk("upstream_tag_revoked_o") || trvk("revbm_err_i") ||
            trvk("revbm_rsp_data_intg_error") != 0) return 0;
        rdata = trvk("revbm_rdata_i");
        bs = trvk("revbm_bit_select");
        for (int k = 0; k < 5; k++) begin
          if (!rdata[bs ^ (1 << k)]) begin value = bs ^ (1 << k); return 1; end
        end
        return 0;
      end
      3: begin value = 0; return first_req; end
      4: begin value = 1; return first_req; end
      5: begin value = 1; return first_req; end
      default: begin value = 0; return first_req; end
    endcase
  endfunction

  protected task injector();
    uvm_hdl_data_t value, offset;
    string path = {Trvk, ".", SiteSignal[site]};
    int unsigned seen, window;
    bit prev_req, through;
    if (!(uvm_hdl_read({Lockstep, ".LockstepOffset"}, offset))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({Lockstep, \".LockstepOffset\"}, offset)) %s", "LockstepOffset"))
    window = offset + 4;
    while (!stop) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      if (opportunity(prev_req, value)) begin
        if (seen == skip) break;
        seen++;
      end
      prev_req = trvk("revbm_req_o");
    end
    if (stop) begin
      `uvm_info(`gfn, $sformatf("EG2 TRVK fault at %s: %0d opportunities, fewer than %0d",
                                SiteName[site], seen, skip + 1), UVM_LOW)
      done = 1;
      return;
    end

    // TODO(inject-registry): announce this fault (source this vseq, target rv_core_ibex TRVK,
    // kind "TRVK fault <site>", now, one cycle; must be detected by fatal_core_err).
    `uvm_info("EG2_INJECT", $sformatf("EG2 fault: %s forced to 'h%0x for one cycle (opportunity %0d)",
                                      path, value, seen), UVM_LOW)
    `DV_CHECK_FATAL(uvm_hdl_force(path, value), path)
    #1ps;
    through = trvk("upstream_rvalid_o") && trvk("upstream_tag_o");
    cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
    `DV_CHECK_FATAL(uvm_hdl_release(path), path)
    effective = through;
    void'(bench_check("TRVK fault took effect", through ^ inject_fail,
                      $sformatf("(a revoked capability reached the core tagged, site %s)",
                                SiteName[site])));

    for (int unsigned i = 0; i < window && !detected; i++) begin
      cpu_clk();
      detected = rd({RvCore, ".fatal_core_err"});
    end
    void'(bench_check($sformatf("TRVK fault at %s detected", SiteName[site]), detected,
                      detected ? "(fatal_core_err)" :
                      $sformatf({"undetected: fatal_core_err stayed low for %0d cycles after a ",
                                 "revoked capability reached the core tagged (REQ_CIB_03)"},
                                window)));
    done = 1;
    if (detected) begin
      // The CPU's fetch is off (rv_core_ibex fatal_core_err): the bench ends the run.
      fork : isolation_fork
        begin
          fork
            wait (cfg.sw_test_status_vif.sw_test_done);
            #(2ms);
          join_any
          disable fork;
        end
      join
      if (!cfg.sw_test_status_vif.sw_test_done) begin
        `uvm_info(`gfn, $sformatf("EG2 bench: %0d checks, %0d failed", bench_checks,
                                  bench_fails), UVM_LOW)
        override_test_status_and_finish(.passed(bench_fails == 0));
      end
    end
  endtask
endclass
