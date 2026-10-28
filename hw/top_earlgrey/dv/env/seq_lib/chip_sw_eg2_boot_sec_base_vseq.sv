// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The base of the EG2 secure-boot and security tests' sequences (software in
// sw/device/tests/eg2_boot and eg2_sec; their library eg2_boot.h).
//
// The software and the bench meet at words in the software's RAM (`volatile uint32_t kEg2...`
// globals): the bench finds them by symbol in the test image and reads or writes them by backdoor,
// descrambled with the key and nonce main SRAM uses now (sram_ctrl key_q / nonce_q: the mask ROM
// renews the key at boot, CREATOR_SW_CFG_SRAM_KEY_RENEW_AND_INIT_EN), so this works behind secure
// boot too, where the image is signed and its constants cannot be written.
//
// Checks the bench makes report `EG2CHK FAIL <what>` as UVM errors, so an `_inject_fail` run's
// stated message is the same whether the software or the bench made the failing check.
// +eg2_inject_fail=1 breaks one expectation of the bench's (each sequence says which).
// Deliberate faults are announced (announce) to the EG2 injection registry when it is compiled in
// (EG2_PERIPH_SB, decision 2026-10-24) and to the log in any case.
class chip_sw_eg2_boot_sec_base_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_boot_sec_base_vseq)

  `uvm_object_new

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam string PdAon = "tb.dut.top_earlgrey.earlgrey_pd_aon";
  localparam string CheriotEnaPath = {PdMain, ".u_rv_core_ibex.cheriot_enable_ibex"};

  // +eg2_inject_fail=1: break one bench expectation (the `_inject_fail` dvsim entries).
  bit inject_fail;
  // Set when the core was seen in CHERIoT mode (watch_cheriot_mode).
  bit cheriot_seen;
  // Bench checks made and failed.
  int unsigned bench_checks, bench_fails;

  // The JTAG TAP strapped before reset: none, the debug module (SBA) or lc_ctrl (its registers).
  typedef enum {TapNone, TapRvDm, TapLc} tap_e;
  tap_e tap = TapNone;

  // UART0's lines so far (ROM, ROM_EXT and the test print there); `uart_line_ev` on each.
  string uart_lines[$];
  event uart_line_ev;

  virtual task pre_start();
    void'($value$plusargs("eg2_inject_fail=%d", inject_fail));
    case (tap)
      TapRvDm: cfg.chip_vif.tap_straps_if.drive(JtagTapRvDm);
      TapLc: cfg.chip_vif.tap_straps_if.drive(JtagTapLc);
      default: ;
    endcase
    super.pre_start();
  endtask

  // As chip_sw_eg2_base_vseq::body, with UART0's lines kept (uart_lines) for the boot-outcome
  // checks: the ROM's and ROM_EXT's banners and shutdown codes are UART0 lines.
  // The console stays on UART0 here (+eg2_dv_console=1 still moves it): the ROM's and ROM_EXT's
  // lines, which the boot checks read, are UART0 lines, and the test's lines keep their order
  // among them.
  virtual task body();
    configure_uart_agent(.uart_idx(0), .enable(1));
    cfg.eg2_sb_uart_required[0] = 1'b1;
    dv_console = 1'b0;
    fork
      forward_dv_console();
      keep_uart_lines();
      log_boot_profile();
    join_none
    chip_sw_base_vseq::body();
  endtask

  protected task keep_uart_lines();
    uart_item item;
    string line;
    console_line_t cl;
    forever begin
      p_sequencer.uart_tx_fifos[0].get(item);
      if (console_char(item, cl, line)) begin
        log_console_line(line, cl);
        uart_lines.push_back(line);
        ->uart_line_ev;
      end
    end
  endtask

  // Index of the first UART0 line from `from` on that starts with `prefix`, or -1.
  protected function int find_uart_line(string prefix, int from = 0);
    for (int i = from; i < uart_lines.size(); i++) begin
      if (uart_lines[i].len() >= prefix.len() &&
          uart_lines[i].substr(0, prefix.len() - 1) == prefix) begin
        return i;
      end
    end
    return -1;
  endfunction

  // Waits for a UART0 line that starts with `prefix` (index in `idx`, -1 on timeout or when the
  // software ended first).
  protected task wait_uart_line(string prefix, output int idx,
                                input int unsigned timeout_ns = 200_000_000);
    idx = find_uart_line(prefix);
    if (idx >= 0) return;
    fork : isolation_fork
      begin
        fork
          forever begin
            @(uart_line_ev);
            idx = find_uart_line(prefix);
            if (idx >= 0) break;
          end
          #(timeout_ns * 1ns);
          wait (cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusPassed,
                                                              SwTestStatusFailed});
        join_any
        disable fork;
      end
    join
  endtask

  // The run ends with the bench's verdict (for a run whose software never ends, or never runs).
  protected function void finish_with_bench_verdict();
    `uvm_info(`gfn, $sformatf("EG2 bench: %0d checks, %0d failed", bench_checks, bench_fails),
              UVM_LOW)
    if (bench_checks == 0) begin
      `uvm_error(`gfn, "EG2CHK FAIL the bench made no check")
    end
    override_test_status_and_finish(.passed(bench_fails == 0 && bench_checks > 0));
  endfunction

  // ----------------------------------------------------------------------------------------------
  // Checks

  protected function bit check(string what, bit cond, string detail = "");
    bench_checks++;
    if (cond) begin
      `uvm_info(`gfn, $sformatf("EG2CHK ok %s %s", what, detail), UVM_LOW)
    end else begin
      bench_fails++;
      `uvm_error(`gfn, $sformatf("EG2CHK FAIL %s %s", what, detail))
    end
    return cond;
  endfunction

  protected function void announce(string source, string targets, string kind);
    // Every build (2026-10-28): outside an EG2 periph run only the insn check consults it.
    void'(eg2_inject_registry::announce(source, targets, kind));
    `uvm_info("EG2_INJECT", $sformatf("%s: %s on %s", source, kind, targets), UVM_LOW)
  endfunction

  // ----------------------------------------------------------------------------------------------
  // Main and retention SRAM through their current scrambling key

  protected function void sram_key(bit is_main, output bit [127:0] key, output bit [63:0] nonce);
    string ctrl = is_main ? {PdMain, ".u_sram_ctrl_main"} : {PdAon, ".u_sram_ctrl_ret"};
    bit [otp_ctrl_pkg::SramKeyWidth-1:0]   k;
    bit [otp_ctrl_pkg::SramNonceWidth-1:0] n;
    if (!(uvm_hdl_read({ctrl, ".key_q"}, k))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({ctrl, \".key_q\"}, k)) %s", ""))
    if (!(uvm_hdl_read({ctrl, ".nonce_q"}, n))) `uvm_fatal(`gfn, $sformatf("Check failed (uvm_hdl_read({ctrl, \".nonce_q\"}, n)) %s", ""))
    key = k;
    nonce = n[63:0];
  endfunction

  // A 32-bit word at byte `offset` of main SRAM (is_main) or retention SRAM, descrambled.
  protected function bit [31:0] sram_rd32(bit is_main, bit [31:0] offset);
    sram_ctrl_bkdr_util s;
    bit [127:0] key;
    bit [63:0] nonce;
    sram_key(is_main, key, nonce);
    `downcast(s, cfg.mem_bkdr_util_h[is_main ? RamMain0 : RamRet0])
    return s.sram_encrypt_read32_integ(offset, key, nonce, 0);
  endfunction

  protected function void sram_wr32(bit is_main, bit [31:0] offset, bit [31:0] data);
    sram_ctrl_bkdr_util s;
    bit [127:0] key;
    bit [63:0] nonce;
    sram_key(is_main, key, nonce);
    `downcast(s, cfg.mem_bkdr_util_h[is_main ? RamMain0 : RamRet0])
    s.sram_encrypt_write32_integ(offset, data, key, nonce, 0);
    // The instruction-set model reads its own memory since review R6 (2026-10-25).
    eg2_iss_wrapper::backdoor_write32((is_main ? top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR
                                               : top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_BASE_ADDR) +
                                      offset, data);
  endfunction

  // The main SRAM offset of software symbol `symbol` (a global in RAM) and its size.
  // Symbols looked up once (the ELF is parsed per lookup).
  protected bit [bus_params_pkg::BUS_AW-1:0] sym_addr_cache[string];
  protected uint sym_size_cache[string];

  protected function bit [31:0] sym_offset(string symbol, output uint size);
    bit [bus_params_pkg::BUS_AW-1:0] addr;
    string image = $sformatf("%0s.elf", cfg.sw_images[SwTypeTestSlotA]);
    if (!sym_addr_cache.exists(symbol)) begin
      bit found = dv_utils_pkg::sw_symbol_get_addr_size(image, symbol, 0, addr, size);
      if (!found) `uvm_fatal(`gfn, $sformatf("no symbol %s in %s", symbol, image))
      sym_addr_cache[symbol] = addr;
      sym_size_cache[symbol] = size;
    end
    addr = sym_addr_cache[symbol];
    size = sym_size_cache[symbol];
    `DV_CHECK_FATAL(addr >= top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR &&
                    addr < top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR +
                           top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES,
                    $sformatf("symbol %s at 0x%0h is not in main SRAM", symbol, addr))
    return addr - top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  endfunction

  // Word `idx` of the software's RAM global `symbol`.
  protected function bit [31:0] sym_rd32(string symbol, int idx = 0);
    uint size;
    bit [31:0] off = sym_offset(symbol, size);
    return sram_rd32(1, off + 4 * idx);
  endfunction

  protected function void sym_wr32(string symbol, int idx, bit [31:0] value);
    uint size;
    bit [31:0] off = sym_offset(symbol, size);
    sram_wr32(1, off + 4 * idx, value);
    `uvm_info(`gfn, $sformatf("EG2 %s[%0d] = 0x%08h", symbol, idx, value), UVM_LOW)
  endfunction

  // Waits until word `idx` of `symbol` reads `value`; false on timeout or if the software ended.
  protected task wait_sym(string symbol, int idx, bit [31:0] value, output bit found,
                          input int unsigned timeout_ns = 200_000_000, int unsigned poll_ns = 2000);
    found = 1'b0;
    for (int unsigned t = 0; t < timeout_ns; t += poll_ns) begin
      #(poll_ns * 1ns);
      if (cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest &&
          sym_rd32(symbol, idx) == value) begin
        found = 1'b1;
        return;
      end
      if (cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusPassed,
                                                        SwTestStatusFailed}) begin
        return;
      end
    end
  endtask

  // ----------------------------------------------------------------------------------------------
  // CHERIoT mode at the core

  protected task watch_cheriot_mode();
    forever begin
      bit [3:0] ena;
      #1us;
      if (uvm_hdl_read(CheriotEnaPath, ena) && ena == 4'b0101) cheriot_seen = 1'b1;
    end
  endtask

  // ----------------------------------------------------------------------------------------------
  // SBA through the JTAG debugger model (the RV_DM TAP strapped by the test's pre_start)

  protected task sba_wr32(bit [31:0] addr, bit [31:0] data, output sba_access_err_e status);
    logic [31:0] q[$];
    q.push_back(data);
    cfg.debugger.sba_write(addr, q, status);
    // The instruction-set model reads its own memory since review R6 (2026-10-25).
    if (status == SbaErrNone) eg2_iss_wrapper::backdoor_write32(addr, data);
  endtask

  protected task sba_rd32(bit [31:0] addr, output bit [31:0] data, output sba_access_err_e status);
    logic [31:0] q[$];
    cfg.debugger.sba_read(addr, q, status);
    data = q.size() > 0 ? q[0] : '0;
  endtask

endclass : chip_sw_eg2_boot_sec_base_vseq
