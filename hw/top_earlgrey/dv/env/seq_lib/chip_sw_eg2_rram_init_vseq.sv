// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_rram_init (sw/device/tests/eg2_rram/eg2_rram_init.c; EG2 spec REQ_RRAM_21). The bench's
// half:
//   - the life-cycle state drawn before the boot (RMA, DEV: the seeds are read; TEST_UNLOCKED0:
//     they are not, lc_seed_hw_rd_en Off);
//   - every `EG2 RRAM init reg off=.. val=..` line: no 32-bit slice of the scrambling keys or the
//     seeds (taken at rram_ctrl's key and seed ports) is in a software-readable register;
//   - `EG2 RRAM init seeds`: each seed's valid at the keymgr port is set exactly when
//     lc_seed_hw_rd_en is On (theory_of_operation.md "Initialization"), and the scrambler holds the
//     sideloaded keys;
//   - `EG2 RRAM init disable`: once DIS.SW_DIS took effect, the scrambler holds the random keys,
//     not the sideloaded ones ("RRAM Scrambling"), and no seed is valid; then the second boot.
class chip_sw_eg2_rram_init_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rram_init_vseq)

  `uvm_object_new

  localparam string RramCtrl = "tb.dut.top_earlgrey.earlgrey_pd_main.u_rram_ctrl";
  localparam string Scr = {RramCtrl, ".u_rram_phy.u_rram_scramble"};

  typedef enum int {LcRma, LcDev, LcTestUnlocked0} lc_class_e;
  rand lc_class_e lc_class;
  rand bit        disable_rram;

  constraint knobs_c {
    lc_class dist {LcRma := 1, LcDev := 1, LcTestUnlocked0 := 1};
    disable_rram dist {1'b1 := 50, 1'b0 := 50};
  }

  virtual function void write_params();
    set_param("disable", disable_rram);
  endfunction

  virtual function void initialize_otp_lc_state();
    super.initialize_otp_lc_state();
    case (lc_class)
      LcDev: otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], LcStDev);
      LcTestUnlocked0: otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], LcStTestUnlocked0);
      default: otp_write_lc_partition_state(cfg.mem_bkdr_util_h[Otp], LcStRma);
    endcase
    `uvm_info(`gfn, $sformatf("EG2 RRAM init: life-cycle state %s", lc_class.name()), UVM_LOW)
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
    return v;
  endfunction

  // The secrets: both keys (128 bits each) and both seeds (256 bits each), as 32-bit slices.
  protected bit [31:0] secrets[$];

  protected function void collect_secrets();
    uvm_hdl_data_t k;
    secrets.delete();
    k = rd({RramCtrl, ".addr_key"});
    for (int i = 0; i < 4; i++) secrets.push_back(k[32 * i +: 32]);
    k = rd({RramCtrl, ".data_key"});
    for (int i = 0; i < 4; i++) secrets.push_back(k[32 * i +: 32]);
    k = rd({RramCtrl, ".seeds"});
    for (int i = 0; i < 16; i++) secrets.push_back(k[32 * i +: 32]);
  endfunction

  protected function void check_seed_valid(bit exp, string when);
    uvm_hdl_data_t v = rd({RramCtrl, ".seeds_valid"});
    `DV_CHECK_EQ(v[1:0], {exp, exp}, $sformatf("seed_valid %s", when))
  endfunction

  virtual task body();
    string line;
    bit [31:0] dis = disable_rram;
    bit seeds_read;
    super.body();
    void'($value$plusargs("eg2_param_disable=%d", dis));
    forever begin
      bit [31:0] off, val;
      wait_sw_line("EG2 RRAM init ", line, 200_000_000, 1);
      if (line == "") return;
      if ($sscanf(line, "EG2 RRAM init reg off=0x%h val=0x%h", off, val) == 2) begin
        if (secrets.size() == 0) collect_secrets();
        foreach (secrets[i]) begin
          if (secrets[i] != '0 && secrets[i] != '1 && val == secrets[i]) begin
            `uvm_error(`gfn, $sformatf("rram_ctrl register 0x%0h reads 0x%0h: part of a secret",
                                       off, val))
          end
        end
      end else if (line == "EG2 RRAM init seeds") begin
        lc_ctrl_pkg::lc_tx_t en = lc_ctrl_pkg::lc_tx_t'(rd({RramCtrl, ".lc_seed_hw_rd_en_i"}));
        seeds_read = (en == lc_ctrl_pkg::On);
        `DV_CHECK_EQ(seeds_read, lc_class != LcTestUnlocked0,
                     $sformatf("lc_seed_hw_rd_en in %s", lc_class.name()))
        check_seed_valid(seeds_read, "after INIT");
        if (!((rd({Scr, ".addr_key_q"})) == (rd({RramCtrl, ".addr_key"})))) `uvm_error(`gfn, $sformatf("Check failed rd({Scr, \".addr_key_q\"}) == rd({RramCtrl, \".addr_key\"}) (0x%0h vs 0x%0h) %s", rd({Scr, ".addr_key_q"}), rd({RramCtrl, ".addr_key"}), "scrambler address key is not the sideloaded key"))
        if (!((rd({Scr, ".data_key_q"})) == (rd({RramCtrl, ".data_key"})))) `uvm_error(`gfn, $sformatf("Check failed rd({Scr, \".data_key_q\"}) == rd({RramCtrl, \".data_key\"}) (0x%0h vs 0x%0h) %s", rd({Scr, ".data_key_q"}), rd({RramCtrl, ".data_key"}), "scrambler data key is not the sideloaded key"))
        if (!dis) return;
      end else if (line == "EG2 RRAM init disable") begin
        cfg.chip_vif.cpu_clk_rst_if.wait_clks(2000);
        if (!((rd({Scr, ".addr_key_q"})) == (rd({RramCtrl, ".rand_addr_key"})))) `uvm_error(`gfn, $sformatf("Check failed rd({Scr, \".addr_key_q\"}) == rd({RramCtrl, \".rand_addr_key\"}) (0x%0h vs 0x%0h) %s", rd({Scr, ".addr_key_q"}), rd({RramCtrl, ".rand_addr_key"}), "scrambler address key after disable is not the random key"))
        if (!((rd({Scr, ".data_key_q"})) == (rd({RramCtrl, ".rand_data_key"})))) `uvm_error(`gfn, $sformatf("Check failed rd({Scr, \".data_key_q\"}) == rd({RramCtrl, \".rand_data_key\"}) (0x%0h vs 0x%0h) %s", rd({Scr, ".data_key_q"}), rd({RramCtrl, ".rand_data_key"}), "scrambler data key after disable is not the random key"))
        if (!((rd({Scr, ".addr_key_q"})) != (rd({RramCtrl, ".addr_key"})))) `uvm_error(`gfn, $sformatf("Check failed rd({Scr, \".addr_key_q\"}) != rd({RramCtrl, \".addr_key\"}) (0x%0h vs 0x%0h) %s", rd({Scr, ".addr_key_q"}), rd({RramCtrl, ".addr_key"}), "scrambler still holds the sideloaded address key after disable"))
        check_seed_valid(1'b0, "after disable");
        set_param("boot_phase", 1);
        `uvm_info(`gfn, "EG2 RRAM init: second boot", UVM_LOW)
        apply_reset();
        return;
      end
    end
  endtask

endclass : chip_sw_eg2_rram_init_vseq
