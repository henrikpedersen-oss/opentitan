// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// chip_rv_dm_sba_tl_errors (EG2 spec REQ_TL_03 "the same cases over SBA, sberror read back through
// JTAG"), both modes. Each explicit TL-UL error case is made by rv_dm's system bus access, and
// sbcs.sberror must report it (RISC-V Debug Specification 0.13.2, section 3.12.18, sbcs.sberror):
//
// | case | SBA access | expected sberror |
// |---|---|---|
// | 0 gap | 32-bit read or write after a device window that | 2 (bad address) or 7 |
// | |   no other window holds |   (other): d_error |
// | 1 in_window | 32-bit read or write at an offset past a | 2 or 7: d_error (reggen |
// | |   register window's last register |   "Error responses") |
// | 2 subword_write | 8- or 16-bit write to a 32-bit register | 2 or 7: d_error (reggen); |
// | | |   register unchanged |
// | 3 rom_write | 32-bit write to the ROM | 2 or 7; word unchanged |
// | 4 rram_write | 32-bit write to the RRAM host window (scratch | 2 or 7; word unchanged |
// | |   pages) |   (RFC RRAM §3.1) |
// | 5 misaligned | 16-bit access at an odd address, or 32-bit at | 3 (alignment), made by the |
// | |   an address not 4-aligned |   debug module itself |
// | 6 bad_size | 64-bit access (sbcs.sbaccess64 reads 0) | 4 (unsupported size) |
//
// The debug specification leaves to the implementation which of 2 and 7 a bus error reports, so
// both are accepted for d_error. After each error sberror is cleared by writing 1 and must read
// 0; "unchanged" is read back over SBA. Finally a good SBA write and read of the software's
// scratch word must work, then the software's loop is ended (finish). Ticket F23: SBA reaches
// main and sec SRAM around cheriot_mem_sys; no case here depends on it.
class chip_eg2_rv_dm_sba_tl_errors_vseq extends chip_eg2_rv_dm_base_vseq;
  `uvm_object_utils(chip_eg2_rv_dm_sba_tl_errors_vseq)
  `uvm_object_new

  typedef enum int unsigned {
    CaseGap, CaseInWindow, CaseSubwordWrite, CaseRomWrite, CaseRramWrite, CaseMisaligned,
    CaseBadSize, CaseNum
  } sba_case_e;

  rand int unsigned n_cases;
  rand sba_case_e   cases[];
  rand bit          writes[];
  rand int unsigned picks[];

  constraint cases_c {
    n_cases inside {[CaseNum:24]};
    cases.size() == n_cases;
    writes.size() == n_cases;
    picks.size() == n_cases;
    // Every case at least once, then by weight.
    foreach (cases[i]) {
      if (i < CaseNum) {
        cases[i] == sba_case_e'(i);
      } else {
        cases[i] dist {CaseGap := 20, CaseInWindow := 15, CaseSubwordWrite := 20,
                       CaseRomWrite := 10, CaseRramWrite := 10, CaseMisaligned := 15,
                       CaseBadSize := 10};
      }
      picks[i] < 1024;
    }
  }

  // Windows whose first address after them is unmapped (top_earlgrey_pkg; the generated
  // window table of eg2_tables.h has no window there).
  localparam bit [31:0] GapAddrs[6] = '{
    top_earlgrey_pkg::TOP_EARLGREY_UART0_BASE_ADDR +
        top_earlgrey_pkg::TOP_EARLGREY_UART0_SIZE_BYTES,
    top_earlgrey_pkg::TOP_EARLGREY_GPIO_BASE_ADDR + top_earlgrey_pkg::TOP_EARLGREY_GPIO_SIZE_BYTES,
    top_earlgrey_pkg::TOP_EARLGREY_RV_TIMER_BASE_ADDR +
        top_earlgrey_pkg::TOP_EARLGREY_RV_TIMER_SIZE_BYTES,
    top_earlgrey_pkg::TOP_EARLGREY_I2C0_BASE_ADDR + top_earlgrey_pkg::TOP_EARLGREY_I2C0_SIZE_BYTES,
    top_earlgrey_pkg::TOP_EARLGREY_AES_BASE_ADDR + top_earlgrey_pkg::TOP_EARLGREY_AES_SIZE_BYTES,
    top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR +
        top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_SIZE_BYTES
  };
  // Register windows and an offset past their last register (generated register headers).
  localparam bit [31:0] HoleAddrs[4] = '{
    top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR + 'h2c0,
    top_earlgrey_pkg::TOP_EARLGREY_UART1_BASE_ADDR + 'h38,
    top_earlgrey_pkg::TOP_EARLGREY_RV_TIMER_BASE_ADDR + 'h120,
    top_earlgrey_pkg::TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR + 'h24
  };
  // 32-bit registers with no side effect (rram_ctrl SCRATCH, spi_host0 CONFIGOPTS, i2c0 TIMING0).
  localparam bit [31:0] WideRegs[3] = '{
    top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_CORE_BASE_ADDR + 'h2a4,
    top_earlgrey_pkg::TOP_EARLGREY_SPI_HOST0_BASE_ADDR + 'h18,
    top_earlgrey_pkg::TOP_EARLGREY_I2C0_BASE_ADDR + 'h3c
  };
  localparam bit [31:0] RramScratch = top_earlgrey_pkg::TOP_EARLGREY_RRAM_CTRL_HOST_BASE_ADDR +
                                      3584 * 512;

  // One SBA access with its size; returns the reported error and the data read.
  protected task sba_one(bit write, bit [31:0] addr, sba_access_size_e size, bit [31:0] wdata,
                         output sba_access_err_e err, output bit [31:0] rdata);
    sba_access_item item = sba_access_item::type_id::create("item");
    item.bus_op = write ? BusOpWrite : BusOpRead;
    item.addr = addr;
    item.size = size;
    item.readonaddr = !write;
    item.readondata = 1'b0;
    item.autoincrement = 0;
    item.wdata.push_back(wdata);
    cfg.debugger.sba_access(item);  // clears sberror after reading it
    `DV_CHECK(!item.timed_out, $sformatf("SBA access to 0x%08x timed out", addr))
    err = item.is_err;
    rdata = item.rdata.size() > 0 ? item.rdata[0] : '0;
  endtask

  protected task expect_err(string what, bit write, bit [31:0] addr, sba_access_size_e size,
                            bit [31:0] wdata, sba_access_err_e exp[$]);
    sba_access_err_e err;
    bit [31:0] rdata;
    uvm_reg_data_t sbcs;
    sba_one(write, addr, size, wdata, err, rdata);
    `uvm_info(`gfn, $sformatf("EG2 SBA %s: %s 0x%08x size %s -> sberror %s", what,
                              write ? "write" : "read", addr, size.name(), err.name()), UVM_LOW)
    `DV_CHECK(err inside {exp}, $sformatf("%s: sberror %s, expected one of %p", what, err.name(),
                                          exp))
    csr_rd(.ptr(cfg.jtag_dmi_ral.sbcs), .value(sbcs), .blocking(1));
    `DV_CHECK_EQ(get_field_val(cfg.jtag_dmi_ral.sbcs.sberror, sbcs), 0,
                 $sformatf("%s: sberror after its clear", what))
  endtask

  protected task rd32(bit [31:0] addr, output bit [31:0] v);
    sba_access_err_e st;
    sba_rd32(addr, v, st);
    `DV_CHECK_EQ(st, SbaErrNone, $sformatf("SBA read of 0x%08x", addr))
  endtask

  virtual task body();
    sba_access_err_e bus_err[$] = '{SbaErrBadAddr, SbaErrOther};
    bit [31:0] val_before, val_after, wdata, v;
    sba_access_err_e st;
    super.body();
    wait_ready();
    foreach (cases[i]) begin
      wdata = {picks[i][9:0], 22'h2a5a5a} ^ 32'h1357_9bdf;
      case (cases[i])
        CaseGap: begin
          bit [31:0] a = GapAddrs[picks[i] % 6];
          expect_err("gap", writes[i], a, SbaAccessSize32b, wdata, bus_err);
        end
        CaseInWindow: begin
          bit [31:0] a = HoleAddrs[picks[i] % 4];
          expect_err("in-window hole", writes[i], a, SbaAccessSize32b, wdata, bus_err);
        end
        CaseSubwordWrite: begin
          bit [31:0] r = WideRegs[picks[i] % 3];
          bit half = picks[i][4];
          bit [31:0] a = half ? r + 2 * picks[i][5] : r + picks[i][6:5];
          rd32(r, val_before);
          expect_err("sub-word register write", 1'b1, a,
                     half ? SbaAccessSize16b : SbaAccessSize8b, wdata, bus_err);
          rd32(r, val_after);
          `DV_CHECK_EQ(val_after, val_before,
                       $sformatf("register 0x%08x after the refused write", r))
        end
        CaseRomWrite, CaseRramWrite: begin
          bit [31:0] a = cases[i] == CaseRomWrite ?
              top_earlgrey_pkg::TOP_EARLGREY_ROM_CTRL_ROM_BASE_ADDR + 4 * picks[i] :
              RramScratch + 4 * picks[i];
          rd32(a, val_before);
          expect_err(cases[i] == CaseRomWrite ? "rom write" : "rram host write", 1'b1, a,
                     SbaAccessSize32b, wdata, bus_err);
          rd32(a, val_after);
          `DV_CHECK_EQ(val_after, val_before,
                       $sformatf("word 0x%08x after the refused write", a))
        end
        CaseMisaligned: begin
          bit half = picks[i][0];
          bit [31:0] a = eg2_debug_scratch_word() + (half ? 1 : 1 + picks[i][2:1] % 3);
          expect_err("misaligned", writes[i], a, half ? SbaAccessSize16b : SbaAccessSize32b,
                     wdata, '{SbaErrBadAlignment});
        end
        default: begin
          expect_err("64-bit", writes[i], eg2_debug_scratch_word(), SbaAccessSize64b, wdata,
                     '{SbaErrBadSize});
        end
      endcase
    end
    // A good access still works after the errors.
    sba_wr32(scratch_addr, 32'hc0de_0000 | n_cases, st);
    `DV_CHECK_EQ(st, SbaErrNone, "good SBA write after the errors")
    rd32(scratch_addr, v);
    `DV_CHECK_EQ(v, 32'hc0de_0000 | n_cases, "good SBA read after the errors")
    finish(4);
  endtask

  protected function bit [31:0] eg2_debug_scratch_word();
    return scratch_addr;
  endfunction

endclass : chip_eg2_rv_dm_sba_tl_errors_vseq
