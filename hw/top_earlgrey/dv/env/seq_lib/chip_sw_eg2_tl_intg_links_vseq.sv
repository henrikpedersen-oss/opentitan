// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_sw_eg2_tl_intg_links[_cheriot] (EG2 spec REQ_TL_04; software
// sw/device/tests/eg2_tl/eg2_tl_intg_links.c, whose header holds the link and receiver table).
//
// Phase 0: once the software prints `EG2 TL traffic 0x<scratch>`, the sequence (for link sba)
// makes SBA reads and writes of the scratch word, waits for a transfer of the drawn kind on the
// drawn link, and flips bit `bit` of the drawn integrity field for that one clock cycle (forced on
// the link's net in earlgrey_pd_main and released after the receiver's clock edge; the
// receiver's integrity check reads it combinationally). It then checks that the receiver's fatal
// alert reaches the alert handler (ALERT_CAUSE by backdoor), writes boot_phase = 1 into the image
// and resets the chip (power-on). Phase 1 is the software's: the same traffic, every check
// passing, and the status sweep finding a clean chip.
//
// Links 1-3 (revbm, meta, tbre) carry traffic in CHERIoT mode only: the RISC-V image draws link
// 0 (cored) or 4 (sba). The image's mode is taken from +sw_images (the `_cheriot` image name).
// The fault is announced before it is applied.
class chip_sw_eg2_tl_intg_links_vseq extends chip_sw_eg2_chk_base_vseq;
  `uvm_object_utils(chip_sw_eg2_tl_intg_links_vseq)

  typedef enum int unsigned {LinkCored, LinkRevbm, LinkMeta, LinkTbre, LinkSba} link_e;
  typedef enum int unsigned {FieldCmd, FieldWdata, FieldRsp, FieldRdata} field_e;

  localparam string PdMain = "tb.dut.top_earlgrey.earlgrey_pd_main";
  localparam bit [31:0] MainSramBase = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  localparam bit [31:0] MainSramSize = top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_SIZE_BYTES;

  rand link_e         link;
  rand field_e        field;
  rand int unsigned   bit_idx;
  bit                 cheriot_image;
  protected bit       sba_stop;

  constraint link_c {
    if (!cheriot_image) {
      link inside {LinkCored, LinkSba};
    } else {
      link dist {LinkCored := 25, LinkRevbm := 20, LinkMeta := 20, LinkTbre := 15, LinkSba := 20};
    }
    // The revocation lookups and the sweeps only read: no write data on revbm and tbre.
    (link inside {LinkRevbm, LinkTbre}) -> field != FieldWdata;
    field dist {FieldCmd := 30, FieldWdata := 20, FieldRsp := 25, FieldRdata := 25};
    bit_idx inside {[0:6]};
  }

  function new(string name = "");
    super.new(name);
    // SBA traffic for link sba needs the debug module's TAP.
    select_rv_dm_tap = 1'b1;
  endfunction

  function void pre_randomize();
    string images;
    super.pre_randomize();
    cheriot_image = $value$plusargs("sw_images=%s", images) && has_cheriot(images);
  endfunction

  protected function bit has_cheriot(string s);
    for (int i = 0; i + 8 <= s.len(); i++) begin
      if (s.substr(i, i + 7) == "_cheriot") return 1'b1;
    end
    return 1'b0;
  endfunction

  virtual function void write_params();
    set_param("link", link);
    set_param("field", field);
    set_param("bit", bit_idx);
    set_param("boot_phase", 0);
  endfunction

  // The link's request and response nets.
  protected function string link_net(bit rsp);
    case (link)
      LinkCored: return {PdMain, rsp ? ".rv_core_ibex_cored_tl_h_rsp"
                                     : ".rv_core_ibex_cored_tl_h_req"};
      LinkRevbm: return {PdMain, rsp ? ".rv_core_ibex_corerevbm_tl_rsp"
                                     : ".rv_core_ibex_corerevbm_tl_req"};
      LinkMeta:  return {PdMain, rsp ? ".cheriot_mem_sys_meta_sram_tl_rsp"
                                     : ".cheriot_mem_sys_meta_sram_tl_req"};
      LinkTbre:  return {PdMain, rsp ? ".main_tl_cheriot_mem_sys__tbre_rsp"
                                     : ".main_tl_cheriot_mem_sys__tbre_req"};
      default:   return {PdMain, rsp ? ".main_tl_rv_dm__sba_rsp" : ".main_tl_rv_dm__sba_req"};
    endcase
  endfunction

  // The receiver's fatal alert (eg2_tl_intg_links.c, the table in its header).
  protected function int unsigned expected_alert();
    bit to_device = field inside {FieldCmd, FieldWdata};
    case (link)
      LinkCored: return to_device ? TopEarlgreyAlertIdSramCtrlMainFatalError
                                  : TopEarlgreyAlertIdRvCoreIbexFatalHwErr;
      LinkRevbm: return to_device ? TopEarlgreyAlertIdCheriotMemSysFatalFault
                                  : TopEarlgreyAlertIdRvCoreIbexFatalHwErr;
      LinkMeta:  return to_device ? TopEarlgreyAlertIdSramCtrlMetaFatalError
                                  : TopEarlgreyAlertIdCheriotMemSysFatalFault;
      LinkTbre:  return to_device ? TopEarlgreyAlertIdSramCtrlMainFatalError
                                  : TopEarlgreyAlertIdCheriotMemSysFatalFault;
      default:   return to_device ? TopEarlgreyAlertIdSramCtrlMainFatalError
                                  : TopEarlgreyAlertIdRvDmFatalFault;
    endcase
  endfunction

  protected function uvm_hdl_data_t rd(string path);
    uvm_hdl_data_t v;
    `DV_CHECK_FATAL(uvm_hdl_read(path, v), $sformatf("cannot read %s", path))
    return v;
  endfunction

  // Waits (at falling edges, where the link is stable for the receiver's next rising edge) for a
  // transfer of the drawn kind, then flips the bit for one clock cycle.
  protected task inject();
    bit rsp = field inside {FieldRsp, FieldRdata};
    string net = link_net(rsp);
    string fpath = {net, field == FieldCmd   ? ".a_user.cmd_intg"  :
                         field == FieldWdata ? ".a_user.data_intg" :
                         field == FieldRsp   ? ".d_user.rsp_intg"  : ".d_user.data_intg"};
    uvm_hdl_data_t v;
    bit hit;
    for (int cyc = 0; cyc < 2_000_000 && !hit; cyc++) begin
      cfg.chip_vif.cpu_clk_rst_if.wait_n_clks(1);
      if (!rsp) begin
        if (rd({net, ".a_valid"}) != 1) continue;
        if (field == FieldWdata && rd({net, ".a_opcode"}) == tlul_pkg::Get) continue;
        // Device-side faults go to main SRAM, so the receiver is known (cored and sba).
        if (link inside {LinkCored, LinkSba}) begin
          bit [31:0] a = rd({net, ".a_address"});
          if (a < MainSramBase || a >= MainSramBase + MainSramSize) continue;
        end
      end else begin
        if (rd({net, ".d_valid"}) != 1) continue;
        if (field == FieldRdata && rd({net, ".d_opcode"}) != tlul_pkg::AccessAckData) continue;
      end
      hit = 1'b1;
    end
    `DV_CHECK_FATAL(hit, $sformatf("no transfer of the drawn kind on %s", net))
    v = rd(fpath);
    announce("chip_sw_eg2_tl_intg_links_vseq", "alert_handler",
             $sformatf("%s bit %0d flipped (0x%0h -> 0x%0h)", fpath, bit_idx, v,
                       v ^ (1 << bit_idx)));
    `DV_CHECK_FATAL(uvm_hdl_force(fpath, v ^ (1 << bit_idx)), $sformatf("cannot force %s", fpath))
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
    #1ps;
    `DV_CHECK_FATAL(uvm_hdl_release(fpath))
  endtask

  // SBA reads and writes of the software's scratch word until stopped.
  protected task sba_traffic(bit [31:0] addr);
    sba_access_err_e st;
    bit [31:0] v;
    cfg.debugger.set_dmactive(1);
    for (int i = 0; !sba_stop; i++) begin
      sba_wr32(addr, 32'h5ba0_0000 | i, st);
      if (sba_stop) break;
      sba_rd32(addr, v, st);
    end
  endtask

  virtual task body();
    string line;
    bit [31:0] scratch;
    uvm_reg_data_t cause;
    int unsigned alert = expected_alert();
    super.body();
    `uvm_info(`gfn, $sformatf(
        "EG2 TL intg: %s image, link %s, field %s, bit %0d, receiver alert %0d",
                              cheriot_image ? "CHERIoT" : "RISC-V", link.name(), field.name(),
                              bit_idx, alert), UVM_LOW)
    wait_sw_line("EG2 TL traffic", line);
    if (!($sscanf(line, "EG2 TL traffic 0x%h", scratch) == 1)) `uvm_fatal(`gfn, $sformatf("Check failed ($sscanf(line, \"EG2 TL traffic 0x%%h\", scratch) == 1) %s", ""))
    sba_stop = 1'b0;
    fork
      if (link == LinkSba) sba_traffic(scratch);
      begin
        inject();
        sba_stop = 1'b1;
      end
    join
    // The receiver's alert at the alert handler.
    for (int i = 0; i < 2000; i++) begin
      csr_rd(.ptr(ral.alert_handler.alert_cause[alert]), .value(cause), .backdoor(1));
      if (cause[0]) break;
      cfg.chip_vif.cpu_clk_rst_if.wait_clks(10);
    end
    `DV_CHECK_EQ(cause[0], 1, $sformatf("receiver's fatal alert %0d not at the alert handler",
                                        alert))
    // Phase 1: the same image after a power-on reset.
    set_param("boot_phase", 1);
    apply_reset("HARD");
  endtask

endclass : chip_sw_eg2_tl_intg_links_vseq
