// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The boot outcome of a secure-boot run (EG2 verification spec REQ_BOOT_02, 06, 08, 09, 11, 12,
// 13, 14; software in sw/device/tests/eg2_boot and eg2_sec). Every test behind the full chain (ROM,
// ROM_EXT, owner image; run mode sw_test_mode_mask_rom, image flag `rom_ext`) without a bench
// action of its own runs under this sequence; the dvsim entry states the outcome the documents
// give its images:
//
//   +eg2_boot_expect=boot     (default) the owner image runs and passes; no shutdown code (`BFV:`)
//                             is printed on UART0 on the way;
//   +eg2_boot_expect=refuse   the boot fails: a shutdown code (`BFV:<code>`, shutdown.c /
//                             rom_ext.c) is printed on UART0, the owner image never starts its
//                             test (the DV status never reaches "in test"), and the core never
//                             reaches CHERIoT mode (REQ_BOOT_02, REQ_BOOT_11);
//   +eg2_boot_refused_by=rom|rom_ext  with refuse: the stage that refuses. The ROM refuses
//                             ROM_EXT before it runs (no `ROM_EXT:` banner, rom_ext.c), ROM_EXT
//                             refuses the owner image after its banner;
//   +eg2_boot_cheriot=1       with boot: the core was seen in CHERIoT mode (the owner image
//                             switched and locked it, REQ_BOOT_02).
//
// The shutdown code is logged, not compared: the ROM and ROM_EXT try both slots, and the code
// they print is the last slot's (an empty slot B reports a bad identifier), so it does not name
// the corruption of slot A.
//
// +eg2_inject_fail=1 (the bench's must-fail entries) inverts the expected stage of a refusal, or
// the expected mode of a boot: the run must fail with `EG2CHK FAIL refused by <stage>` or
// `EG2CHK FAIL owner image in CHERIoT mode`.
class chip_sw_eg2_boot_outcome_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_boot_outcome_vseq)

  `uvm_object_new

  string expect_outcome = "boot";
  string refused_by = "rom_ext";
  bit exp_cheriot;
  // How long the bench watches after the shutdown code for an image that must not start.
  int unsigned settle_ns = 500_000;
  // +eg2_boot_final_log=<boots>:<slots>: the boot_svc end-to-end tests' expected state (below).
  string final_log;
  localparam int unsigned RetOwner = 2048;

  virtual task pre_start();
    void'($value$plusargs("eg2_boot_expect=%s", expect_outcome));
    void'($value$plusargs("eg2_boot_refused_by=%s", refused_by));
    void'($value$plusargs("eg2_boot_cheriot=%d", exp_cheriot));
    void'($value$plusargs("eg2_boot_final_log=%s", final_log));
    // (No string literal inside a DV_CHECK condition: Xcelium rejects it, *E,INSSTR.)
    if (!(expect_outcome inside {"boot", "refuse"})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_boot_expect=%s: boot or refuse", expect_outcome))
    end
    if (!(refused_by inside {"rom", "rom_ext"})) begin
      `uvm_fatal(`gfn, $sformatf("+eg2_boot_refused_by=%s: rom or rom_ext", refused_by))
    end
    super.pre_start();
  endtask

  virtual task body();
    super.body();
    fork
      watch_cheriot_mode();
    join_none
    if (expect_outcome == "refuse") check_refused();
  endtask

  protected task check_refused();
    int bfv, banner;
    bit by_rom_ext, exp_by_rom_ext;
    wait_uart_line("BFV:", bfv, cfg.sw_test_timeout_ns);
    void'(check("boot refused with a shutdown code on UART0", bfv >= 0,
                bfv >= 0 ? uart_lines[bfv] : "no BFV line"));
    // Anything the image could still do after the code.
    #(settle_ns * 1ns);
    banner = find_uart_line("ROM_EXT:");
    by_rom_ext = banner >= 0 && (bfv < 0 || banner < bfv);
    exp_by_rom_ext = (refused_by == "rom_ext") ^ inject_fail;
    if (inject_fail) announce(`gfn, "refused by", "+eg2_inject_fail: the other stage expected");
    void'(check($sformatf("refused by %s", exp_by_rom_ext ? "ROM_EXT" : "the ROM"),
                by_rom_ext == exp_by_rom_ext,
                $sformatf("(ROM_EXT banner %s the shutdown code)",
                          by_rom_ext ? "before" : "not before")));
    void'(check("the refused image never started its test",
                !(cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusInTest,
                                                                 SwTestStatusPassed}),
                cfg.sw_test_status_vif.sw_test_status.name()));
    void'(check("the mode stays RISC-V", !cheriot_seen));
    finish_with_bench_verdict();
  endtask

  virtual task post_start();
    super.post_start();
    if (expect_outcome != "boot") return;
    void'(check("no shutdown code on the way to the owner image", find_uart_line("BFV:") < 0,
                find_uart_line("BFV:") >= 0 ? uart_lines[find_uart_line("BFV:")] : ""));
    if (final_log != "") check_final_log();
    if (exp_cheriot || (inject_fail && final_log == "")) begin
      bit flip = inject_fail && final_log == "";
      if (flip) announce(`gfn, "mode", "+eg2_inject_fail: the other mode expected");
      void'(check("owner image in CHERIoT mode", cheriot_seen == (exp_cheriot ^ flip)));
    end
  endtask

  // ROM_EXT's boot_svc end-to-end tests (rom_ext/e2e/boot_svc, chip_sw_eg2_boot_svc_e2e_*): their
  // FPGA pass line is `FinalBootLog: <boots>:<slot of each boot>`. The slots are a RAM string, which
  // the DV log prints as an address, so the bench reads the test's state where the test keeps it:
  // boot_svc_retram_t (boot_svc_test_lib.h) at the start of the retention SRAM's owner area
  // (retention_sram.h: offset 2048): `boots` at +8, `partition[10]` at +14.
  // +eg2_inject_fail=1 expects another value: `EG2CHK FAIL boot_svc FinalBootLog`.
  protected function void check_final_log();
    bit [31:0] boots = sram_rd32(0, RetOwner + 8);
    bit [31:0] w;
    string got = $sformatf("%0d:", boots);
    for (int i = 0; i < 10 && i < boots; i++) begin
      w = sram_rd32(0, RetOwner + ((14 + i) & ~3));
      got = {got, string'(w[8 * ((14 + i) % 4) +: 8])};
    end
    if (inject_fail) announce(`gfn, "boot_svc FinalBootLog", "+eg2_inject_fail: another value");
    void'(check("boot_svc FinalBootLog", (got == final_log) ^ inject_fail,
                $sformatf("exp %s got %s", final_log, got)));
  endfunction

endclass : chip_sw_eg2_boot_outcome_vseq
