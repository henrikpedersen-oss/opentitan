// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// rstmgr against the reset manager reference model (hw/top_earlgrey/dv/eg2_model/ip/
// eg2_rstmgr_core.h, through eg2_rstmgr_cmodel_pkg). On with +rstmgr_cmodel=1 (cfg.en_cmodel), next
// to rstmgr_scoreboard.
//
// The model sees only the block's ports and its TL-UL accesses, never a signal inside rstmgr:
//   the inputs as levels: por_n_i, rst_por_ni and rst_ni (the bench, like the top, wires rstmgr's
//     own por_io_div4 and lc_io_div4 leaves back into them: tb.sv), pwr_i (rst_lc_req,
//     rst_sys_req, rstreqs, reset_cause), scanmode_i / scan_rst_ni, alert_dump_i and cpu_dump_i
//     (as 32-bit words, the low word first);
//   every TL-UL write as it completes, every read's data.
// A read is compared against the model's bounds (eg2_rstmgr_dpi.cc: the bits that must read 1, the
// bits that may read 1); ALERT_INFO and CPU_INFO against the captured dump (eg2_rst_dump_word_ok).
// The documents give no cycle timing for the reset synchronizers, the POR stretcher or the
// recording, so the design counts as settled ("quiet") only when no input has changed for
// settle_aon always-on clock cycles; until then the fields hardware updates are not compared. While
// quiet, each always-on clock cycle compares the outputs: resets_o.rst_lc_n (both domains),
// rst_sys_n (main), the ten software-controllable leaves (main), pwr_o.rst_lc_src_n /
// rst_sys_src_n, and sw_rst_req_o.
class rstmgr_cmodel_sb extends uvm_component;
  `uvm_component_utils(rstmgr_cmodel_sb)

  rstmgr_env_cfg cfg;
  int unsigned   settle_aon = 64;  // +rstmgr_cmodel_settle: always-on cycles without a change
  // +rstmgr_cmodel_corrupt=1: the first read the model predicts exactly is expected with bit 0
  // inverted, so the run must fail.
  bit            corrupt;
  // +rstmgr_cmodel_corrupt_dump=1: the first crash dump word compared is taken with bit 0 inverted,
  // so the run must fail on the dump check.
  bit            corrupt_dump;
  int unsigned   hist_len = 24;    // +rstmgr_cmodel_history: model inputs kept for a mismatch report

  uvm_tlm_analysis_fifo #(tl_seq_item) tl_a_fifo;
  uvm_tlm_analysis_fifo #(tl_seq_item) tl_d_fifo;

  rstmgr_reg_cov cov;

  localparam int AlertWords = ($bits(alert_crashdump_t) + 31) / 32;
  localparam int CpuWords = ($bits(cpu_crash_dump_t) + 31) / 32;

  protected chandle      h;
  protected int unsigned quiet_cnt;
  protected bit          changed;
  // Quiet inputs only (no port change; register writes do not count): enough for the crash dump
  // words, which no write changes (reggen completes a write to a clk_por_i register only once it has
  // crossed, so the read after an INDEX write sees the new INDEX). 2026-11-07: with the full quiet
  // window every dump read followed an INDEX write and was skipped (dump_read_cg 6 %).
  protected int unsigned quiet_in_cnt;
  protected bit          in_changed;
  protected bit [31:0]   in_last;
  protected bit [AlertWords*32-1:0] alert_last;
  protected bit [CpuWords*32-1:0]   cpu_last;
  protected bit          sampled;
  protected int          tl_outstanding;

  protected int unsigned n_rd_cmp, n_rd_exact, n_rd_open, n_out_cmp, n_mismatch, n_writes;
  protected int unsigned n_inputs, n_settled, n_dump_cmp;

  function new(string name = "", uvm_component parent = null);
    super.new(name, parent);
  endfunction

  function void build_phase(uvm_phase phase);
    super.build_phase(phase);
    tl_a_fifo = new("tl_a_fifo", this);
    tl_d_fifo = new("tl_d_fifo", this);
    if (cfg.en_cov) cov = rstmgr_reg_cov::type_id::create("rstmgr_reg_cov");
    void'($value$plusargs("rstmgr_cmodel_settle=%d", settle_aon));
    void'($value$plusargs("rstmgr_cmodel_corrupt=%b", corrupt));
    void'($value$plusargs("rstmgr_cmodel_corrupt_dump=%b", corrupt_dump));
    void'($value$plusargs("rstmgr_cmodel_history=%d", hist_len));
    h = eg2_rstmgr_cmodel_pkg::eg2_rst_create();
  endfunction

  task run_phase(uvm_phase phase);
    fork
      watch_reset();
      forever begin
        @(posedge cfg.rstmgr_vif.clk_aon);
        sample();
        aon_tick();
      end
      // Reset inputs change between always-on clock edges too: a pulse shorter than an always-on
      // cycle on rst_por_ni or rst_ni must still reach the model.
      forever begin
        @(cfg.rstmgr_vif.por_n or
          cfg.rstmgr_vif.resets_o.rst_por_io_div4_n[rstmgr_pkg::DomainAonSel] or
          cfg.rstmgr_vif.resets_o.rst_lc_io_div4_n[rstmgr_pkg::DomainMainSel]);
        sample();
      end
      forever process_tl();
      forever process_tl_a();
    join_none
  endtask

  // rst_ni (the TL-UL interface's reset, tb.sv): requests in flight are gone.
  protected task watch_reset();
    forever begin
      @(negedge cfg.rstmgr_vif.resets_o.rst_lc_io_div4_n[rstmgr_pkg::DomainMainSel]);
      tl_a_fifo.flush();
      tl_d_fifo.flush();
      tl_outstanding = 0;
      changed = 1'b1;
      // OpenTitan's rstmgr_scoreboard writes ALERT_INFO_CTRL / CPU_INFO_CTRL back by backdoor from
      // its RAL mirror once this reset ends (restore_por_csr_fields): their values after a reset are
      // the bench's, not rstmgr's, so the model does not predict them until the next write.
      eg2_rstmgr_cmodel_pkg::eg2_rst_ctrl_unknown(h);
      if (cov != null) cov.reset();
    end
  endtask

  protected function bit quiet();
    return quiet_cnt >= settle_aon;
  endfunction

  // ---- Ports ----

  protected function void sample();
    bit por_ast, por, loc, scan;
    bit [1:0] lc, sys, cause;
    bit [4:0] rstreqs;
    bit [31:0] in_now;
    bit [AlertWords*32-1:0] alert_now;
    bit [CpuWords*32-1:0] cpu_now;
    // rst_por_ni / rst_ni as tb.sv connects them (rstmgr's own leaves, as on the chip).
    por_ast = cfg.rstmgr_vif.por_n !== '1;
    por     = por_ast || cfg.rstmgr_vif.resets_o.rst_por_io_div4_n[rstmgr_pkg::DomainAonSel] !== 1'b1;
    loc     = cfg.rstmgr_vif.resets_o.rst_lc_io_div4_n[rstmgr_pkg::DomainMainSel] !== 1'b1;
    lc      = cfg.rstmgr_vif.pwr_i.rst_lc_req;
    sys     = cfg.rstmgr_vif.pwr_i.rst_sys_req;
    rstreqs = cfg.rstmgr_vif.pwr_i.rstreqs;
    cause   = cfg.rstmgr_vif.pwr_i.reset_cause;
    scan    = cfg.rstmgr_vif.scanmode_i == prim_mubi_pkg::MuBi4True &&
              cfg.rstmgr_vif.scan_rst_ni === 1'b0;
    // Layout (rstmgr_reg_cov::sample_ports decodes it): por 0, scan 1, lc 3:2, sys 5:4,
    // rstreqs 10:6, cause 12:11, rst_ni low 13, por_n_i low 14.
    in_now = 32'({por_ast, loc, cause, rstreqs, sys, lc, scan, por});
    alert_now = (AlertWords*32)'(cfg.rstmgr_vif.alert_dump_i);
    cpu_now = (CpuWords*32)'(cfg.rstmgr_vif.cpu_dump_i);
    if (!sampled || alert_now !== alert_last) begin
      for (int i = 0; i < AlertWords; i++) begin
        eg2_rstmgr_cmodel_pkg::eg2_rst_set_dump(h, 1, AlertWords, i, alert_now[i*32 +: 32]);
      end
      alert_last = alert_now;
      changed = 1'b1;
      in_changed = 1'b1;
    end
    if (!sampled || cpu_now !== cpu_last) begin
      for (int i = 0; i < CpuWords; i++) begin
        eg2_rstmgr_cmodel_pkg::eg2_rst_set_dump(h, 0, CpuWords, i, cpu_now[i*32 +: 32]);
      end
      cpu_last = cpu_now;
      changed = 1'b1;
      in_changed = 1'b1;
    end
    if (!sampled || in_now !== in_last) begin
      changed = 1'b1;
      in_changed = 1'b1;
      n_inputs++;
      if (cov != null && sampled) cov.sample_ports(in_last, in_now);
      note($sformatf("inputs por_n_low %0b rst_por_low %0b rst_ni_low %0b lc_req %02b sys_req %02b rstreqs %05b cause %0d scan %0b",
                     por_ast, por, loc, lc, sys, rstreqs, cause, scan));
      eg2_rstmgr_cmodel_pkg::eg2_rst_inputs(h, por_ast, por, loc, lc, sys, rstreqs, cause, scan);
      in_last = in_now;
      sampled = 1'b1;
    end
  endfunction

  protected function void aon_tick();
    if (in_changed) begin
      quiet_in_cnt = 0;
      in_changed = 1'b0;
    end else if (quiet_in_cnt < settle_aon) begin
      quiet_in_cnt++;
    end
    if (changed) begin
      quiet_cnt = 0;
      changed = 1'b0;
      return;
    end
    if (quiet_cnt < settle_aon) begin
      quiet_cnt++;
      if (quiet_cnt == settle_aon) begin
        n_settled++;
        if (cov != null) cov.sample_settled(in_last);
      end
    end
    if (quiet() && tl_outstanding == 0) check_outputs();
  endfunction

  protected function void cmp(string what, int unsigned v, int unsigned lo, int unsigned hi);
    n_out_cmp++;
    if ((v & ~hi) != 0 || (lo & ~v) != 0) begin
      n_mismatch++;
      `uvm_error(`gfn, $sformatf("%0s: rstmgr 0x%0h, model must 0x%0h may 0x%0h%0s", what, v, lo,
                                 hi, n_mismatch <= 5 ? history() : ""))
    end
  endfunction

  protected function void check_outputs();
    int unsigned lc_lo, lc_hi, sys_lo, sys_hi, sw_lo, sw_hi, req_lo, req_hi;
    bit [9:0] sw;
    rstmgr_pkg::rstmgr_out_t r = cfg.rstmgr_vif.resets_o;
    eg2_rstmgr_cmodel_pkg::eg2_rst_outputs(h, lc_lo, lc_hi, sys_lo, sys_hi, sw_lo, sw_hi, req_lo,
                                           req_hi);
    // The software-controllable leaves in SW_RST_CTRL_N order (rstmgr.hjson's SW_RST
    // countermeasures), main domain.
    sw = {r.rst_i3c1_n[1], r.rst_i3c0_n[1], r.rst_i2c2_n[1], r.rst_i2c1_n[1], r.rst_i2c0_n[1],
          r.rst_usb_aon_n[1], r.rst_usb_n[1], r.rst_spi_host1_n[1], r.rst_spi_host0_n[1],
          r.rst_spi_device_n[1]};
    cmp("resets_o.rst_lc_n", r.rst_lc_n, lc_lo, lc_hi);
    cmp("resets_o.rst_sys_n[main]", r.rst_sys_n[1], sys_lo[1], sys_hi[1]);
    cmp("resets_o software-controlled leaves", sw, sw_lo, sw_hi);
    cmp("pwr_o.rst_lc_src_n", cfg.rstmgr_vif.pwr_o.rst_lc_src_n, lc_lo, lc_hi);
    cmp("pwr_o.rst_sys_src_n", cfg.rstmgr_vif.pwr_o.rst_sys_src_n, sys_lo, sys_hi);
    cmp("sw_rst_req_o is true", cfg.rstmgr_vif.sw_rst_req_o == prim_mubi_pkg::MuBi4True, req_lo,
        req_hi);
  endfunction

  // ---- Registers ----

  protected task process_tl();
    tl_seq_item  it;
    int unsigned addr, lo, hi, d;
    bit          q;
    tl_d_fifo.get(it);
    if (tl_outstanding > 0) tl_outstanding--;
    if (cfg.rstmgr_vif.resets_o.rst_lc_io_div4_n[rstmgr_pkg::DomainMainSel] !== 1'b1 ||
        it.d_error) return;
    addr = it.a_addr & 'h7c;
    q = quiet();
    if (it.is_write()) begin
      n_writes++;
      if (cov != null) cov.sample_write(addr, it.a_data, read_now('h0c), read_now('h1c));
      eg2_rstmgr_cmodel_pkg::eg2_rst_csr_write(h, addr, it.a_data);
      note($sformatf("write 0x%02h = 0x%08h%0s", addr, it.a_data, q ? "" : " (settling)"));
      changed = 1'b1;
      return;
    end
    d = it.d_data;
    if (cov != null && !(addr inside {'h18, 'h28})) cov.sample_read(addr, d, q);
    if (addr inside {'h18, 'h28}) begin
      int unsigned idx = (read_now(addr == 'h18 ? 'h10 : 'h20) >> 4) & 'hf;
      q = quiet_in_cnt >= settle_aon;
      if (cov != null) cov.sample_read(addr, d, q);
      if (q) n_dump_cmp++;
      if (q && corrupt_dump) begin
        d ^= 1;
        corrupt_dump = 1'b0;
      end
      if (!eg2_rstmgr_cmodel_pkg::eg2_rst_dump_word_ok(h, addr == 'h18, idx, d, q)) begin
        n_mismatch++;
        `uvm_error(`gfn, $sformatf("read 0x%02h (%0s) INDEX %0d: rstmgr 0x%08h is not the captured dump's word%0s",
                                   addr, reg_name(addr), idx, d, n_mismatch <= 5 ? history() : ""))
      end
      note($sformatf("read 0x%02h INDEX %0d: rstmgr 0x%08h%0s", addr, idx, d,
                     q ? "" : " (settling)"));
      return;
    end
    eg2_rstmgr_cmodel_pkg::eg2_rst_csr_read(h, addr, q, lo, hi);
    note($sformatf("read 0x%02h: rstmgr 0x%08h, model must 0x%08h may 0x%08h%0s", addr, d, lo, hi,
                   q ? "" : " (settling)"));
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
      `uvm_error(`gfn, $sformatf("read 0x%02h (%0s): rstmgr 0x%08h, model must 0x%08h may 0x%08h%0s",
                                 addr, reg_name(addr), d, lo, hi, n_mismatch <= 5 ? history() : ""))
    end
  endtask

  protected task process_tl_a();
    tl_seq_item it;
    tl_a_fifo.get(it);
    tl_outstanding++;
    if (it.is_write()) changed = 1'b1;
  endtask

  protected function int unsigned read_now(int unsigned addr);
    return eg2_rstmgr_cmodel_pkg::eg2_rst_csr_value(h, addr);
  endfunction

  protected static function string reg_name(int unsigned addr);
    case (addr)
      'h00: return "ALERT_TEST";
      'h04: return "RESET_REQ";
      'h08: return "RESET_INFO";
      'h0c: return "ALERT_REGWEN";
      'h10: return "ALERT_INFO_CTRL";
      'h14: return "ALERT_INFO_ATTR";
      'h18: return "ALERT_INFO";
      'h1c: return "CPU_REGWEN";
      'h20: return "CPU_INFO_CTRL";
      'h24: return "CPU_INFO_ATTR";
      'h28: return "CPU_INFO";
      'h7c: return "ERR_CODE";
      default: begin
        if (addr >= 'h2c && addr < 'h54) return $sformatf("SW_RST_REGWEN_%0d", (addr - 'h2c) / 4);
        if (addr >= 'h54 && addr < 'h7c) return $sformatf("SW_RST_CTRL_N_%0d", (addr - 'h54) / 4);
        return "?";
      end
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
    if (n_rd_cmp == 0) `uvm_error(`gfn, "the model compared no register read")
    if (n_settled == 0) `uvm_error(`gfn, "the design never settled: the model compared no quiet state")
  endfunction

  function void report_phase(uvm_phase phase);
    `uvm_info(`gfn, $sformatf({"rstmgr cmodel: compared reads %0d (exact %0d, with open bits %0d), ",
                               "dump words %0d, output samples %0d; mismatches %0d; writes %0d, ",
                               "input changes %0d, settled %0d times, resets %0d, power-on resets %0d"},
                              n_rd_cmp, n_rd_exact, n_rd_open, n_dump_cmp, n_out_cmp, n_mismatch,
                              n_writes, n_inputs, n_settled,
                              eg2_rstmgr_cmodel_pkg::eg2_rst_resets(h),
                              eg2_rstmgr_cmodel_pkg::eg2_rst_pors(h)), UVM_NONE)
    if (h != null) eg2_rstmgr_cmodel_pkg::eg2_rst_release(h);
    h = null;
  endfunction
endclass
