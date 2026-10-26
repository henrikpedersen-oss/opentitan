// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// chip_sw_eg2_pbf_sym_crypto_sb and _fault (eg2_top_verification spec REQ_PBF_10; software
// sw/device/tests/eg2_pbf/eg2_pbf_sym_crypto_{sb,fault}.c). Behind ROM_EXT the signed image keeps
// its defaults (chip_sw_eg2_base_vseq) and the bench follows them.

// AES, HMAC/SHA-2 and KMAC/SHA-3 with software and sideloaded keys.
class chip_sw_eg2_pbf_sym_crypto_sb_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_sym_crypto_sb_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [2:0] engines;
  rand bit [2:0] aes_mode;     // 0 ECB, 1 CBC, 2 CFB-128, 3 OFB, 4 CTR, 5 drawn by software
  rand bit [1:0] key_len;      // 0 128, 1 192, 2 256, 3 drawn
  rand bit [2:0] msg_class;    // 0, 1, block - 1, block, long; 5 drawn
  rand bit [6:0] manual_pct;
  rand bit       sideload;

  constraint knobs_c {
    engines != 0;
    engines dist {3'b111 :/ 60, [3'b001 : 3'b110] :/ 40};
    aes_mode inside {[0 : 5]};
    // Modes and key lengths by class: each named one, or the software's draw.
    aes_mode dist {[0 : 4] :/ 70, 5 :/ 30};
    key_len dist {[0 : 2] :/ 70, 3 :/ 30};
    msg_class inside {[0 : 5]};
    msg_class dist {[0 : 4] :/ 80, 5 :/ 20};
    manual_pct inside {[0 : 100]};
    sideload dist {1 :/ 80, 0 :/ 20};
  }

  virtual function void write_params();
    set_param("engines", engines);
    set_param("aes_mode", aes_mode);
    set_param("key_len", key_len);
    set_param("msg_class", msg_class);
    set_param("manual_pct", manual_pct);
    set_param("sideload", sideload);
  endfunction

  virtual task body();
    bit [31:0] en = pinned("engines", engines), sl = pinned("sideload", sideload);
    if (cfg.rom_ext_assembled) begin
      en = 7;
      sl = 1;
    end
    // REQ_PBF_05: the engines worked (blocks out of AES, HMAC digests, KMAC squeezes) and, with
    // sideload, keymgr_dpe drove a key to AES.
    if (en[0]) add_activity("aes output valid", {PD, ".u_aes.u_reg.status_output_valid_qs"}, 3);
    if (en[1]) add_activity("hmac done", {PD, ".u_hmac.u_reg.intr_state_hmac_done_qs"}, 2);
    if (en[2]) add_activity("kmac squeeze", {PD, ".u_kmac.u_reg.status_sha3_squeeze_qs"}, 2);
    if (sl) add_activity("keymgr_dpe aes sideload key", {PD, ".keymgr_dpe_aes_key"}, 1);
    super.body();
  endtask
endclass

// An AES or KMAC operation faulted by an escalation or a reset.
class chip_sw_eg2_pbf_sym_crypto_fault_vseq extends chip_sw_eg2_pbf_base_vseq;
  `uvm_object_utils(chip_sw_eg2_pbf_sym_crypto_fault_vseq)
  `uvm_object_new

  localparam string PD = "tb.dut.top_earlgrey.earlgrey_pd_main";

  rand bit [1:0] engine;       // 0 AES, 1 KMAC
  rand bit [1:0] fault;        // 0 escalation, 1 software reset
  rand bit [1:0] fault_point;

  constraint knobs_c {
    engine inside {[0 : 1]};
    fault inside {[0 : 1]};
    fault_point inside {[0 : 2]};
  }

  virtual function void write_params();
    set_param("engine", engine);
    set_param("fault", fault);
    set_param("fault_point", fault_point);
  endfunction

  // On the life-cycle escalation (lc_tx_t On = 4'b0101): every crypto block in its terminal
  // state with its fatal alert, and no AES output (aes interfaces.md AES.MAIN.FSM.GLOBAL_ESC,
  // AES.DATA_REG.LOCAL_ESC; kmac theory of operation "Reaction to Life Cycle Escalation
  // Requests"), checked before the escalation reset.
  protected task check_escalation();
    uvm_hdl_data_t v;
    forever begin
      void'(uvm_hdl_read({PD, ".lc_ctrl_lc_escalate_en"}, v));
      if (v[3:0] == 4'b0101) break;
      #100ns;
    end
    void'(announce_fault("eg2_pbf_sym_crypto_fault escalation", "aes,kmac",
                         "life-cycle escalation"));
    #2us;
    void'(uvm_hdl_read({PD, ".u_aes.u_reg.status_alert_fatal_fault_qs"}, v));
    if (v[0] !== 1'b1) `uvm_error(`gfn, "EG2_PBF aes: no fatal alert status under escalation")
    void'(uvm_hdl_read({PD, ".u_aes.u_reg.status_output_valid_qs"}, v));
    if (v[0] !== 1'b0) `uvm_error(`gfn, "EG2_PBF aes: output valid under escalation")
    void'(uvm_hdl_read({PD, ".u_kmac.u_reg.status_alert_fatal_fault_qs"}, v));
    if (v[0] !== 1'b1) `uvm_error(`gfn, "EG2_PBF kmac: no fatal alert status under escalation")
    `uvm_info(`gfn, "EG2_PBF escalation: aes and kmac in their terminal state", UVM_LOW)
  endtask

  virtual task body();
    super.body();
    fork
      check_escalation();
    join_none
  endtask
endclass
