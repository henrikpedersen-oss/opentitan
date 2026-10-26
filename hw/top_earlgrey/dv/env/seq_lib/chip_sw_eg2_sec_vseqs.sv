// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The sequences of the EG2 security area (chip_eg2_sec_tests.hjson; software in
// sw/device/tests/eg2_sec, each test's header names its bench part). The knobs are the
// software's EG2 parameters (kEg2Param_<name>), drawn here; a secure-boot run keeps the signed
// image's defaults. `model_gaps` stays 0 on the chip: no sequence writes it.

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_lc_rram_rollback (REQ_SEC_05): no path rewrites the life-cycle partition. The bench
// keeps the partition's words (LC_STATE, LC_TRANSITION_CNT) as the RRAM holds them before the
// software runs and compares them after (backdoor).
// Not done here (reported): the rram_macro's JTAG path (REQ_RRAM_20), which needs a JTAG driver
// of the macro's TAP that this bench does not have.
// +eg2_inject_fail=1 changes one stored word before the comparison: the run must fail with
// `EG2CHK FAIL life-cycle partition unchanged (backdoor)`.
class chip_sw_eg2_lc_rram_rollback_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_lc_rram_rollback_vseq)
  `uvm_object_new

  rand int unsigned paths, value;
  constraint knobs_c {
    paths dist {15 := 50, [1:14] :/ 50};
    value inside {[0:2]};
  }

  localparam int unsigned LcWords =
      (otp_ctrl_reg_pkg::LcStateSize + otp_ctrl_reg_pkg::LcTransitionCntSize) / 4;
  bit [31:0] lc_words[LcWords];

  virtual function void write_params();
    set_param("paths", paths);
    set_param("value", value);
  endfunction

  protected function bit [31:0] lc_word(int i);
    return cfg.mem_bkdr_util_h[Otp].read32(otp_ctrl_reg_pkg::LcTransitionCntOffset + 4 * i);
  endfunction

  virtual task body();
    super.body();
    // LC_TRANSITION_CNT and LC_STATE are adjacent (otp_ctrl_mmap.hjson LIFE_CYCLE partition).
    `DV_CHECK_EQ_FATAL(otp_ctrl_reg_pkg::LcTransitionCntOffset +
                       otp_ctrl_reg_pkg::LcTransitionCntSize, otp_ctrl_reg_pkg::LcStateOffset)
    foreach (lc_words[i]) lc_words[i] = lc_word(i);
  endtask

  virtual task post_start();
    int diffs;
    super.post_start();
    if (inject_fail) begin
      announce(`gfn, "life-cycle partition unchanged (backdoor)", "expected word changed");
      lc_words[0] = ~lc_words[0];
    end
    foreach (lc_words[i]) diffs += (lc_word(i) != lc_words[i]);
    void'(check("life-cycle partition unchanged (backdoor)", diffs == 0,
                $sformatf("%0d of %0d words differ", diffs, LcWords)));
  endtask
endclass : chip_sw_eg2_lc_rram_rollback_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_lc_invalid_encodings (REQ_SEC_06): every encoding of the LIFE_CYCLE state that is
// not a defined state decodes to INVALID with every function disabled (device life cycle
// "INVALID"; lc_ctrl.hjson LC_STATE). Before the first boot the bench writes an encoding of the
// drawn class into the partition (backdoor):
//   0 all ones: RRAM content that reads as all ones (all zeros is RAW, a defined state);
//   1 one 16-bit word of a valid state (the image's) changed at random;
//   2 random.
// It reads LC_STATE over the LC TAP (INVALID in every copy), checks lc_ctrl's outputs (CPU,
// hardware debug, DFT and NVM debug enables all off) and that the CPU never fetches. It then
// restores the image's encoding and resets: the software runs and checks the state it runs in.
// +eg2_inject_fail=1 expects a defined state: `EG2CHK FAIL lc_ctrl LC_STATE after an invalid
// encoding`.
class chip_sw_eg2_lc_invalid_encodings_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_lc_invalid_encodings_vseq)
  `uvm_object_new

  rand int unsigned enc_class;
  rand int unsigned word_idx;
  rand bit [15:0] word_xor;
  rand bit [otp_ctrl_reg_pkg::LcStateSize*8-1:0] random_enc;
  constraint knobs_c {
    enc_class inside {[0:2]};
    word_idx < otp_ctrl_reg_pkg::LcStateSize / 2;
    word_xor != 0;
  }

  lc_ctrl_state_pkg::lc_state_e orig_state;
  bit [otp_ctrl_reg_pkg::LcStateSize*8-1:0] enc;

  virtual task pre_start();
    tap = TapLc;
    void'($value$plusargs("eg2_lc_enc_class=%d", enc_class));
    super.pre_start();
  endtask

  protected function bit is_defined(bit [otp_ctrl_reg_pkg::LcStateSize*8-1:0] e);
    lc_ctrl_state_pkg::lc_state_e s;
    s = lc_ctrl_state_pkg::lc_state_e'(e);
    return s inside {lc_ctrl_state_pkg::LcStRaw, lc_ctrl_state_pkg::LcStTestUnlocked0,
                     lc_ctrl_state_pkg::LcStTestLocked0, lc_ctrl_state_pkg::LcStTestUnlocked1,
                     lc_ctrl_state_pkg::LcStTestLocked1, lc_ctrl_state_pkg::LcStTestUnlocked2,
                     lc_ctrl_state_pkg::LcStTestLocked2, lc_ctrl_state_pkg::LcStTestUnlocked3,
                     lc_ctrl_state_pkg::LcStTestLocked3, lc_ctrl_state_pkg::LcStTestUnlocked4,
                     lc_ctrl_state_pkg::LcStTestLocked4, lc_ctrl_state_pkg::LcStTestUnlocked5,
                     lc_ctrl_state_pkg::LcStTestLocked5, lc_ctrl_state_pkg::LcStTestUnlocked6,
                     lc_ctrl_state_pkg::LcStTestLocked6, lc_ctrl_state_pkg::LcStTestUnlocked7,
                     lc_ctrl_state_pkg::LcStDev, lc_ctrl_state_pkg::LcStProd,
                     lc_ctrl_state_pkg::LcStProdEnd, lc_ctrl_state_pkg::LcStRma,
                     lc_ctrl_state_pkg::LcStScrap};
  endfunction

  protected function void write_lc_state_raw(bit [otp_ctrl_reg_pkg::LcStateSize*8-1:0] e);
    for (int i = 0; i < otp_ctrl_reg_pkg::LcStateSize; i += 4) begin
      cfg.mem_bkdr_util_h[Otp].write32(otp_ctrl_reg_pkg::LcStateOffset + i, e[i*8 +: 32]);
    end
  endfunction

  // Called by dut_init after the OTP image is loaded, before the chip leaves reset.
  virtual function void initialize_otp_lc_state();
    super.initialize_otp_lc_state();
    orig_state = otp_read_lc_partition_state(cfg.mem_bkdr_util_h[Otp]);
    case (enc_class)
      0: enc = '1;
      1: begin
        enc = orig_state;
        enc[16 * word_idx +: 16] ^= word_xor;
      end
      default: enc = random_enc;
    endcase
    // A change that lands on another defined state is not an invalid encoding: change more.
    while (is_defined(enc)) enc[15:0] += 1;
    `uvm_info(`gfn, $sformatf("EG2 LC encoding class %0d: 0x%0h (image's state %s)", enc_class,
                              enc, orig_state.name()), UVM_LOW)
    announce(`gfn, "lc_ctrl", $sformatf("invalid LIFE_CYCLE encoding, class %0d", enc_class));
    write_lc_state_raw(enc);
  endfunction

  virtual task body();
    bit [TL_DW-1:0] lc_state_csr;
    bit [TL_DW-1:0] exp_csr = {DecLcStateNumRep{DecLcStInvalid}};
    super.body();
    wait_lc_initialized(.allow_err(1));
    jtag_riscv_agent_pkg::jtag_read_csr(ral.lc_ctrl_regs.lc_state.get_offset(),
                                        p_sequencer.jtag_sequencer_h, lc_state_csr);
    if (inject_fail) announce(`gfn, "lc_ctrl LC_STATE", "a defined state expected");
    void'(check("lc_ctrl LC_STATE after an invalid encoding",
                (lc_state_csr == exp_csr) ^ inject_fail,
                $sformatf("exp 0x%08h got 0x%08h", exp_csr, lc_state_csr)));
    void'(check("CPU enable off in INVALID",
                cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalCpuEn) != lc_ctrl_pkg::On));
    void'(check("hardware debug off in INVALID",
                cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalHwDebugEn) != lc_ctrl_pkg::On));
    void'(check("DFT off in INVALID",
                cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalDftEn) != lc_ctrl_pkg::On));
    void'(check("NVM debug off in INVALID",
                cfg.chip_vif.get_lc_ctrl_enable_signal(LcCtrlSignalNvmDebugEn) !=
                lc_ctrl_pkg::On));
    #50us;
    void'(check("CPU fetch never enabled in INVALID", !cfg.chip_vif.pwrmgr_cpu_fetch_en));
    void'(check("no software ran in INVALID",
                !(cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusInTest,
                                                                 SwTestStatusPassed,
                                                                 SwTestStatusFailed}),
                cfg.sw_test_status_vif.sw_test_status.name()));
    // Restore the image's state; the software then runs and checks it.
    otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], orig_state);
    apply_reset();
  endtask
endclass : chip_sw_eg2_lc_invalid_encodings_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_sec_rma_key_change (REQ_SEC_08): keys derived before RMA cannot be derived after
// it. The run starts in PROD with SECRET2 provisioned by the bench (its RMA token, random creator
// root key). Phase 0: the software derives K1 and K2 and leaves them in `kEg2KeysOut` (marker in
// word 16). The bench reads them, moves the chip to RMA over the LC TAP with the token, resets,
// loads the image again (the RMA erase wipes the RRAM), writes the keys into `kEg2PrevKeys` and
// phase 1, and lets the software derive again in RMA; the software and the bench (the new keys
// from `kEg2KeysOut`) require every key changed.
// +eg2_inject_fail=1 makes the bench expect K1 unchanged: `EG2CHK FAIL bench: K1 differs across
// RMA` (the software's inject build fails with `EG2CHK FAIL K1 differs across RMA`).
class chip_sw_eg2_sec_rma_key_change_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_rma_key_change_vseq)
  `uvm_object_new

  localparam bit [31:0] OutReady = 32'h4b45_5953;  // "SYEK"

  rand bit [31:0] input_seed;
  rand bit [7:0] lc_rma_token[TokenWidthByte];
  rand bit [otp_ctrl_reg_pkg::CreatorRootKeyShare0Size*8-1:0] root_share0, root_share1;
  constraint input_seed_c {
    input_seed != 0;
  }

  bit [31:0] keys0[16];

  virtual task pre_start();
    tap = TapLc;
    super.pre_start();
  endtask

  virtual function void write_params();
    set_param("phase", 0);
    set_param("input_seed", input_seed);
  endfunction

  virtual function void initialize_otp_lc_state();
    super.initialize_otp_lc_state();
    otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], LcStProd);
    otp_write_secret2_partition(.mem_bkdr_util_h(cfg.mem_bkdr_util_h[Otp]),
                                .rma_token(dec_otp_token_from_lc_csrs(lc_rma_token)),
                                .creator_root_key_share0(root_share0),
                                .creator_root_key_share1(root_share1));
  endfunction

  virtual task body();
    bit found;
    bit [7:0] prev_bytes[64];
    bit [31:0] keys1[16];
    int same1, same2;
    super.body();
    wait_sym("kEg2KeysOut", 16, OutReady, found, cfg.sw_test_timeout_ns);
    if (!found) begin
      `uvm_info(`gfn, "the software ended before its keys were ready", UVM_LOW)
      return;
    end
    foreach (keys0[i]) keys0[i] = sym_rd32("kEg2KeysOut", i);
    jtag_lc_state_transition(DecLcStProd, DecLcStRma, {<<8{lc_rma_token}});
    apply_reset();
    // The phase-0 marker stays in SRAM until the boot renews it: wait for the new boot.
    cfg.sw_test_status_vif.sw_test_status = SwTestStatusUnderReset;
    cfg.mem_bkdr_util_h[RramData].load_mem_from_file(
        {cfg.sw_images[SwTypeTestSlotA], ".128.scr.vmem"});
    // The RMA wiped the RRAM: the model forgets what it kept there and takes the image back
    // (review R19, 2026-10-26).
    eg2_iss_wrapper::rram_image_reloaded();
    set_param("phase", 1);
    set_param("input_seed", input_seed);
    foreach (prev_bytes[i]) prev_bytes[i] = keys0[i / 4][8 * (i % 4) +: 8];
    sw_symbol_backdoor_overwrite("kEg2PrevKeys", prev_bytes);
    wait_sym("kEg2KeysOut", 16, OutReady, found, cfg.sw_test_timeout_ns);
    if (!found) begin
      `uvm_info(`gfn, "the software ended before its phase-1 keys were ready", UVM_LOW)
      return;
    end
    foreach (keys1[i]) keys1[i] = sym_rd32("kEg2KeysOut", i);
    for (int i = 0; i < 8; i++) begin
      same1 += keys1[i] == keys0[i];
      same2 += keys1[8 + i] == keys0[8 + i];
    end
    if (inject_fail) announce(`gfn, "bench: K1 differs across RMA", "K1 expected unchanged");
    void'(check("bench: K1 differs across RMA", (same1 < 8) ^ inject_fail,
                $sformatf("%0d of 8 words equal", same1)));
    void'(check("bench: K2 differs across RMA", same2 < 8,
                $sformatf("%0d of 8 words equal", same2)));
  endtask
