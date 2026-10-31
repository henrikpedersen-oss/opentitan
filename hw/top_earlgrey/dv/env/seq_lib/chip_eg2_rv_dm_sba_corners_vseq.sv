// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_eg2_rv_dm_sba_corners (EG2 spec REQ_CDM_06, REQ_CMS_16; "Debug Controller" / "Corner
// cases"; sw/device/tests/eg2_debug/eg2_rv_dm_sba_corners.c), both modes; the tag checks in
// CHERIoT mode.
//
// System bus access through rv_dm, against the RISC-V Debug Specification 0.13.2 (3.12.18 sbcs:
// sberror "2 bad address, 3 alignment, 4 size, 7 other"; sbbusyerror "set when the debugger
// attempts to read data while a read is in progress, or when the debugger initiates a new access
// while one is already in progress (while sbbusy is set). It remains set until explicitly cleared
// by the debugger. While this field is set, no more system bus accesses can be initiated by the
// Debug Module"; sbautoincrement "sbaddress is incremented by the access size (in bytes) selected
// in sbaccess after every system bus access"; sbreadondata "every read from sbdata0 automatically
// triggers a system bus read at the (possibly auto-incremented) address"; 3.12.23 sbdata0 "If
// the width of the read access is less than the width of sbdata, the contents of the remaining
// high bits may take on any value") and rv_dm's interfaces.md ("when bus errors (either address
// errors or integrity errors) occur on the SBA TL-UL path, alerts are not triggered. Instead the
// error status is fed into the PULP RISC-V Debug System"):
//
//  | case        | stimulus                                         | expected                    |
//  |-------------|--------------------------------------------------|-----------------------------|
//  | bus error   | read past a register window's last register      | sberror 2 or 7              |
//  | unmapped    | read after a device window no window follows     | sberror 2 or 7              |
//  | integrity   | a read of scratch with its response's data       | sberror 2 or 7; no rv_dm    |
//  |             |   integrity bit flipped (bench fault)            |   alert                     |
//  | busy        | a second access while a write across the always- | sbbusyerror 1; no access    |
//  |             |   on CDC (aon_timer WKUP_THOLD_LO) is in flight; |   while it is set; accesses |
//  |             |   then a write while sbbusyerror is set          |   work after its clear      |
//  | autoinc     | n words written with sbautoincrement, read back  | the words; sbaddress0       |
//  |             |   with sbreadondata and sbautoincrement          |   advanced by 4 per access  |
//  | sizes       | 8- and 16-bit writes at each lane of a scratch   | only the addressed lanes    |
//  |             |   word, read back at 32 and at their own size    |   change; data right-aligned|
//  | race        | the core writes byte 0, SBA byte 2 of one word   | the software's checks       |
//  |             |   (8-bit), concurrently (action RACE)            |   (eg2_rv_dm_sba_corners.c) |
//  | revbm       | the revocation bit of a capability's base set    | read back; the core reads   |
//  |             |   and cleared over SBA (CAP_PREP, CAP_CHECK)     |   the same word; CHERIoT:   |
//  |             |                                                  |   tag 0 while set, 1 after  |
//  | tag         | CHERIoT: the slot holding the capability written | tag 0 (REQ_CMS_16)          |
//  |             |   over SBA with its own value                    |                             |
//  | mem_sys     | cheriot_mem_sys TBRE_BASE_ADDR written over SBA  | RISC-V: read back; CHERIoT: |
//  |             |                                                  |   unchanged (REQ_SOC_08)    |
//
// EXPECTED FAILURES in CHERIoT mode: `tag` until ticket F23 (SBA bypasses cheriot_mem_sys, the tag
// stays set); `mem_sys` until ticket F29 (no boot stage locks cheriot_mem_sys.regs).
class chip_eg2_rv_dm_sba_corners_vseq extends chip_eg2_rv_dm_corners_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_sba_corners_vseq)
  `uvm_object_new

  localparam bit [31:0] AonThold = top_earlgrey_pkg::TOP_EARLGREY_AON_TIMER_BASE_ADDR + 32'hc;
  localparam bit [31:0] HoleAddr = top_earlgrey_pkg::TOP_EARLGREY_UART1_BASE_ADDR + 32'h38;
  localparam bit [31:0] GapAddr = top_earlgrey_pkg::TOP_EARLGREY_GPIO_BASE_ADDR +
                                  top_earlgrey_pkg::TOP_EARLGREY_GPIO_SIZE_BYTES;
  localparam bit [31:0] TbreBase = top_earlgrey_pkg::TOP_EARLGREY_CHERIOT_MEM_SYS_REGS_BASE_ADDR +
                                   32'h14;
  localparam bit [31:0] RevbmBase = top_earlgrey_pkg::TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR;
  localparam bit [31:0] RevbmCovered = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  localparam bit [2:0] SbaBusErr[2] = '{3'd2, 3'd7};

  rand int unsigned n_words;
  rand bit [31:0] words[8];
  rand bit [7:0] n_race;
  rand bit [31:0] tbre_val;
  rand int unsigned intg_bit;

  constraint knobs_c {
    n_words inside {[2:8]};
    n_race inside {[4:40]};
    tbre_val[2:0] == 0;
    tbre_val[31:28] == 4'h1;
    intg_bit < 7;
  }

  bit cheriot;

  protected task expect_bus_err(string what, bit write, bit [31:0] addr);
    bit [2:0] serr;
    bit [31:0] rdata;
    sba_raw(write, addr, 3'd2, 32'h5a5a_a5a5, serr, rdata);
    chk_true($sformatf("%s: sberror %0d is a bus error (2 or 7)", what, serr),
             serr inside {3'd2, 3'd7});
  endtask

  protected task sba_rd(bit [31:0] addr, output bit [31:0] v);
    bit [2:0] serr;
    sba_raw(1'b0, addr, 3'd2, 0, serr, v);
    chk_eq($sformatf("SBA read of 0x%08x: sberror", addr), serr, 0);
  endtask

  protected task sba_wr(bit [31:0] addr, bit [31:0] v, bit [2:0] size = 3'd2);
    bit [2:0] serr;
    bit [31:0] rdata;
    sba_raw(1'b1, addr, size, v, serr, rdata);
    chk_eq($sformatf("SBA write of 0x%08x: sberror", addr), serr, 0);
  endtask

  // Integrity: the next response on rv_dm's SBA port gets data integrity bit `intg_bit` flipped
  // for one cycle (a bench fault).
  protected task case_integrity();
    uvm_hdl_data_t a, v_hdl;
    uvm_reg_data_t v;
    bit [2:0] serr;
    bit alert_seen;
    string path = SbaRspPath;
    string apath = {PdHier, ".u_rv_dm.alert_tx_o"};
    sbcs_wr(3'd2, 1'b0, 1'b1, 1'b0);
    fork
      begin
        tlul_pkg::tl_d2h_t d;
        for (int i = 0; i < 20000; i++) begin
          if (!uvm_hdl_read(path, v_hdl)) `uvm_fatal(`gfn, {"cannot read ", path})
          d = tlul_pkg::tl_d2h_t'(v_hdl[$bits(tlul_pkg::tl_d2h_t)-1:0]);
          if (d.d_valid) begin
            d.d_user.data_intg[intg_bit] ^= 1'b1;
            // TODO(inject-registry): announce this fault (rv_dm SBA response integrity, now).
            `uvm_info(`gfn, $sformatf("EG2 fault: SBA response data_intg bit %0d flipped",
                                      intg_bit), UVM_LOW)
            if (!uvm_hdl_force(path, uvm_hdl_data_t'(d))) `uvm_fatal(`gfn, path)
            cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
            if (!uvm_hdl_release(path)) `uvm_fatal(`gfn, path)
            break;
          end
          cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
        end
      end
      sbaddr_wr(scratch_addr);
    join
    sba_wait(v);
    serr = get_field_val(cfg.jtag_dmi_ral.sbcs.sberror, v);
    chk_true($sformatf("integrity error: sberror %0d is a bus error (2 or 7)", serr),
             serr inside {3'd2, 3'd7});
    for (int i = 0; i < 2000; i++) begin
      prim_alert_pkg::alert_tx_t tx;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
      if (!uvm_hdl_read(apath, a)) `uvm_fatal(`gfn, {"cannot read ", apath})
      tx = prim_alert_pkg::alert_tx_t'(a[$bits(prim_alert_pkg::alert_tx_t)-1:0]);
      alert_seen |= tx.alert_p;
    end
    chk_true("integrity error on SBA: rv_dm raises no alert", !alert_seen);
    sbcs_wr(3'd2);
  endtask

  protected task case_busy();
    uvm_reg_data_t v;
    bit [31:0] before_val, after_val;
    bit seen;
    sba_rd(scratch_addr + 4 * 20, before_val);
    sbcs_wr(3'd2);
    sbaddr_wr(AonThold);
    sbdata_wr(words[0]);
    sbdata_wr(words[1]);  // a new write while the first is in flight
    sbcs_rd(v);
    chk_true("the write across the CDC still ran at the second access (stimulus)",
             get_field_val(cfg.jtag_dmi_ral.sbcs.sbbusy, v) ||
             get_field_val(cfg.jtag_dmi_ral.sbcs.sbbusyerror, v));
    sba_wait(v);
    chk_eq("sbbusyerror after an access started while busy", get_field_val(
           cfg.jtag_dmi_ral.sbcs.sbbusyerror, v), 1);
    // While sbbusyerror is set no access starts.
    sbaddr_wr(scratch_addr + 4 * 20);
    fork
      begin
        for (int i = 0; i < 2000; i++) begin
          cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
          seen |= sba_a_valid();
        end
      end
      sbdata_wr(~before_val);
    join
    chk_true("no SBA request while sbbusyerror is set", !seen);
    sbcs_wr(3'd2);  // clears sbbusyerror and sberror
    sba_rd(scratch_addr + 4 * 20, after_val);
    chk_eq("the word written while sbbusyerror was set", after_val, before_val);
    sba_wr(scratch_addr + 4 * 20, ~before_val);
    sba_rd(scratch_addr + 4 * 20, after_val);
    chk_eq("an access after sbbusyerror's clear", after_val, ~before_val);
  endtask

  protected task case_autoinc();
    uvm_reg_data_t v;
    bit [31:0] a, base = scratch_addr + 4 * 8, d;
    sbcs_wr(3'd2, 1'b1, 1'b0, 1'b0);
    sbaddr_wr(base);
    for (int i = 0; i < n_words; i++) begin
      sbdata_wr(words[i]);
      sba_wait(v);
      chk_eq($sformatf("auto-increment write %0d: sberror", i), get_field_val(
             cfg.jtag_dmi_ral.sbcs.sberror, v), 0);
    end
    sbaddr_rd(a);
    chk_eq("sbaddress0 after the auto-increment writes", a, base + 4 * n_words, 1'b1);
    sbcs_wr(3'd2, 1'b1, 1'b1, 1'b1);
    sbaddr_wr(base);  // read-on-address: reads word 0
    for (int i = 0; i < n_words; i++) begin
      sba_wait(v);
      if (i == n_words - 1) sbcs_wr(3'd2, 1'b1, 1'b0, 1'b0, 1'b0);  // no read after the last
      sbdata_rd(d);
      chk_eq($sformatf("read-on-data word %0d", i), d, words[i]);
    end
    sbaddr_rd(a);
    chk_eq("sbaddress0 after the read-on-data reads", a, base + 4 * n_words);
    sbcs_wr(3'd2);
  endtask

  protected task case_sizes();
    bit [31:0] w = scratch_addr + 4 * 4, base_val = words[2], cur, got, exp;
    bit [2:0] serr;
    sba_wr(w, base_val);
    exp = base_val;
    for (int lane = 0; lane < 4; lane++) begin
      bit [7:0] b = words[3][8 * lane +: 8];
      sba_raw(1'b1, w + lane, 3'd0, {24'h0, b}, serr, cur);
      chk_eq($sformatf("8-bit write lane %0d: sberror", lane), serr, 0);
      exp[8 * lane +: 8] = b;
      sba_rd(w, got);
      chk_eq($sformatf("word after the 8-bit write to lane %0d", lane), got, exp);
      sba_raw(1'b0, w + lane, 3'd0, 0, serr, got);
      chk_eq($sformatf("8-bit read lane %0d", lane), got[7:0], b);
    end
    for (int half = 0; half < 2; half++) begin
      bit [15:0] h = words[4][16 * half +: 16];
      sba_raw(1'b1, w + 2 * half, 3'd1, {16'h0, h}, serr, cur);
      chk_eq($sformatf("16-bit write half %0d: sberror", half), serr, 0);
      exp[16 * half +: 16] = h;
      sba_rd(w, got);
      chk_eq($sformatf("word after the 16-bit write to half %0d", half), got, exp);
      sba_raw(1'b0, w + 2 * half, 3'd1, 0, serr, got);
      chk_eq($sformatf("16-bit read half %0d", half), got[15:0], h);
    end
  endtask

  protected task case_race();
    bit ok;
    sba_wr(scratch_addr, 0);
    send_cmd(8'h10, n_race, ok);  // RACE
    if (!ok) return;
    for (int v = 1; v <= n_race; v++) begin
      bit [2:0] serr;
      bit [31:0] got;
      sba_raw(1'b1, scratch_addr + 2, 3'd0, v, serr, got);
      chk_eq($sformatf("race: 8-bit SBA write %0d", v), serr, 0);
      sba_raw(1'b0, scratch_addr + 1, 3'd0, 0, serr, got);
      chk_eq("race: byte 1 over SBA", got[7:0], 0);
    end
    end_cmd(1'b1, ok);
  endtask

  // The revocation bit of the capability's base, and (CHERIoT) the tag after an SBA write.
  protected task case_revbm_tag();
    bit ok;
    bit [31:0] waddr, w0, got, slot_word;
    int unsigned bitpos;
    send_cmd(8'h11, 0, ok);  // CAP_PREP
    if (!ok) return;
    end_cmd(1'b0, ok);
    if (!ok) return;
    if (cap_base_addr < RevbmCovered || cap_base_addr >= RevbmCovered + 32'h3_0000) begin
      `uvm_error(`gfn, $sformatf("EG2 DM: capability base 0x%08x outside the bitmap's range",
                                 cap_base_addr))
      return;
    end
    waddr = RevbmBase + (((cap_base_addr - RevbmCovered) >> 8) << 2);
    bitpos = ((cap_base_addr - RevbmCovered) >> 3) & 31;
    sba_rd(waddr, w0);
    sba_wr(waddr, w0 | (32'h1 << bitpos));
    sba_rd(waddr, got);
    chk_eq("revocation bitmap word read back over SBA", got, w0 | (32'h1 << bitpos));
    send_cmd(8'h12, 1, ok);  // CAP_CHECK: revoked
    if (!ok) return;
    end_cmd(1'b0, ok);
    chk_eq("the core's view of the bitmap word", revbm_word, w0 | (32'h1 << bitpos));
    sba_wr(waddr, w0);
    send_cmd(8'h12, 0, ok);  // CAP_CHECK: not revoked
    if (!ok) return;
    end_cmd(1'b0, ok);
    chk_eq("the core's view of the bitmap word after the clear", revbm_word, w0);
    if (cheriot) begin
      // REQ_CMS_16: an SBA write over the tagged granule leaves tag 0 (expected to fail: F23).
      sba_rd(cap_slot_addr, slot_word);
      sba_wr(cap_slot_addr, slot_word);
      send_cmd(8'h12, 1, ok);  // CAP_CHECK: written over SBA
      if (!ok) return;
      end_cmd(1'b0, ok);
    end
  endtask

  protected task case_mem_sys();
    bit [31:0] v0, got;
    sba_rd(TbreBase, v0);
    sba_wr(TbreBase, tbre_val);
    sba_rd(TbreBase, got);
    if (cheriot) begin
      chk_eq("CHERIoT: TBRE_BASE_ADDR after an SBA write (locked, REQ_SOC_08)", got, v0);
    end else begin
      chk_eq("RISC-V: TBRE_BASE_ADDR read back over SBA", got, tbre_val);
    end
    sba_wr(TbreBase, v0);
  endtask

  virtual task body();
    bit ok;
    super.body();
    corners_ready(ok);
    if (!ok) return;
    cheriot = rvfi.cheriot_ena === 1'b1;
    expect_bus_err("bus error (in-window hole)", 1'b0, HoleAddr);
    expect_bus_err("unmapped address", 1'b0, GapAddr);
    case_busy();
    case_autoinc();
    case_sizes();
    case_race();
    case_revbm_tag();
    case_mem_sys();
    // Last: one integrity error on an SBA response stays in rv_dm's TL-UL host until rv_dm is
    // reset (tlul_adapter_host intg_err_o "is permanent once detected"; rv_dm's documentation does
    // not say so: GAP-RTLREQ-9), so every later SBA access ends with sberror 7. A power-on reset
    // clears it, so that the done word can be written over SBA after the software's next boot.
    case_integrity();
    begin
      int unsigned b0 = boots, r0 = readies;
      assert_por_reset();
      wait_boot_ready(b0, r0, ok);
      if (!ok) return;
      cfg.debugger.set_dmactive(1);
    end
    finish(32'h14);
  endtask

endclass : chip_eg2_rv_dm_sba_corners_vseq
