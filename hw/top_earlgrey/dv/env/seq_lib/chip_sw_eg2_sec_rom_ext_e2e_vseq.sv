// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_eg2_sec_e2e_<case>_<variation>[_cheriot] (EG2 spec REQ_SEC_15, REQ_SEC_11, REQ_PBF_14):
// ROM_EXT's `attestation` and `dice_chain` end-to-end suites (sw/device/silicon_creator/rom_ext/e2e,
// sources unchanged; images in sw/device/tests/eg2_sec/BUILD) behind the full secure-boot chain.
// The bench makes the FPGA host's checks it can make:
//
//   +eg2_e2e_suite=print_certs   attestation/print_certs_test. The device prints each certificate
//                                of the DICE page as `<name>: <base64>` (LOG_INFO "%s: %s"); both
//                                strings are in RAM, which the DV log prints as addresses, so the
//                                bench reads them from main SRAM when the line is printed. CDI_0 and
//                                CDI_1 must be there, valid base64, and encoded as the variation's
//                                format says: X.509 DER (a SEQUENCE, 0x30) or a CWT COSE_Sign1 (a
//                                CBOR array of 4, 0x84, dice_cwt.c). They are written to
//                                eg2_attestation_certs.txt in the run directory in the FPGA
//                                console's format. Their signatures and contents against the key
//                                manager are the host's step (sw/host/tests/attestation), which the
//                                bench does not make (status/dv_todo.md).
//   +eg2_e2e_suite=no_refresh    dice_chain/no_refresh_<variation>_test. The FPGA's exit_failure:
//                                ROM_EXT's `warning: CDI_1 certificate not valid; updating` on
//                                UART0 between the image's `Rebooting` and `Rebooted`; it must not
//                                be there (the certificate made in the first boot is still valid).
//   +eg2_e2e_suite=debug_mode    dice_chain/debug_mode_{off,on}_<variation>_test: self-checking (the
//                                debug flag in CDI_1 against the owner key's domain); the bench
//                                checks the boot only.
//
// Every run: no shutdown code (`BFV:`) on the way to the owner image, the owner image ran in the
// mode +eg2_boot_cheriot says, and the mode is recorded (`EG2 attestation device mode`). Whether
// the CHERIoT mode is part of the attested device mode is open (decision 9): it is recorded, not
// asserted.
//
// +eg2_inject_fail=1 (the must-fail entries) inverts the suite's own bench check: print_certs
// `EG2CHK FAIL CDI_1 certificate printed`, no_refresh `EG2CHK FAIL CDI_1 not regenerated after the
// reboot`, debug_mode `EG2CHK FAIL owner image in the expected mode`.
class chip_sw_eg2_sec_rom_ext_e2e_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_rom_ext_e2e_vseq)

  `uvm_object_new

  string suite = "";
  string variation = "x509";
  bit exp_cheriot;
  // The certificates as printed: name -> base64.
  string certs[string];
  string cert_order[$];
  // UART0's line count when the image printed `Rebooting` (-1: never), and `Rebooted` seen.
  int reboot_mark = -1;
  bit rebooted;
  localparam int unsigned MaxStrBytes = 4096;

  virtual task pre_start();
    void'($value$plusargs("eg2_e2e_suite=%s", suite));
    void'($value$plusargs("eg2_e2e_variation=%s", variation));
    void'($value$plusargs("eg2_boot_cheriot=%d", exp_cheriot));
    // (No string literal inside a DV_CHECK condition: Xcelium rejects it, *E,INSSTR.)
    if (!(suite inside {"print_certs", "no_refresh", "debug_mode"})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_e2e_suite=%s: print_certs, no_refresh or debug_mode", suite))
    end
    if (!(variation inside {"x509", "cwt"})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_e2e_variation=%s: x509 or cwt", variation))
    end
    super.pre_start();
  endtask

  virtual task body();
    super.body();
    fork
      watch_cheriot_mode();
      watch_sw_log();
    join_none
  endtask

  // A NUL-terminated string at main SRAM address `addr`, through the current scrambling key.
  protected function string ram_str(bit [31:0] addr);
    string s = "";
    bit [31:0] base = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
    bit [31:0] size = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES;
    bit [31:0] w;
    if (addr < base || addr >= base + size) return "";
    for (int unsigned i = 0; i < MaxStrBytes && addr + i < base + size; i++) begin
      bit [31:0] a = addr + i;
      bit [7:0] c;
      if (i == 0 || a % 4 == 0) w = sram_rd32(1, (a - base) & ~32'h3);
      c = w[8 * (a % 4) +: 8];
      if (c == 0) break;
      s = {s, string'(c)};
    end
    return s;
  endfunction

  // The address a `%s` argument held: the DV log prints a RAM string as its address in hex.
  protected function bit [31:0] arg_addr(int idx);
    string a = string'(cfg.sw_logger_vif.printed_arg[idx]);
    return a.atohex();
  endfunction

  protected task watch_sw_log();
    forever begin
      string fmt, log;
      @(cfg.sw_logger_vif.printed_log_event);
      fmt = string'(cfg.sw_logger_vif.printed_format);
      log = string'(cfg.sw_logger_vif.printed_log);
      if (fmt == "%s: %s" && cfg.sw_logger_vif.printed_arg.size() >= 2) begin
        string name = ram_str(arg_addr(0));
        string b64 = ram_str(arg_addr(1));
        if (!certs.exists(name)) cert_order.push_back(name);
        certs[name] = b64;
        `uvm_info(`gfn, $sformatf("EG2 attestation: %s, %0d base64 characters", name, b64.len()),
                  UVM_LOW)
      end else if (log == "Rebooting") begin
        reboot_mark = uart_lines.size();
      end else if (log == "Rebooted") begin
        rebooted = 1'b1;
      end
    end
  endtask

  // The base64 alphabet's value of character code `c`, -1 outside it.
  protected function int b64_value(bit [7:0] c);
    if (c >= 8'h41 && c <= 8'h5a) return int'(c) - 8'h41;       // A-Z
    if (c >= 8'h61 && c <= 8'h7a) return int'(c) - 8'h61 + 26;  // a-z
    if (c >= 8'h30 && c <= 8'h39) return int'(c) - 8'h30 + 52;  // 0-9
    if (c == 8'h2b) return 62;                                   // +
    if (c == 8'h2f) return 63;                                   // /
    return -1;
  endfunction

  // Valid base64 (length a multiple of 4, only the alphabet and at most two trailing `=`);
  // `first` gets the first decoded byte.
  protected function bit b64_ok(string s, output bit [7:0] first);
    int pad;
    first = 0;
    if (s.len() == 0 || s.len() % 4 != 0) return 0;
    for (int i = 0; i < s.len(); i++) begin
      bit [7:0] c = s[i];
      if (c == 8'h3d) begin  // =
        if (i < s.len() - 2) return 0;
        pad++;
      end else if (pad != 0 || b64_value(c) < 0) begin
        return 0;
      end
    end
    first = 8'((b64_value(s[0]) << 2) | (b64_value(s[1]) >> 4));
    return 1;
  endfunction

  protected function void check_certs();
    string names[$] = '{"CDI_0", "CDI_1"};
    bit [7:0] magic = variation == "x509" ? 8'h30 : 8'h84;
    int fd;
    foreach (names[i]) begin
      bit [7:0] first;
      bit present = certs.exists(names[i]) && certs[names[i]].len() > 0;
      bit flip = inject_fail && names[i] == "CDI_1";
      if (flip) announce(`gfn, "CDI_1 certificate printed", "+eg2_inject_fail: expected absent");
      if (!check($sformatf("%s certificate printed", names[i]), present ^ flip)) continue;
      if (!present) continue;
      void'(check($sformatf("%s certificate is valid base64", names[i]),
                  b64_ok(certs[names[i]], first)));
      void'(check($sformatf("%s certificate encoded as %s", names[i], variation), first == magic,
                  $sformatf("first byte 0x%02x, expected 0x%02x", first, magic)));
    end
    fd = $fopen("eg2_attestation_certs.txt", "w");
    if (fd) begin
      foreach (cert_order[i]) $fwrite(fd, "%s: %s\n", cert_order[i], certs[cert_order[i]]);
      $fclose(fd);
      `uvm_info(`gfn, $sformatf("EG2 attestation: %0d certificates in eg2_attestation_certs.txt",
                                cert_order.size()), UVM_LOW)
    end
  endfunction

  protected function void check_no_refresh();
    int warn = reboot_mark >= 0 ? find_uart_line("warning: CDI_1 certificate not valid",
                                                 reboot_mark) : -1;
    if (inject_fail) announce(`gfn, "CDI_1 not regenerated", "+eg2_inject_fail: expected again");
    void'(check("the image rebooted itself", reboot_mark >= 0 && rebooted));
    void'(check("CDI_1 not regenerated after the reboot", (warn < 0) ^ inject_fail,
                warn >= 0 ? uart_lines[warn] : ""));
  endfunction

  virtual task post_start();
    bit flip = inject_fail && suite == "debug_mode";
    super.post_start();
    void'(check("no shutdown code on the way to the owner image", find_uart_line("BFV:") < 0,
                find_uart_line("BFV:") >= 0 ? uart_lines[find_uart_line("BFV:")] : ""));
    if (flip) announce(`gfn, "mode", "+eg2_inject_fail: the other mode expected");
    void'(check("owner image in the expected mode", cheriot_seen == (exp_cheriot ^ flip),
                $sformatf("(CHERIoT mode seen %0d)", cheriot_seen)));
    `uvm_info(`gfn, $sformatf({"EG2 attestation device mode: owner image in %s mode, ROM_EXT DICE ",
                               "%s. Decision 9 (is the CHERIoT mode part of the attested device ",
                               "mode?) is open: recorded, not asserted"},
                              cheriot_seen ? "CHERIoT" : "RISC-V", variation), UVM_LOW)
    case (suite)
      "print_certs": check_certs();
      "no_refresh": check_no_refresh();
      default: ;
    endcase
    `uvm_info(`gfn, $sformatf("EG2 bench: %0d checks, %0d failed", bench_checks, bench_fails),
              UVM_LOW)
  endtask

endclass : chip_sw_eg2_sec_rom_ext_e2e_vseq