endclass : chip_sw_eg2_sec_rma_key_change_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_sec_device_id (REQ_SEC_12): the device identifier read over the LC TAP equals the
// software's reading (`kEg2DeviceIdSw`, marker in word 8). The OTP image is the test's own
// (eg2_sec BUILD: a provisioned DEVICE_ID with its CRC-32).
// +eg2_inject_fail=1 expects word 0 to differ: `EG2CHK FAIL JTAG DEVICE_ID_0 equals the
// software's`.
class chip_sw_eg2_sec_device_id_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_device_id_vseq)
  `uvm_object_new

  localparam bit [31:0] SwReady = 32'h4944_5344;  // "DSDI"

  rand bit order;

  virtual task pre_start();
    tap = TapLc;
    super.pre_start();
  endtask

  virtual function void write_params();
    set_param("order", order);
  endfunction

  virtual task post_start();
    bit [TL_DW-1:0] jtag_word;
    super.post_start();
    if (!check("software's DEVICE_ID reading in place", sym_rd32("kEg2DeviceIdSw", 8) == SwReady))
      return;
    for (int i = 0; i < 8; i++) begin
      jtag_riscv_agent_pkg::jtag_read_csr(ral.lc_ctrl_regs.device_id[i].get_offset(),
                                          p_sequencer.jtag_sequencer_h, jtag_word);
      if (i == 0 && inject_fail) announce(`gfn, "JTAG DEVICE_ID_0", "word 0 expected to differ");
      void'(check($sformatf("JTAG DEVICE_ID_%0d equals the software's", i),
                  (jtag_word == sym_rd32("kEg2DeviceIdSw", i)) ^ (i == 0 && inject_fail),
                  $sformatf("jtag 0x%08h sw 0x%08h", jtag_word, sym_rd32("kEg2DeviceIdSw", i))));
    end
  endtask
