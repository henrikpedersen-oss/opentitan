// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// The mode-independent layer of the EG2 constrained-random tests (eg2_top_verification, spec
// "Constraints in SystemVerilog": chip_sw_eg2_rand_vseq).
//
// Class families and their weight arrays, under the suite's weight rule: every class 1-100; one
// run in five one class drawn at random gets half of the total; otherwise the boundary classes
// together at least 60 %, the plain ones at most 30 % and at least 5 %. The families here are the
// integer operand, the address against the memory map, the access and the timing of an event. A
// test's vseq writes the weights its software reads (`write_family`); the CHERIoT layer
// (chip_sw_eg2_cheriot_vseq) adds the capability families.
//
// Bench steps. A test that needs the bench prints `EG2 BENCH <n> <op> <a> <b>` on UART0 (n decimal,
// or hex after an `x`; a and b hex). This class reads UART0's lines (as chip_sw_eg2_base_vseq
// does, logging each one), calls `bench_op(op, a, b, result)` for each step, then writes n and
// the result into kEg2BenchAck (eg2_memsys.c), which the software polls. The ops every EG2 test
// may use are here (SBA accesses, resets, faults); a test's vseq overrides bench_op for its own
// and calls super for the rest. Every deliberate fault is announced to the injection registry
// (decision 2026-10-24; announce_fault).
class chip_sw_eg2_rand_vseq extends chip_sw_eg2_base_vseq;
  `uvm_object_utils(chip_sw_eg2_rand_vseq)

  `uvm_object_new

  // ------------------------------------------------------------------ families

  typedef enum int unsigned {
    OpZero, OpOne, OpMinusOne, OpMin, OpMax, OpPow2, OpPow2Near, OpShared, OpUniform, OpNum
  } operand_e;
  localparam bit [OpNum-1:0] OperandPlain = 1 << OpUniform;

  typedef enum int unsigned {
    MapFirstGranule, MapLastGranule, MapBoundary, MapDevFirst, MapDevLast, MapUnmapped,
    MapInterior, MapNum
  } map_e;
  localparam bit [MapNum-1:0] MapPlain = 1 << MapInterior;

  typedef enum int unsigned {
    AccByte, AccHalf, AccWord, AccCap, AccMisaligned, AccNum
  } access_e;
  localparam bit [AccNum-1:0] AccessPlain = 1 << AccWord;

  typedef enum int unsigned {
    WhenNone, WhenBackToBack, WhenStall, WhenWfi, WhenTrap, WhenNum
  } when_e;
  localparam bit [WhenNum-1:0] WhenPlain = 1 << WhenNone;

  rand int unsigned w_operand[OpNum];
  rand int unsigned w_map[MapNum];
  rand int unsigned w_access[AccNum];
  rand int unsigned w_when[WhenNum];
  rand bit          boost;
  rand int unsigned boosted;

  constraint boost_c {
    solve boost, boosted before w_operand, w_map, w_access, w_when;
    boost dist {1'b1 := 1, 1'b0 := 4};
    boosted < 16;
  }

  // The weight rule, one block per family (the cursor example of the spec, generalised).
  constraint w_operand_c {
    foreach (w_operand[i]) w_operand[i] inside {[1:100]};
    if (boost && boosted < OpNum) {
      w_operand[boosted] * 2 == w_operand.sum();
    } else {
      (w_operand.sum() with (OperandPlain[item.index] ? 0 : item)) * 10 >= w_operand.sum() * 6;
      (w_operand.sum() with (OperandPlain[item.index] ? item : 0)) * 10 <= w_operand.sum() * 3;
      (w_operand.sum() with (OperandPlain[item.index] ? item : 0)) * 20 >= w_operand.sum();
    }
  }
  constraint w_map_c {
    foreach (w_map[i]) w_map[i] inside {[1:100]};
    if (boost && boosted < MapNum) {
      w_map[boosted] * 2 == w_map.sum();
    } else {
      (w_map.sum() with (MapPlain[item.index] ? 0 : item)) * 10 >= w_map.sum() * 6;
      (w_map.sum() with (MapPlain[item.index] ? item : 0)) * 10 <= w_map.sum() * 3;
      (w_map.sum() with (MapPlain[item.index] ? item : 0)) * 20 >= w_map.sum();
    }
  }
  constraint w_access_c {
    foreach (w_access[i]) w_access[i] inside {[1:100]};
    if (boost && boosted < AccNum) {
      w_access[boosted] * 2 == w_access.sum();
    } else {
      (w_access.sum() with (AccessPlain[item.index] ? 0 : item)) * 10 >= w_access.sum() * 6;
      (w_access.sum() with (AccessPlain[item.index] ? item : 0)) * 10 <= w_access.sum() * 3;
      (w_access.sum() with (AccessPlain[item.index] ? item : 0)) * 20 >= w_access.sum();
    }
  }
  constraint w_when_c {
    foreach (w_when[i]) w_when[i] inside {[1:100]};
    if (boost && boosted < WhenNum) {
      w_when[boosted] * 2 == w_when.sum();
    } else {
      (w_when.sum() with (WhenPlain[item.index] ? 0 : item)) * 10 >= w_when.sum() * 6;
      (w_when.sum() with (WhenPlain[item.index] ? item : 0)) * 10 <= w_when.sum() * 3;
      (w_when.sum() with (WhenPlain[item.index] ? item : 0)) * 20 >= w_when.sum();
    }
  }

  // Writes a family's weights as kEg2Param_<names[i]>.
  protected function void write_family(string names[], int unsigned w[]);
    `DV_CHECK_EQ_FATAL(names.size(), w.size(), "a family's names and weights differ in number")
    foreach (names[i]) set_param(names[i], w[i]);
  endfunction

  // The knobs every test leaves at 0 on the chip: the model's gaps are the model's.
  virtual function void write_params();
    super.write_params();
  endfunction

  // ------------------------------------------------------------------ bench steps

  // Steps done, for the log.
  int unsigned bench_steps;
  protected bit ack_is_mailbox;  // kEg2BenchAck registered with the instruction-set model

  virtual task body();
    configure_uart_agent(.uart_idx(0), .enable(1));
    cfg.eg2_sb_uart_required[0] = 1'b1;
    fork
      forward_dv_console();
      uart_lines_and_steps();
      log_boot_profile();
    join_none
    // chip_sw_eg2_base_vseq's body without its UART reader, which this class replaces.
    chip_sw_base_vseq::body();
  endtask

  // UART0's monitor FIFO carries both console channels (chip_sw_eg2_base_vseq::console_char).
  protected task uart_lines_and_steps();
    uart_item item;
    string line;
    console_line_t cl;
    forever begin
      p_sequencer.uart_tx_fifos[0].get(item);
      if (console_char(item, cl, line)) begin
        log_console_line(line, cl);
        if (line.substr(0, 9) == "EG2 BENCH " && line.len() > 10) do_step(line);
      end
    end
  endtask

  // `EG2 BENCH <n> <op> <a> <b>`; a step the software marks "skipped" is not one.
  protected task do_step(string line);
    string tok[$];
    string cur;
    bit [31:0] n, a, b, result;
    for (int i = 10; i < line.len(); i++) begin
      if (line[i] == " ") begin
        if (cur != "") tok.push_back(cur);
        cur = "";
      end else begin
        cur = {cur, string'(line[i])};
      end
    end
    if (cur != "") tok.push_back(cur);
    if (tok.size() < 4 || (tok.size() > 4 && tok[4] == "skipped")) return;
    n = (tok[0][0] == "x") ? tok[0].substr(1, tok[0].len() - 1).atohex() : tok[0].atoi();
    a = tok[2].atohex();
    b = tok[3].atohex();
    `uvm_info(`gfn, $sformatf("EG2 bench step %0d: %s 0x%08x 0x%08x", n, tok[1], a, b), UVM_LOW)
    result = 0;
    bench_op(tok[1], a, b, result);
    bench_steps++;
    ack(n, result);
  endtask

  // The answer: kEg2BenchAck = {n, result}, in RRAM.
  protected task ack(bit [31:0] n, bit [31:0] result);
    bit [7:0] bytes[8];
    for (int i = 0; i < 4; i++) begin
      bytes[i] = n[8 * i +: 8];
      bytes[4 + i] = result[8 * i +: 8];
    end
    // Not wait_rram_rd_buf_rdy's 1 us: the software polls this RRAM word, with a pause between
    // polls (eg2_memsys.c), so the buffer is idle once per poll period, not within 1 us.
    `DV_WAIT(cfg.chip_vif.rram_rd_buf_rdy === 1'b1, "RRAM-rd-buf timeout (bench ack)", 100_000)
    // The software polls this word while the bench writes it: the instruction-set model takes the
    // chip's reads of it as they come (a mailbox), it cannot know which poll sees the new value.
    if (!ack_is_mailbox) begin
      bit [bus_params_pkg::BUS_AW-1:0] a;
      uint sz;
      if (dv_utils_pkg::sw_symbol_get_addr_size($sformatf("%0s.elf", cfg.sw_images[SwTypeTestSlotA]),
                                                "kEg2BenchAck", 1'b1, a, sz)) begin
        eg2_iss_wrapper::backdoor_mailbox(a, sz);
      end
      ack_is_mailbox = 1'b1;
    end
    sw_symbol_backdoor_overwrite("kEg2BenchAck", bytes);
  endtask

  // The ops. Unknown ones are a bench error.
  virtual task bench_op(string op, bit [31:0] a, bit [31:0] b, output bit [31:0] result);
    result = 0;
    case (op)
      "sba_read": sba_access_w(1'b0, a, b, 32'h0, result);
      "sba_rmw": begin
        bit [31:0] err, v;
        sba_read32(a & ~32'h3, v, err);
        sba_access_w(1'b1, a, b, v >> (8 * (a % 4)), result);
        result |= err;
      end
      "sba_wr32": sba_access_w(1'b1, a, 32, b, result);
      "sba_fill": sba_fill(a, b, result);
      "por": begin
        // The software's next boot knows it follows this reset by kEg2Param_por_done.
        // Written while POR is asserted, before any software can read it.
        `uvm_info(`gfn, "EG2 bench: power-on reset", UVM_LOW)
        cfg.chip_vif.por_n_if.drive(0);
        repeat (6) @cfg.chip_vif.pwrmgr_low_power_if.cb;
        set_param("por_done", 1);
        cfg.clk_rst_vif.wait_clks(10);
        cfg.chip_vif.por_n_if.drive(1);
      end
      "ndm_reset": ndm_reset();
      default: `uvm_fatal(`gfn, $sformatf("EG2 bench step %s: no such op in %s", op, get_type_name()))
    endcase
  endtask

  // ------------------------------------------------------------------ SBA

  // The debug module: SBA in this test needs the RV_DM TAP strap (set in the test vseq's
  // pre_start, `use_jtag`) and a debug-enabled life-cycle state.
  bit use_jtag;

  virtual task pre_start();
    if (use_jtag) cfg.chip_vif.tap_straps_if.drive(JtagTapRvDm);
    super.pre_start();
  endtask

  protected bit dm_active;
  protected task dm_on();
    if (!dm_active) begin
      `DV_CHECK_FATAL(use_jtag, "an SBA bench step in a vseq without use_jtag")
      cfg.debugger.set_dmactive(1);
      dm_active = 1;
    end
  endtask

  protected task sba_read32(bit [31:0] addr, output bit [31:0] value, output bit [31:0] err);
    logic [31:0] q[$];
    sba_access_err_e status;
    dm_on();
    cfg.debugger.sba_read(addr, q, status);
    value = q.size() ? q[0] : 0;
    err = 32'(status);
  endtask

  // One SBA access of `width` bits (8, 16, 32) at `addr`; result = sberror.
  protected task sba_access_w(bit write, bit [31:0] addr, bit [31:0] width, bit [31:0] data,
                              output bit [31:0] result);
    sba_access_item item = sba_access_item::type_id::create("eg2_sba");
    dm_on();
    item.bus_op = write ? BusOpWrite : BusOpRead;
    item.addr = addr;
    item.size = width == 8 ? SbaAccessSize8b : width == 16 ? SbaAccessSize16b : SbaAccessSize32b;
    item.autoincrement = 0;
    item.readonaddr = !write;
    item.readondata = 0;
    if (write) item.wdata.push_back(data << (8 * (addr % 4)));
    cfg.debugger.sba_access(item);
    result = 32'(item.is_err);
    // The instruction-set model reads its own memory since review R6 (2026-10-25): it gets the
    // bytes the bench wrote.
    if (write && !item.is_err) begin
      bit [7:0] b[] = new[width / 8];
      foreach (b[i]) b[i] = data[8 * i +: 8];
      eg2_iss_wrapper::backdoor_write(addr, b);
    end
    `uvm_info(`gfn, $sformatf("EG2 SBA %s 0x%08x, %0d bits: sberror %0d", write ? "write" : "read",
                              addr, width, item.is_err), UVM_LOW)
  endtask

  // SBA writes over [addr, addr + len) at a width drawn once, with random data.
  protected task sba_fill(bit [31:0] addr, bit [31:0] len, output bit [31:0] result);
    int unsigned width = 8 << $urandom_range(0, 2);
    result = 0;
    for (bit [31:0] p = addr; p < addr + len; p += width / 8) begin
      bit [31:0] err;
      sba_access_w(1'b1, p, width, $urandom(), err);
      result |= err;
    end
  endtask

  protected task ndm_reset();
    dm_on();
    `uvm_info(`gfn, "EG2 bench: NDM reset", UVM_LOW)
    csr_wr(.ptr(jtag_dmi_ral.dmcontrol.ndmreset), .value(1), .blocking(1), .predict(1));
    cfg.chip_vif.aon_clk_por_rst_if.wait_clks($urandom_range(5, 20));
    csr_wr(.ptr(jtag_dmi_ral.dmcontrol.ndmreset), .value(0), .blocking(1), .predict(1));
  endtask

  // ------------------------------------------------------------------ faults

  // The registry id of the last bench fault announced (-1: none).
  protected int last_fault = -1;

  // Announces a bench fault at `path` to the instruction check (eg2_inject_registry). The model is
  // not told of it, so a mismatch it causes is EXPECTED, and the check then stops until the chip
  // reset the fault must escalate to (`diverges`; a stop no reset ends fails the run). A fault
  // that changes nothing the core sees is not an error (must_detect 0). Four mismatches: the
  // instructions the model compared before the stop took effect. 2026-10-28, chip_cheriot_fi_meta.
  protected function void announce_fault(string path, string kind);
    last_fault = eg2_inject_registry::announce({"bench fault at ", path}, "insn", kind, $realtime,
                                               eg2_inject_registry::UntilConsumed, 1'b0, 4, 1'b1);
  endfunction

  // One clock of the core's clock domain.
  protected task cpu_clk();
    cfg.chip_vif.cpu_clk_rst_if.wait_clks(1);
  endtask

  // Waits (at most `timeout` cycles) for a TL-UL A-channel request at `path` with an address in
  // [lo, hi), then forces `bad` on the net for one cycle. `bad` is computed by `corrupt_a`.
  protected task corrupt_next_a(string path, bit [31:0] lo, bit [31:0] hi, int field, int bitpos,
                                int timeout = 200000);
    uvm_hdl_data_t v;
    tlul_pkg::tl_h2d_t h;
    for (int i = 0; i < timeout; i++) begin
      `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
      h = tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
      if (h.a_valid && h.a_address >= lo && h.a_address < hi) begin
        case (field)
          0: h.a_user.cmd_intg[bitpos % 7] ^= 1'b1;
          1: h.a_user.data_intg[bitpos % 7] ^= 1'b1;
          default: h.a_data[bitpos % 32] ^= 1'b1;
        endcase
        announce_fault(path, $sformatf("A-channel field %0d bit %0d at 0x%08x", field, bitpos,
                                       h.a_address));
        `uvm_info(`gfn, $sformatf("EG2 fault: %s A-channel field %0d bit %0d at 0x%08x", path,
                                  field, bitpos, h.a_address), UVM_LOW)
        `DV_CHECK_FATAL(uvm_hdl_force(path, uvm_hdl_data_t'(h)), path)
        cpu_clk();
        `DV_CHECK_FATAL(uvm_hdl_release(path), path)
        return;
      end
      cpu_clk();
    end
    `uvm_error(`gfn, $sformatf("EG2 fault: no request at %s in [0x%08x, 0x%08x)", path, lo, hi))
  endtask

  // The same on a D channel: the next response with d_valid.
  protected task corrupt_next_d(string path, int field, int bitpos, int timeout = 200000);
    uvm_hdl_data_t v;
    tlul_pkg::tl_d2h_t d;
    for (int i = 0; i < timeout; i++) begin
      `DV_CHECK_FATAL(uvm_hdl_read(path, v), path)
      d = tlul_pkg::tl_d2h_t'(v[$bits(tlul_pkg::tl_d2h_t)-1:0]);
      if (d.d_valid) begin
        case (field)
          0: d.d_user.rsp_intg[bitpos % 7] ^= 1'b1;
          1: d.d_user.data_intg[bitpos % 7] ^= 1'b1;
          default: d.d_data[bitpos % 32] ^= 1'b1;
        endcase
        announce_fault(path, $sformatf("D-channel field %0d bit %0d", field, bitpos));
        `uvm_info(`gfn, $sformatf("EG2 fault: %s D-channel field %0d bit %0d", path, field,
                                  bitpos), UVM_LOW)
        `DV_CHECK_FATAL(uvm_hdl_force(path, uvm_hdl_data_t'(d)), path)
        cpu_clk();
        `DV_CHECK_FATAL(uvm_hdl_release(path), path)
        return;
      end
      cpu_clk();
    end
    `uvm_error(`gfn, $sformatf("EG2 fault: no response at %s", path))
  endtask

  // The response to the next request in [lo, hi) on the A channel at `apath`, corrupted on the D
  // channel at `dpath` (the core's data port answers in order, one request at a time).
  protected task corrupt_d_after_a(string apath, string dpath, bit [31:0] lo, bit [31:0] hi,
                                   int field, int bitpos, int timeout = 200000);
    uvm_hdl_data_t v;
    tlul_pkg::tl_h2d_t h;
    for (int i = 0; i < timeout; i++) begin
      `DV_CHECK_FATAL(uvm_hdl_read(apath, v), apath)
      h = tlul_pkg::tl_h2d_t'(v[$bits(tlul_pkg::tl_h2d_t)-1:0]);
      if (h.a_valid && h.a_address >= lo && h.a_address < hi) begin
        corrupt_next_d(dpath, field, bitpos, timeout);
        return;
      end
      cpu_clk();
    end
    `uvm_error(`gfn, $sformatf("EG2 fault: no request at %s in [0x%08x, 0x%08x)", apath, lo, hi))
  endtask

  // A MuBi4 sideband (the tag) forced to an encoding other than True and False for the next
  // cycle in which the TL-UL channel at `valid_path` (a whole tl_h2d_t or tl_d2h_t, whose valid
  // is its most significant bit, `valid_msb`) is valid, with an address in [lo, hi) for an A
  // channel.
  protected task corrupt_mubi(string path, string valid_path, int valid_msb, bit [31:0] lo = 0,
                              bit [31:0] hi = 32'hffff_ffff, int timeout = 200000);
    uvm_hdl_data_t valid;
    tlul_pkg::tl_h2d_t h2d;
    bit [3:0] bad;
    do bad = 4'($urandom()); while (bad inside {prim_mubi_pkg::MuBi4True,
                                                 prim_mubi_pkg::MuBi4False});
    for (int i = 0; i < timeout; i++) begin
      `DV_CHECK_FATAL(uvm_hdl_read(valid_path, valid), valid_path)
      // Cast first: Xcelium does not parse a member select on a cast (*E,EXPRPA).
      h2d = tlul_pkg::tl_h2d_t'(valid[$bits(tlul_pkg::tl_h2d_t)-1:0]);
      if (valid[valid_msb] && (valid_msb != $bits(tlul_pkg::tl_h2d_t) - 1 ||
          (h2d.a_address inside {[lo:hi-1]}))) begin
        announce_fault(path, $sformatf("MuBi4 0x%0x", bad));
        `uvm_info(`gfn, $sformatf("EG2 fault: %s = 0x%0x", path, bad), UVM_LOW)
        `DV_CHECK_FATAL(uvm_hdl_force(path, uvm_hdl_data_t'(bad)), path)
        cpu_clk();
        `DV_CHECK_FATAL(uvm_hdl_release(path), path)
        return;
      end
      cpu_clk();
    end
    `uvm_error(`gfn, $sformatf("EG2 fault: %s never valid", valid_path))
  endtask

endclass : chip_sw_eg2_rand_vseq