endclass : chip_sw_eg2_sec_device_id_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_keymgr_dpe_diversification (REQ_SEC_09): one key-derivation input changed per run.
// For `input` 1 (device identifier) and 2 (life-cycle state) the bench makes the change while the
// software's reset is in progress: a new HW_CFG0.DEVICE_ID (`bits` bits flipped, the digest
// recomputed), or the life-cycle state moved from RMA to DEV. The software checks the keys.
class chip_sw_eg2_keymgr_dpe_diversification_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_keymgr_dpe_diversification_vseq)
  `uvm_object_new

  rand int unsigned input_sel, bits;
  constraint knobs_c {
    input_sel inside {[0:4]};
    bits dist {1 := 50, [2:32] :/ 50};
  }

  virtual function void write_params();
    set_param("input", input_sel);
    set_param("bits", bits);
  endfunction

  virtual task body();
    bit [31:0] sel = input_sel;
    super.body();
    void'($value$plusargs("eg2_param_input=%d", sel));
    if (!(sel inside {1, 2})) return;
    // The software's reset (rstmgr RESET_REQ) after its first derivation.
    wait (cfg.sw_test_status_vif.sw_test_status == SwTestStatusInTest);
    wait (cfg.chip_vif.cpu_rst_n === 1'b0);
    if (sel == 1) change_device_id();
    else change_lc_state();
  endtask

  protected function void change_device_id();
    bit [otp_ctrl_reg_pkg::DeviceIdSize*8-1:0] id;
    bit [otp_ctrl_reg_pkg::ManufStateSize*8-1:0] manuf;
    bit [31:0] n = bits;
    void'($value$plusargs("eg2_param_bits=%d", n));
    for (int i = 0; i < otp_ctrl_reg_pkg::DeviceIdSize; i += 4) begin
      id[i*8 +: 32] = cfg.mem_bkdr_util_h[Otp].read32(otp_ctrl_reg_pkg::DeviceIdOffset + i);
    end
    for (int i = 0; i < otp_ctrl_reg_pkg::ManufStateSize; i += 4) begin
      manuf[i*8 +: 32] = cfg.mem_bkdr_util_h[Otp].read32(otp_ctrl_reg_pkg::ManufStateOffset + i);
    end
    for (int i = 0; i < n; i++) id[(i * 37) % (otp_ctrl_reg_pkg::DeviceIdSize * 8)] ^= 1'b1;
    announce(`gfn, "keymgr_dpe", $sformatf("DEVICE_ID changed in %0d bits during the reset", n));
    otp_write_hw_cfg0_partition(cfg.mem_bkdr_util_h[Otp], id, manuf);
  endfunction

  protected function void change_lc_state();
    announce(`gfn, "keymgr_dpe", "life-cycle state RMA -> DEV during the reset");
    otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], LcStDev);
  endfunction
endclass : chip_sw_eg2_keymgr_dpe_diversification_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_sec_partial_reset (REQ_SEC_22): the core's lockstep shadow is held in reset for
// `hold` cycles while the software's busy loop runs (`kEg2PartialResetBusy`). The shadow then
// restarts behind the main core: rv_core_ibex must raise its major internal alert (ibex
// "Lockstep": a divergence is detected, never silent). If it does, the CPU's fetch may be off and
// the software cannot finish, so the bench ends the run with its verdict; if it does not, the
// software checks that no error status was left without the alert.
// +eg2_inject_fail=1 expects no alert: `EG2CHK FAIL lockstep divergence raises
// alert_major_internal`.
class chip_sw_eg2_sec_partial_reset_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_partial_reset_vseq)
  `uvm_object_new

  localparam string Lockstep = {"tb.dut.top_earlgrey.earlgrey_pd_main.u_rv_core_ibex.u_core",
                                ".gen_lockstep.u_ibex_lockstep"};

  rand int unsigned busy_loops, hold;
  constraint knobs_c {
    busy_loops inside {[2000:50000]};
    hold dist {[1:4] :/ 50, [5:200] :/ 50};
  }

  virtual function void write_params();
    set_param("busy_loops", busy_loops);
  endfunction

  virtual task body();
    bit found, raised, alert;
    super.body();
    wait_sym("kEg2PartialResetBusy", 0, 1, found, cfg.sw_test_timeout_ns, 200);
    if (!found) return;
    announce(`gfn, "lockstep", $sformatf("shadow core reset held %0d cycles", hold));
    if (!uvm_hdl_force({Lockstep, ".rst_shadow_n"}, 1'b0)) begin
      `uvm_fatal(`gfn, "cannot force the lockstep's rst_shadow_n")
    end
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(hold);
    if (!uvm_hdl_release({Lockstep, ".rst_shadow_n"})) begin
      `uvm_fatal(`gfn, "cannot release the lockstep's rst_shadow_n")
    end
    for (int i = 0; i < 2000 && !raised; i++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
      if (uvm_hdl_read({Lockstep, ".alert_major_internal_o"}, alert) && alert) raised = 1'b1;
    end
    `uvm_info(`gfn, $sformatf("EG2 lockstep alert_major_internal raised: %0d", raised), UVM_LOW)
    if (inject_fail) announce(`gfn, "lockstep divergence", "no alert expected");
    if (raised || inject_fail) begin
      void'(check("lockstep divergence raises alert_major_internal", raised ^ inject_fail));
      // The software may not run again: the bench's verdict ends the run.
      fork : isolation_fork
        begin
          fork
            wait (cfg.sw_test_status_vif.sw_test_done);
            #(2ms);
          join_any
          disable fork;
        end
      join
      if (!cfg.sw_test_status_vif.sw_test_done) finish_with_bench_verdict();
    end
  endtask
endclass : chip_sw_eg2_sec_partial_reset_vseq

// -------------------------------------------------------------------------------------------------
// chip_sw_eg2_sec_wipe_before_switch (REQ_SEC_11): when the owner image enters its test (after
// the CHERIoT switch in the `_cheriot` build), the bench reads the root secrets keymgr_dpe holds
// (the creator root key shares, the creator and owner seeds: its inputs) and searches main and
// retention SRAM, descrambled, for any 64-bit aligned run of them. A secret that is all zero
// cannot be searched for: the bench says so.
// Not searched (reported): the sec SRAM, which has no backdoor in this bench.
// +eg2_inject_fail=1 plants a copy of the root key's first 64 bits in main SRAM first:
// `EG2CHK FAIL secret found in SRAM`.
class chip_sw_eg2_sec_wipe_before_switch_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_wipe_before_switch_vseq)
  `uvm_object_new

  localparam string Kmgr = "tb.dut.top_earlgrey.earlgrey_pd_main.u_keymgr_dpe";

  bit [63:0] secrets[$];
  string secret_names[$];

  protected function void add_secret(string name, bit [255:0] value);
    if (value == '0) begin
      `uvm_info(`gfn, $sformatf("EG2 %s is all zero in this run: not searched", name), UVM_LOW)
      return;
    end
    for (int i = 0; i < 4; i++) begin
      secrets.push_back(value[64 * i +: 64]);
      secret_names.push_back($sformatf("%s[%0d]", name, i));
    end
  endfunction

  protected function int scan(bit is_main, int unsigned size_bytes, output string where);
    bit [31:0] lo, hi;
    lo = sram_rd32(is_main, 0);
    for (int unsigned off = 4; off < size_bytes; off += 4) begin
      hi = sram_rd32(is_main, off);
      foreach (secrets[s]) begin
        if ({hi, lo} == secrets[s]) begin
          where = $sformatf("%s at %s SRAM offset 0x%0h", secret_names[s],
                            is_main ? "main" : "retention", off - 4);
          return 1;
        end
      end
      lo = hi;
    end
    return 0;
  endfunction

  virtual task body();
    bit [513:0] root;
    bit [256:0] seed;
    string where;
    int found;
    super.body();
    wait (cfg.sw_test_status_vif.sw_test_status inside {SwTestStatusInTest, SwTestStatusPassed,
                                                        SwTestStatusFailed});
    if (cfg.sw_test_status_vif.sw_test_status != SwTestStatusInTest) return;
    if (!uvm_hdl_read({Kmgr, ".creator_root_key_i"}, root)) `uvm_fatal(`gfn, "creator_root_key_i")
    add_secret("creator root key share 0", root[513:258]);
    add_secret("creator root key share 1", root[256:1]);
    if (!uvm_hdl_read({Kmgr, ".creator_seed_i"}, seed)) `uvm_fatal(`gfn, "creator_seed_i")
    add_secret("creator seed", seed[256:1]);
    if (!uvm_hdl_read({Kmgr, ".owner_seed_i"}, seed)) `uvm_fatal(`gfn, "owner_seed_i")
    add_secret("owner seed", seed[256:1]);
    void'(check("at least one secret to search for", secrets.size() > 0));
    if (inject_fail && secrets.size() > 0) begin
      announce(`gfn, "secret found in SRAM", "root key copy planted in main SRAM");
      // Half way up main SRAM: below the stack, above this image's data.
      sram_wr32(1, 'h10000, secrets[0][31:0]);
      sram_wr32(1, 'h10004, secrets[0][63:32]);
    end
    found = scan(1, top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES, where);
    if (!found) found = scan(0, top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_RET_RAM_SIZE_BYTES, where);
    void'(check("secret found in SRAM", !found, found ? where : "none in main and retention SRAM"));
  endtask
endclass : chip_sw_eg2_sec_wipe_before_switch_vseq

// -------------------------------------------------------------------------------------------------
// The knobs of the security tests with no bench action of their own.

// chip_sw_eg2_keymgr_dpe_locks (REQ_SEC_10).
class chip_sw_eg2_keymgr_dpe_locks_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_keymgr_dpe_locks_vseq)
  `uvm_object_new

  rand int unsigned regs, versions, wclass;
  rand bit [31:0] max_ver;
  constraint knobs_c {
    regs dist {7 := 50, [1:6] :/ 50};
    versions dist {7 := 50, [1:6] :/ 50};
    wclass inside {[0:2]};
    max_ver dist {1 := 10, [2:'hff] :/ 60, ['h100:'hfffffffe] :/ 30};
  }

  virtual function void write_params();
    set_param("regs", regs);
    set_param("versions", versions);
    set_param("wclass", wclass);
    set_param("max_ver", max_ver);
  endfunction
endclass : chip_sw_eg2_keymgr_dpe_locks_vseq

// chip_sw_eg2_sec_reset_residue (REQ_SEC_25).
class chip_sw_eg2_sec_reset_residue_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_reset_residue_vseq)
  `uvm_object_new

  rand int unsigned locs, reset_type;
  constraint knobs_c {
    locs dist {3 := 50, [1:2] :/ 50};
    reset_type inside {[0:2]};
  }

  virtual function void write_params();
    set_param("locs", locs);
    set_param("reset", reset_type);
  endfunction
endclass : chip_sw_eg2_sec_reset_residue_vseq

// chip_sw_eg2_sec_secret_partitions (REQ_SEC_07; Bazel eg2_sec_lc_collateral): the life-cycle
// state is the OTP image the dvsim entry loads.
class chip_sw_eg2_sec_secret_partitions_vseq extends chip_sw_eg2_boot_sec_base_vseq;
  `uvm_object_utils(chip_sw_eg2_sec_secret_partitions_vseq)
  `uvm_object_new

  rand int unsigned paths, part;
  constraint knobs_c {
    paths dist {15 := 50, [1:14] :/ 50};
    part dist {7 := 50, [1:6] :/ 50};
  }

  virtual function void write_params();
    set_param("paths", paths);
    set_param("part", part);
  endfunction
endclass : chip_sw_eg2_sec_secret_partitions_vseq
