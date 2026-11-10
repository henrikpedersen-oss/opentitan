// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The OTP plug unit bench's own part (edagen rram_ctrl_otp.yaml dv.files), on edagen's generated
// skeleton (agents ot on clk_otp_i and ct on clk_i):
//   - rram_ctrl_otp_cmodel_vseq, two threads:
//       otp side (ot): otp_ctrl as theory_of_operation.md "Generalized Open-source Interface" and
//         otp_ctrl's theory of operation describe it: Init first, then commands with the ready /
//         valid handshake, at most two outstanding, random command, size, address (aligned to the
//         size, now and then not; a hot set of blocks, the whole depth, past the OTP pages) and
//         write data (mostly only setting bits, sometimes clearing one);
//       controller side (ct): a model of what sits behind the plug, written from rram_ctrl's
//         documentation: CONTROL (registers.md: Read returns NUM + 1 bus words, a Write takes whole
//         RRAM words, otherwise op_err), the OTP rule of memory protection (theory_of_operation.md
//         "Memory Protection for OTP Hardware Plug": the OTP pages only, everything else mp_err),
//         a read error returning all-ones words with valid integrity from the failing word on
//         (programmers_guide.md "Errors During Multi-Word Controller Reads"), rram_disable
//         blocking every new request (the memory-protection reference model's reading: an mp_err),
//         TL-UL data integrity on every bus word (inv 39/32), rram_init_done and rram_wr_busy; the
//         RRAM words of the six OTP pages are the bench memory.
//     A command that gets a fault (an RRAM read or write error, a corrupted readback of a data
//     word after it was written, a bus-integrity error) runs alone, so the faults of its accesses
//     are known when its response arrives. Commands run two at a time only on different blocks.
//   - rram_ctrl_otp_cmodel_scoreboard: every response against the OTP plug reference model
//     (eg2_rram_otp_core.h through eg2_rram_otp_dpi.cc), in order; the command's 64-bit block and
//     its syndrome in the bench memory against the model's after its response, the whole region
//     every 64 commands and at the end; requests outside the OTP pages only for commands addressed
//     past them; no response without a command, every command answered, fatal_err / intg_err low;
//     the functional coverage.
//   - tests: rram_ctrl_otp_cmodel_random_test; rram_ctrl_otp_cmodel_random_inject_fail_test (one
//     predicted value corrupted, must fail); rram_ctrl_otp_cmodel_fatal_test (+fatal=0 disable,
//     1 undefined command, 2 bus-integrity error on a read word; then only that commands are still
//     answered, with an error after disable, and intg_err for 2).
// What the model does not predict is counted, and the bench memory of that command's block is
// copied into the model (counted as resyncs).
package rram_ctrl_otp_cmodel_pkg;
  import uvm_pkg::*;
  `include "uvm_macros.svh"
  import rram_ctrl_otp_ot_agent_pkg::*;
  import rram_ctrl_otp_ct_agent_pkg::*;
  import rram_ctrl_otp_env_pkg::*;
  import rram_ctrl_otp_test_pkg::*;

  import "DPI-C" function chandle eg2_rram_otp_create();
  import "DPI-C" function void eg2_rram_otp_release(chandle h);
  import "DPI-C" function void eg2_rram_otp_poke(chandle h, int unsigned off, int unsigned v);
  import "DPI-C" function int unsigned eg2_rram_otp_peek(chandle h, int unsigned off);
  import "DPI-C" function int unsigned eg2_rram_otp_hamming(input bit [63:0] d);
  import "DPI-C" function int eg2_rram_otp_cmd(chandle h, int unsigned cmd, int unsigned size,
      int unsigned addr, input bit [63:0] wdata, int rd_err, int wr_err, int readback_corrupt,
      int disabled, output bit [63:0] rdata, output bit [63:0] rmask, output int mem_pred,
      output int unsigned blk_lo, output int unsigned blk_hi);

  localparam bit [6:0] CmdRead = 7'h7a, CmdWrite = 7'h49, CmdReadRaw = 7'h54, CmdWriteRaw = 7'h67,
                       CmdInit = 7'h20, CmdZeroize = 7'h3d;
  localparam bit [3:0] MuBi4True = 4'h6, MuBi4False = 4'h9;
  localparam int unsigned RegionBytes = 3072, IntgBytes = 512, DataBytes = 2560;
  localparam bit [20:0] IntgStartAddr = 21'h1ff400;
  localparam int unsigned MaxRspWait = 6000;  // otp cycles from acceptance to response

  // Fault kinds of a command that runs alone.
  typedef enum int {FNone, FRdErr, FWrErr, FReadback, FCorruptData, FCorruptSyn, FIntg} fault_e;

  function automatic string cmd_name(bit [6:0] c);
    case (c)
      CmdRead: return "Read";
      CmdWrite: return "Write";
      CmdReadRaw: return "ReadRaw";
      CmdWriteRaw: return "WriteRaw";
      CmdInit: return "Init";
      CmdZeroize: return "Zeroize";
      default: return $sformatf("cmd %02h", c);
    endcase
  endfunction

  function automatic bit [38:0] bus_word(bit [31:0] d);
    return prim_secded_pkg::prim_secded_inv_39_32_enc(d);
  endfunction

  // ---------------------------------------------------------------------------------------------
  // What the stimulus, the controller-path model and the scoreboard share.
  class otp_bench;
    static bit [7:0] mem[RegionBytes];  // the OTP pages as the controller path holds them

    // The command running alone with a fault (both clock domains look at these).
    static fault_e fault = FNone;
    static bit fault_on;
    static bit rd_applied, wr_applied, rb_applied, intg_applied, intg_ever;
    static bit wrote_data;          // the current lone command has written a data word
    // rram_disable as the controller path sees it.
    static bit disabled;
    static int unsigned cur_b;      // data byte address of the lone command
    // Requests and their kind.
    static int unsigned n_reads, n_writes, n_rewrites, n_outside, n_op_err, n_disabled_req;
    static int unsigned n_wr_busy_cycles, n_rd_gap, n_wr_stall;
    static int unsigned outside_allowed;  // the lone command is addressed past the OTP pages
    static bit [3:0] part_seen, num_bad;
    static bit [1:0] ops_seen;
    static string trace[$];  // the last controller operations, for a hang report

    static function void log(string op);
      trace.push_back($sformatf("%0t %s", $time, op));
      if (trace.size() > 12) void'(trace.pop_front());
    endfunction

    static function string last_ops();
      string s = "";
      foreach (trace[i]) s = {s, "\n    ", trace[i]};
      return s;
    endfunction

    static function bit [63:0] block(int unsigned k);
      bit [63:0] v;
      for (int i = 7; i >= 0; i--) v = {v[55:0], mem[IntgBytes + 8 * k + i]};
      return v;
    endfunction

    // The 16-byte RRAM word holding data byte b, and the one holding its syndrome, for reports.
    static function string words_of(int unsigned b);
      int unsigned d = (IntgBytes + b) & ~15, k = (b / 8) & ~15;
      string s = $sformatf("data word %06h:", IntgStartAddr + d);
      for (int i = 15; i >= 0; i--) s = {s, $sformatf("%02h", mem[d + i])};
      s = {s, $sformatf(" syndrome word %06h:", IntgStartAddr + k)};
      for (int i = 15; i >= 0; i--) s = {s, $sformatf("%02h", mem[k + i])};
      return s;
    endfunction

    static function bit [15:0] word16(int unsigned b);
      return {mem[IntgBytes + b + 1], mem[IntgBytes + b]};
    endfunction

    static function void start_fault(fault_e f);
      fault = f;
      fault_on = (f != FNone);
      rd_applied = 0;
      wr_applied = 0;
      rb_applied = 0;
      intg_applied = 0;
      wrote_data = 0;
    endfunction
  endclass

  typedef class rram_ctrl_otp_cmodel_scoreboard;

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_otp_cmodel_vseq extends rram_ctrl_otp_base_vseq;
    `uvm_object_utils(rram_ctrl_otp_cmodel_vseq)

    int unsigned num_cmds = 3000;
    bit unaligned_on;
    int fatal_kind = -1;       // -1: none (random test); 0 disable, 1 undefined command, 2 intg error
    virtual rram_ctrl_otp_ot_if ov;
    virtual rram_ctrl_otp_ct_if cv;
    int unsigned hot[16];      // blocks most commands go to
    bit stop_ct;

    function new(string name = "rram_ctrl_otp_cmodel_vseq");
      super.new(name);
    endfunction

    // ---- the controller path ----
    task ct_idle();
      cv.done <= 0;
      cv.err <= '0;
      cv.rvalid <= 0;
      cv.rdata <= '0;
    endtask

    function automatic bit in_region(bit [20:0] a);
      return a >= IntgStartAddr;  // to the top of the 21-bit address space
    endfunction

    task ct_thread();
      int unsigned busy = 0;
      cv.wready <= 0;
      cv.wr_busy <= 0;
      ct_idle();
      forever begin
        bit [1:0] op;
        bit [20:0] base;
        int unsigned nw;
        bit [3:0] err;
        bit outside, data_page, inj_rd, inj_wr, inj_rb, inj_intg;
        int unsigned rd_fail_at;
        @(posedge cv.clk);
        if (stop_ct) return;
        // rram_wr_busy: some cycles after a write.
        if (busy > 0) begin
          busy--;
          otp_bench::n_wr_busy_cycles++;
        end
        cv.wr_busy <= busy > 0;
        cv.wready <= 0;
        if (!(cv.req === 1'b1 && cv.start === 1'b1)) continue;
        // A new operation.
        op = cv.op;
        base = cv.addr;
        nw = cv.num + 1;
        otp_bench::ops_seen[op] = 1;
        otp_bench::part_seen[cv.part] = 1;
        err = '0;
        outside = !in_region(base) || cv.part !== 1'b0 || !in_region(base + 21'(4 * (nw - 1)));
        data_page = in_region(base) && base >= IntgStartAddr + 21'(IntgBytes);
        if (outside) begin
          otp_bench::n_outside++;
          if (!otp_bench::outside_allowed)
            `uvm_error("otp_ct", $sformatf("request outside the OTP pages: op %0d partition %0d address %06h, %0d words",
                                           op, cv.part, base, nw))
          err[2] = 1;  // mp_err (HwOtpDataCfg)
        end
        if (otp_bench::disabled) begin
          otp_bench::n_disabled_req++;
          err[2] = 1;  // rram_disable: blocked (mp_err)
        end
        inj_rd = otp_bench::fault_on && otp_bench::fault == FRdErr && !otp_bench::rd_applied;
        inj_wr = otp_bench::fault_on && otp_bench::fault == FWrErr && !otp_bench::wr_applied;
        // The readback of the command's own word: its 64-bit block is bus words 0-1 or 2-3.
        inj_rb = otp_bench::fault_on && otp_bench::fault == FReadback && otp_bench::wrote_data &&
                 !otp_bench::rb_applied && data_page &&
                 base == ((IntgStartAddr + 21'(IntgBytes + otp_bench::cur_b)) & ~21'hf);
        inj_intg = otp_bench::fault_on && otp_bench::fault == FIntg && !otp_bench::intg_applied;
        case (op)
          2'd0: begin  // Read: NUM + 1 bus words
            otp_bench::n_reads++;
            rd_fail_at = nw;
            if (err != 0) rd_fail_at = 0;
            else if (inj_rd) begin
              rd_fail_at = $urandom_range(0, nw - 1);
              err[1] = 1;
              otp_bench::rd_applied = 1;
            end
            for (int unsigned i = 0; i < nw; i++) begin
              bit [31:0] d;
              bit [38:0] w;
              bit [20:0] a = base + 21'(4 * i);
              // Random gaps.
              while ($urandom_range(0, 3) == 0) begin
                otp_bench::n_rd_gap++;
                cv.rvalid <= 0;  // before the edge: the last word is not offered twice
                @(posedge cv.clk);
              end
              if (i >= rd_fail_at) d = '1;
              else begin
                int unsigned off = int'(a - IntgStartAddr);
                d = {otp_bench::mem[off + 3], otp_bench::mem[off + 2], otp_bench::mem[off + 1],
                     otp_bench::mem[off]};
              end
              if (inj_rb && i == 2 * ((otp_bench::cur_b / 8) % 2) + (otp_bench::cur_b % 8) / 4) begin
                int unsigned bit_i = $urandom_range(0, 31);  // once: d[$urandom..] ^= 1 draws twice
                d[bit_i] = !d[bit_i];  // the RRAM word read back is not what was written
                otp_bench::rb_applied = 1;
              end
              w = bus_word(d);
              if (inj_intg && i == 0) begin
                int unsigned bit_j = 32 + $urandom_range(0, 6);
                w[bit_j] = !w[bit_j];  // bus integrity broken
                otp_bench::intg_applied = 1;
                otp_bench::intg_ever = 1;
              end
              cv.rvalid <= 1;
              cv.rdata <= w;
              // rram_ctrl_rd raises op_done with the last word (the plug takes done_i only with
              // rvalid_i).
              if (i == nw - 1) begin
                otp_bench::log($sformatf("op %0d addr %06h words %0d err %b%s", op, base, nw, err,
                                         inj_rb ? " readback corrupted" : ""));
                cv.done <= 1;
                cv.err <= err;
              end
              @(posedge cv.clk);
              if (cv.rready !== 1'b1) begin
                // rready low: hold the word until it is taken.
                while (cv.rready !== 1'b1) @(posedge cv.clk);
              end
            end
            ct_idle();
          end
          2'd1: begin  // Write: whole RRAM words only
            bit [31:0] wd[$];
            otp_bench::n_writes++;
            if (nw % 4 != 0) begin
              err[3] = 1;  // invalid_op_err (op_err): rejected without accessing the RRAM
              otp_bench::n_op_err++;
            end else begin
              if (err == 0 && inj_wr) begin
                err[0] = 1;
                otp_bench::wr_applied = 1;
              end
              // rram_ctrl_wr raises op_done as it takes the last word. On this path the plug drives
              // wvalid_o only while wready_i is high (it is the write FIFO's ready), so wready is
              // offered without waiting for wvalid.
              while (wd.size() < nw) begin
                bit rdy = $urandom_range(0, 4) != 0;
                bit last = rdy && wd.size() == nw - 1;
                cv.wready <= rdy;
                if (last) begin
                  otp_bench::log($sformatf("op %0d addr %06h words %0d err %b", op, base, nw, err));
                  cv.done <= 1;
                  cv.err <= err;
                end
                @(posedge cv.clk);
                if (rdy) begin
                  if (cv.wvalid !== 1'b1)
                    `uvm_error("otp_ct", $sformatf("write word %0d: no wvalid with wready", wd.size()))
                  if (cv.wdata !== bus_word(cv.wdata[31:0]))
                    `uvm_error("otp_ct", $sformatf("write word %0d with broken bus integrity: %010h", wd.size(), cv.wdata))
                  wd.push_back(cv.wdata[31:0]);
                end else otp_bench::n_wr_stall++;
              end
              cv.wready <= 0;
              ct_idle();
              if (err == 0) begin
                for (int unsigned i = 0; i < nw; i++) begin
                  int unsigned off = int'(base + 21'(4 * i) - IntgStartAddr);
                  for (int b = 0; b < 4; b++) otp_bench::mem[off + b] = wd[i][8 * b +: 8];
                end
                if (data_page && otp_bench::fault_on) otp_bench::wrote_data = 1;
                busy = $urandom_range(0, 3) == 0 ? 0 : $urandom_range(1, 24);
              end
            end
          end
          default: begin  // Rewrite (or reserved): the word stays as it is
            otp_bench::n_rewrites++;
            if (op != 2'd2) err[3] = 1;
          end
        endcase
        if (op > 2'd1 || (op == 2'd1 && nw % 4 != 0)) begin
          // done alone: a Rewrite, or an operation rejected without data.
          otp_bench::log($sformatf("op %0d addr %06h words %0d err %b", op, base, nw, err));
          cv.done <= 1;
          cv.err <= err;
          @(posedge cv.clk);
          ct_idle();
        end
        // One cycle before looking for the next operation: the plug sees done at this edge.
        @(posedge cv.clk);
      end
    endtask

    // ---- the otp_ctrl side ----
    typedef struct {
      bit [6:0] cmd;
      bit [1:0] size;
      bit [10:0] addr;
      bit [63:0] wdata;
      int unsigned lo, hi;   // data blocks
    } cmd_t;

    function automatic void blocks_of(bit [1:0] size, bit [10:0] addr, output int unsigned lo,
                                      output int unsigned hi);
      int unsigned b = int'(addr) * 2;
      lo = b / 8;
      hi = (b + 2 * (int'(size) + 1) + 7) / 8;
    endfunction

    function automatic bit [10:0] pick_addr(bit [1:0] size, bit aligned);
      int unsigned w;
      case ($urandom_range(0, 19))
        0: w = $urandom_range(1280, 2047);           // past the OTP pages
        1: w = $urandom_range(1024, 1279);           // past otp_ctrl's depth, in the pages
        2: w = $urandom_range(0, 7);                 // first blocks
        3, 4: w = $urandom_range(0, 1279);           // anywhere
        default: w = hot[$urandom_range(0, 15)] * 4 + $urandom_range(0, 3);
      endcase
      if (aligned) begin
        case (size)
          2'd3: w &= ~3;
          2'd2: w = (w & ~3) | $urandom_range(0, 1);
          2'd1: w &= ~1;
          default: ;
        endcase
      end else begin
        // Crossing a block: start late in one.
        w = (w & ~3) | 3;
      end
      return 11'(w);
    endfunction

    function automatic bit [63:0] pick_wdata(bit [6:0] c, bit [1:0] size, bit [10:0] addr);
      bit [63:0] old = 0, nw;
      int unsigned b = int'(addr) * 2;
      for (int i = 0; i <= int'(size); i++)
        if (b + 2 * i + 1 < DataBytes) old[16 * i +: 16] = otp_bench::word16(b + 2 * i);
      nw = {$urandom(), $urandom()};
      case ($urandom_range(0, 19))
        0, 1, 2: return nw;                  // anything: often clears a bit
        3: return old;                       // the same again
        4: return '1;
        default: return old | (nw & {$urandom(), $urandom()});  // only sets bits
      endcase
    endfunction

    task issue(cmd_t c);
      ov.valid <= 1;
      ov.cmd <= c.cmd;
      ov.size <= c.size;
      ov.addr <= c.addr;
      ov.wdata <= c.wdata;
      begin
        int unsigned t = 0;
        do begin
          @(posedge ov.clk);
          if (++t > MaxRspWait) `uvm_fatal("otp_ot", $sformatf("%s size %0d addr %03h not accepted after %0d otp cycles (%0d outstanding: %s)",
                                                                cmd_name(c.cmd), c.size, c.addr, MaxRspWait, sb.outstanding(), sb.pending()))
        end while (ov.ready !== 1'b1);
      end
      ov.valid <= 0;
      ov.cmd <= 7'($urandom());
      ov.size <= 2'($urandom());
      ov.addr <= 11'($urandom());
      ov.wdata <= {$urandom(), $urandom()};
    endtask

    rram_ctrl_otp_cmodel_scoreboard sb;

    task wait_outstanding(int unsigned n);
      int unsigned t = 0;
      while (sb.outstanding() > n) begin
        @(posedge ov.clk);
        if (++t > MaxRspWait)
          `uvm_fatal("otp_ot", $sformatf("%0d command(s) not answered after %0d otp cycles: %s; controller path: req %b start %b op %0d addr %06h wvalid %b, wr_busy %b; last operations:%s",
                                         sb.outstanding(), MaxRspWait, sb.pending(), cv.req, cv.start, cv.op, cv.addr,
                                         cv.wvalid, cv.wr_busy, otp_bench::last_ops()))
      end
    endtask

    task run_cmd(cmd_t c, fault_e f, bit alone);
      if (alone || f != FNone) wait_outstanding(0);
      else wait_outstanding(1);
      if (f == FCorruptData || f == FCorruptSyn) begin
        // A stored bit flipped (both the bench memory and the model: a persistent fault).
        int unsigned k = c.lo < 320 ? c.lo : 0;
        int unsigned off = f == FCorruptData ? IntgBytes + 8 * k + $urandom_range(0, 7) : k;
        bit [7:0] v = otp_bench::mem[off] ^ (8'h1 << $urandom_range(0, 7));
        otp_bench::mem[off] = v;
        sb.poke(off, v);
        sb.n_corrupt++;
      end
      otp_bench::outside_allowed = int'(c.addr) * 2 >= DataBytes ||
                                   int'(c.addr) * 2 + 2 * (int'(c.size) + 1) > DataBytes;
      if (otp_bench::outside_allowed) wait_outstanding(0);
      otp_bench::start_fault((f == FCorruptData || f == FCorruptSyn) ? FNone : f);
      otp_bench::cur_b = int'(c.addr) * 2;
      sb.push(c.cmd, c.size, c.addr, c.wdata, alone || f != FNone || otp_bench::outside_allowed, f);
      issue(c);
      if (alone || f != FNone || otp_bench::outside_allowed) begin
        wait_outstanding(0);
        otp_bench::start_fault(FNone);
        otp_bench::outside_allowed = 0;
      end
    endtask

    function automatic cmd_t random_cmd(bit allow_unaligned, ref bit alone);
      cmd_t c;
      // otp_ctrl aligns every access to its size; unaligned commands only with +unaligned=1 (the
      // documentation does not say what the plug does with them).
      bit aligned = !allow_unaligned || !unaligned_on || $urandom_range(0, 39) != 0;
      case ($urandom_range(0, 99)) inside
        [0:24]: c.cmd = CmdRead;
        [25:34]: c.cmd = CmdReadRaw;
        [35:59]: c.cmd = CmdWrite;
        [60:71]: c.cmd = CmdWriteRaw;
        [72:81]: c.cmd = CmdZeroize;
        default: c.cmd = $urandom_range(0, 1) ? CmdRead : CmdWrite;
      endcase
      // 16-, 32- or 64-bit words (theory_of_operation.md "OTP Hardware Plug": "a single OTP word,
      // 16, 32, or 64 bits wide"; size 2, three native words, is not among them).
      case ($urandom_range(0, 9))
        0, 1, 2, 3: c.size = 2'd3;
        4, 5, 6: c.size = 2'd1;
        default: c.size = 2'd0;
      endcase
      c.addr = pick_addr(c.size, aligned);
      if (!aligned && c.size == 0) c.size = 1;
      c.wdata = (c.cmd == CmdWrite || c.cmd == CmdWriteRaw) ? pick_wdata(c.cmd, c.size, c.addr)
                                                            : {$urandom(), $urandom()};
      blocks_of(c.size, c.addr, c.lo, c.hi);
      alone = !aligned;
      return c;
    endfunction

    // Two commands at once only on different blocks (the model's memory check and its resync
    // after an unpredicted command look at one command's block at a time).
    function bit clashes(cmd_t c);
      return sb.busy_block(c.lo) || sb.busy_block(c.hi - 1);
    endfunction

    task otp_thread();
      cmd_t c;
      bit alone;
      // otp_ctrl starts with Init (otp_ctrl theory_of_operation.md "Direct Access Interface Control").
      repeat ($urandom_range(0, 20)) @(posedge ov.clk);
      c = '{cmd: CmdInit, size: 0, addr: 0, wdata: 0, lo: 0, hi: 1};
      run_cmd(c, FNone, 1);
      for (int unsigned n = 0; n < num_cmds; n++) begin
        fault_e f = FNone;
        c = random_cmd(1, alone);
        if ($urandom_range(0, 99) < 14) begin
          case ($urandom_range(0, 9))
            0, 1: f = FRdErr;
            2, 3: f = FWrErr;
            4, 5: begin
              f = FReadback;
              if (!(c.cmd inside {CmdWrite, CmdWriteRaw, CmdZeroize})) c.cmd = CmdWrite;
              c.wdata = pick_wdata(c.cmd, c.size, c.addr);
            end
            6, 7: f = FCorruptData;
            default: f = FCorruptSyn;
          endcase
          if (f inside {FCorruptData, FCorruptSyn} && $urandom_range(0, 1))
            c.cmd = $urandom_range(0, 1) ? CmdRead : CmdReadRaw;
        end
        if (!alone && f == FNone && clashes(c)) wait_outstanding(0);
        run_cmd(c, f, alone);
        if ($urandom_range(0, 15) == 0) repeat ($urandom_range(1, 60)) @(posedge ov.clk);
        if ($urandom_range(0, 63) == 0) begin
          int unsigned k = $urandom_range(0, 15);
          hot[k] = $urandom_range(0, 319);
        end
      end
      wait_outstanding(0);
      if (fatal_kind >= 0) fatal_phase();
    endtask

    // One fatal event at the end, then only that commands are still answered.
    task fatal_phase();
      cmd_t c;
      bit alone;
      sb.fatal_kind = fatal_kind;
      sb.fatal_started = 1;
      case (fatal_kind)
        0: begin
          `uvm_info("otp_ot", "fatal: RRAM access disabled", UVM_LOW)
          @(posedge cv.clk);
          cv.rram_dis <= MuBi4True;
          otp_bench::disabled = 1;
          sb.disable_at_ot_cycle = sb.ot_cycles;
        end
        1: begin
          `uvm_info("otp_ot", "fatal: an undefined command", UVM_LOW)
          c = random_cmd(0, alone);
          do c.cmd = 7'($urandom()); while (c.cmd inside {CmdRead, CmdWrite, CmdReadRaw, CmdWriteRaw,
                                                           CmdInit, CmdZeroize});
          sb.fatal_cmd = 1;
          run_cmd(c, FNone, 1);
        end
        default: begin
          `uvm_info("otp_ot", "fatal: a bus-integrity error on a read word", UVM_LOW)
          c = random_cmd(0, alone);
          c.cmd = $urandom_range(0, 1) ? CmdRead : CmdReadRaw;
          c.addr = pick_addr(c.size, 1);
          if (int'(c.addr) * 2 >= DataBytes) c.addr = 0;
          blocks_of(c.size, c.addr, c.lo, c.hi);
          sb.fatal_intg = 1;
          run_cmd(c, FIntg, 1);
        end
      endcase
      sb.after_fatal = 1;
      for (int unsigned n = 0; n < 40; n++) begin
        c = random_cmd(0, alone);
        run_cmd(c, FNone, 1);
      end
      wait_outstanding(0);
    endtask

    virtual task body();
      ov = cfg.ot_cfg.vif;
      cv = cfg.ct_cfg.vif;
      void'(uvm_config_db#(int unsigned)::get(null, "", "num_cmds", num_cmds));
      void'(uvm_config_db#(int)::get(null, "", "fatal_kind", fatal_kind));
      void'($value$plusargs("num_cmds=%d", num_cmds));
      void'($value$plusargs("fatal=%d", fatal_kind));
      void'($value$plusargs("unaligned=%d", unaligned_on));
      if (!$cast(sb, uvm_root::get().find("uvm_test_top.env.scoreboard")))
        `uvm_fatal("otp_vseq", "scoreboard is not rram_ctrl_otp_cmodel_scoreboard")
      foreach (hot[i]) hot[i] = $urandom_range(0, 319);
      // The bench memory: erased (all zero) or all ones per block with its syndrome, some blocks
      // programmed with a consistent syndrome, as an OTP image would be.
      for (int unsigned k = 0; k < DataBytes / 8; k++) begin
        bit [63:0] d;
        case ($urandom_range(0, 5))
          0: d = '1;
          1, 2: d = {$urandom(), $urandom()};
          default: d = '0;
        endcase
        for (int i = 0; i < 8; i++) otp_bench::mem[IntgBytes + 8 * k + i] = d[8 * i +: 8];
        otp_bench::mem[k] = 8'(eg2_rram_otp_hamming(d));
      end
      for (int unsigned k = DataBytes / 8; k < IntgBytes; k++) otp_bench::mem[k] = 8'($urandom());
      foreach (otp_bench::mem[i]) sb.poke(i, otp_bench::mem[i]);
      // Inputs defined while in reset.
      ov.valid <= 0;
      cv.init_done <= 0;
      cv.rram_dis <= MuBi4False;
      cv.wr_busy <= 0;
      cv.wready <= 0;
      ct_idle();
      super.body();
      wait (ov.rst_n === 1'b1);
      fork
        ct_thread();
        begin
          // rram_init_done some time after reset.
          repeat ($urandom_range(0, 400)) @(posedge cv.clk);
          cv.init_done <= 1;
        end
      join_none
      otp_thread();
      repeat (20) @(posedge ov.clk);
      stop_ct = 1;
      // After a fatal event the OTP pages are no longer predicted.
      if (fatal_kind < 0) sb.final_compare();
    endtask
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_otp_cmodel_scoreboard extends rram_ctrl_otp_scoreboard;
    `uvm_component_utils(rram_ctrl_otp_cmodel_scoreboard)

    chandle h;
    bit inject_fail, injected;
    typedef struct {
      bit [6:0] cmd;
      bit [1:0] size;
      bit [10:0] addr;
      bit [63:0] wdata;
      bit alone;
      bit accepted;
      int unsigned accepted_at;
      bit disabled;
      int unsigned lo, hi;
      fault_e fk;
      bit [63:0] old;   // the words before the command (bench memory)
    } exp_t;
    exp_t q[$];
    int unsigned ot_cycles, disable_at_ot_cycle = '1;
    int fatal_kind = -1;
    bit fatal_cmd, fatal_intg, after_fatal;
    bit intg_err_seen, fatal_err_seen, fatal_started, fatal_err_seen_reported, intg_reported;
    int unsigned n_rsp, n_err_checked, n_rdata_checked, n_unpred, n_any_err, n_resync, n_err,
                 n_checks, n_mem_cmp, n_corrupt, n_blank, n_ecc, n_macro, n_after_fatal;
    int unsigned n_cmd[string];
    bit strict, rsp_log;                     // +strict=1: known findings (tickets) fail the test
    int unsigned n_known_prec, n_rsw_more_bits_ok, n_oor_intg_write, n_known_neigh;
    bit ready_low_long;
    int unsigned ready_low;

    // Coverage.
    int c_cmd, c_size, c_err, c_where, c_fault, c_alone, c_lat, c_aligned;
    covergroup cg_rsp;
      cp_cmd: coverpoint c_cmd { bins read = {0}; bins write = {1}; bins read_raw = {2};
                                 bins write_raw = {3}; bins init = {4}; bins zeroize = {5};
                                 bins undefined = {6}; }
      cp_size: coverpoint c_size { bins s[] = {[0:3]}; }
      cp_err: coverpoint c_err { bins no_error = {0}; bins macro = {1}; bins ecc_uncorr = {3};
                                 bins write_blank = {4}; illegal_bins corr = {2};
                                 bins other = {[5:7]}; }
      cp_where: coverpoint c_where { bins first_block = {0}; bins hot = {1}; bins anywhere = {2};
                                     bins past_depth = {3}; bins past_pages = {4}; }
      cp_fault: coverpoint c_fault { bins none = {FNone}; bins rd_err = {FRdErr}; bins wr_err = {FWrErr};
                                     bins readback = {FReadback}; bins corrupt_data = {FCorruptData};
                                     bins corrupt_syn = {FCorruptSyn}; bins intg = {FIntg}; }
      cp_alone: coverpoint c_alone { bins two_at_once = {0}; bins alone = {1}; }
      cp_aligned: coverpoint c_aligned { bins aligned = {1}; bins crossing = {0}; }
      cp_lat: coverpoint c_lat { bins l_short = {[0:15]}; bins l_mid = {[16:40]}; bins l_long = {[41:$]}; }
      x_cmd_size: cross cp_cmd, cp_size {
        ignore_bins init = binsof(cp_cmd.init) || binsof(cp_cmd.undefined);
      }
      x_cmd_err: cross cp_cmd, cp_err {
        ignore_bins no = binsof(cp_cmd.init) || binsof(cp_cmd.undefined) || binsof(cp_err.other);
        ignore_bins blank_read = (binsof(cp_cmd.read) || binsof(cp_cmd.read_raw) || binsof(cp_cmd.zeroize))
                                 && binsof(cp_err.write_blank);
        ignore_bins ecc_raw = binsof(cp_cmd.read_raw) && binsof(cp_err.ecc_uncorr);
      }
      x_cmd_where: cross cp_cmd, cp_where {
        ignore_bins no = binsof(cp_cmd.init) || binsof(cp_cmd.undefined);
      }
      x_cmd_alone: cross cp_cmd, cp_alone { ignore_bins no = binsof(cp_cmd.undefined); }
    endgroup

    function new(string name, uvm_component parent);
      super.new(name, parent);
      cg_rsp = new();
      cg_rsp.option.per_instance = 1;
    endfunction

    virtual function void build_phase(uvm_phase phase);
      super.build_phase(phase);
      void'(uvm_config_db#(bit)::get(this, "", "inject_fail", inject_fail));
      void'($value$plusargs("strict=%d", strict));
      rsp_log = $test$plusargs("otp_rsp_log");
      h = eg2_rram_otp_create();
    endfunction

    function void poke(int unsigned off, bit [7:0] v);
      eg2_rram_otp_poke(h, off, v);
    endfunction

    function int unsigned outstanding();
      int unsigned n = 0;
      foreach (q[i]) n++;
      return n;
    endfunction

    function string pending();
      string s = "";
      foreach (q[i]) s = {s, $sformatf("[%s size %0d addr %03h wdata %016h%s fault %s] ", cmd_name(q[i].cmd), q[i].size,
                                       q[i].addr, q[i].wdata, q[i].accepted ? "" : " not accepted", q[i].fk.name())};
      return s;
    endfunction

    function bit busy_block(int unsigned k);
      foreach (q[i]) if (k >= q[i].lo && k < q[i].hi) return 1;
      return 0;
    endfunction

    function void push(bit [6:0] cmd, bit [1:0] size, bit [10:0] addr, bit [63:0] wdata, bit alone,
                       fault_e fk);
      exp_t e;
      e.fk = fk;
      for (int i = 0; i <= int'(size); i++)
        if (int'(addr) * 2 + 2 * i + 1 < DataBytes) e.old[16 * i +: 16] = otp_bench::word16(int'(addr) * 2 + 2 * i);
      e.cmd = cmd;
      e.size = size;
      e.addr = addr;
      e.wdata = wdata;
      e.alone = alone;
      e.accepted = 0;
      e.disabled = otp_bench::disabled;
      e.lo = int'(addr) * 2 / 8;
      e.hi = (int'(addr) * 2 + 2 * (int'(size) + 1) + 7) / 8;
      q.push_back(e);
    endfunction

    function void expect_ok(bit ok, string what);
      n_checks++;
      if (!ok) begin
        n_err++;
        `uvm_error(get_type_name(), what)
      end
    endfunction

    // The model's block k and its syndrome against the bench memory.
    function bit cmp_block(int unsigned k, string why);
      bit ok = 1;
      if (k >= DataBytes / 8) return 1;
      for (int i = 0; i < 8; i++)
        if (eg2_rram_otp_peek(h, IntgBytes + 8 * k + i) != otp_bench::mem[IntgBytes + 8 * k + i]) ok = 0;
      if (eg2_rram_otp_peek(h, k) != otp_bench::mem[k]) ok = 0;
      n_mem_cmp++;
      expect_ok(ok, $sformatf("%s: block %0d is %016h syndrome %02h, the model's %016h syndrome %02h", why, k,
                              otp_bench::block(k), otp_bench::mem[k], model_block(k), eg2_rram_otp_peek(h, k)));
      return ok;
    endfunction

    function bit [63:0] model_block(int unsigned k);
      bit [63:0] v;
      for (int i = 7; i >= 0; i--) v = {v[55:0], 8'(eg2_rram_otp_peek(h, IntgBytes + 8 * k + i))};
      return v;
    endfunction

    function void resync_block(int unsigned k);
      if (k >= DataBytes / 8) return;
      for (int i = 0; i < 8; i++) poke(IntgBytes + 8 * k + i, otp_bench::mem[IntgBytes + 8 * k + i]);
      poke(k, otp_bench::mem[k]);
      n_resync++;
    endfunction

    function void neighbours(exp_t e, int unsigned lo, int unsigned hi, bit rd_fault);
      int unsigned ks[$];
      foreach (q[i]) return;  // only with nothing else outstanding
      for (int unsigned k = lo; k < hi && k < IntgBytes; k++) begin
        int unsigned nb = k ^ 1;
        if ((nb < lo || nb >= hi) && nb < DataBytes / 8) begin
          bit same = 1;
          for (int i = 0; i < 8; i++)
            if (eg2_rram_otp_peek(h, IntgBytes + 8 * nb + i) != otp_bench::mem[IntgBytes + 8 * nb + i]) same = 0;
          if (!same) report_neighbour(e, rd_fault, $sformatf("block %0d (the other half of its RRAM word) is %016h, the model's %016h",
                                                           nb, otp_bench::block(nb), model_block(nb)), nb, 1);
        end
        for (int unsigned j = k & ~15; j < (k & ~15) + 16; j++)
          if ((j < lo || j >= hi) && eg2_rram_otp_peek(h, j) != otp_bench::mem[j])
            report_neighbour(e, rd_fault, $sformatf("syndrome %0d (same integrity-page word) is %02h, the model's %02h",
                                                    j, otp_bench::mem[j], eg2_rram_otp_peek(h, j)), j, 0);
      end
    endfunction

    function void report_neighbour(exp_t e, bit rd_fault, string what, int unsigned idx, bit is_block);
      if (rd_fault && !strict) n_known_neigh++;
      else expect_ok(0, $sformatf("after %s size %0d addr %03h (fault %s): %s", cmd_name(e.cmd), e.size, e.addr,
                                  e.fk.name(), what));
      if (rd_fault && !strict && n_known_neigh <= 3)
        `uvm_info(get_type_name(), $sformatf("known finding: after %s size %0d addr %03h with an RRAM read error: %s",
                                             cmd_name(e.cmd), e.size, e.addr, what), UVM_LOW)
      // Take the bench memory, so the rest of the run is checked from there.
      if (is_block) resync_block(idx);
      else begin
        poke(idx, otp_bench::mem[idx]);
        n_resync++;
      end
    endfunction

    function void final_compare();
      int unsigned bad = 0;
      foreach (otp_bench::mem[i])
        if (eg2_rram_otp_peek(h, i) != otp_bench::mem[i]) begin
          if (bad < 8) `uvm_error(get_type_name(), $sformatf("OTP region offset %0d is %02h, the model's %02h", i,
                                                             otp_bench::mem[i], eg2_rram_otp_peek(h, i)))
          bad++;
        end
      n_mem_cmp++;
      expect_ok(bad == 0, $sformatf("%0d bytes of the OTP region differ from the model", bad));
    endfunction

    // otp side, every clk_otp_i cycle.
    virtual function void predict_ot(rram_ctrl_otp_ot_item item);
      ot_cycles++;
      // Acceptance: valid and ready in the same cycle.
      if (item.valid === 1'b1 && item.ready === 1'b1) begin
        int i = -1;
        foreach (q[j]) if (!q[j].accepted) begin i = j; break; end
        if (i < 0) begin
          // A stray acceptance would be the stimulus's fault (it always pushes first).
          `uvm_error(get_type_name(), "a command accepted that the stimulus did not issue")
        end else begin
          q[i].accepted = 1;
          q[i].accepted_at = ot_cycles;
          q[i].disabled = disable_at_ot_cycle != '1;
        end
      end
      if (item.ready !== 1'b1 && q.size() > 0 && !q[0].accepted) begin
        if (++ready_low > MaxRspWait && !ready_low_long) begin
          ready_low_long = 1;
          `uvm_error(get_type_name(), $sformatf("ready low for %0d otp cycles with a command waiting", MaxRspWait))
        end
      end else ready_low = 0;
      if (item.rvalid === 1'b1) response(item);
    endfunction

    function int cmd_index(bit [6:0] c);
      case (c)
        CmdRead: return 0;
        CmdWrite: return 1;
        CmdReadRaw: return 2;
        CmdWriteRaw: return 3;
        CmdInit: return 4;
        CmdZeroize: return 5;
        default: return 6;
      endcase
    endfunction

    function void response(rram_ctrl_otp_ot_item item);
      exp_t e;
      int ex;
      bit [63:0] rdata, rmask;
      int mem_pred;
      int unsigned lo, hi;
      bit rd_f, wr_f, rb_f;
      fault_e f;
      n_rsp++;
      if (q.size() == 0 || !q[0].accepted) begin
        expect_ok(0, "a response without a command");
        return;
      end
      e = q.pop_front();
      f = e.fk;
      rd_f = e.alone && otp_bench::rd_applied;
      wr_f = e.alone && otp_bench::wr_applied;
      rb_f = e.alone && otp_bench::rb_applied;
      n_cmd[cmd_name(e.cmd)]++;
      if (after_fatal || (fatal_kind == 0 && e.disabled) || fatal_cmd || fatal_intg) begin
        // After the fatal event: what is documented is that a command is still answered, and with
        // an error once RRAM access is disabled; the region is no longer compared.
        n_after_fatal++;
        if (fatal_kind == 0 && e.disabled && e.accepted_at > disable_at_ot_cycle + 100) begin
          n_err_checked++;
          expect_ok(item.err != 0, $sformatf("%s after RRAM access was disabled answered %0d, not an error",
                                             cmd_name(e.cmd), item.err));
        end
        if (fatal_cmd && !after_fatal) begin
          n_err_checked++;
          expect_ok(item.err != 0, $sformatf("undefined command %02h answered %0d, not an error", e.cmd, item.err));
        end
        fatal_cmd = 0;
        if (fatal_intg && !after_fatal) fatal_intg = 0;
        return;
      end
      ex = eg2_rram_otp_cmd(h, e.cmd, e.size, e.addr, e.wdata, rd_f, wr_f, rb_f, 0, rdata, rmask,
                            mem_pred, lo, hi);
      if (inject_fail && !injected && ex == 0 && rmask != 0) begin
        rdata[0] = !rdata[0];
        injected = 1;
        `uvm_info(get_type_name(), "inject_fail: one bit of a predicted read data flipped", UVM_LOW)
      end
      if (rsp_log)
        $display("%0t otp_rsp %s size %0d addr %03h wdata %016h fault %s: err %0d rdata %016h, model %0d mem_pred %0d blocks %0d-%0d",
                 $time, cmd_name(e.cmd), e.size, e.addr, e.wdata, f.name(), item.err, item.rdata, ex, mem_pred, lo, hi);
      if (ex == -1) n_unpred++;
      else if (ex == -2) begin
        n_any_err++;
        expect_ok(item.err != 0, $sformatf("%s: answered %0d, expected an error", cmd_name(e.cmd), item.err));
      end else begin
        n_err_checked++;
        // Known finding (status/rtl_tickets.md, "rram_ctrl_otp: an RRAM error is reported as
        // MacroWriteBlankError or MacroEccUncorrError"): the documented MacroError of an RRAM read,
        // write or memory-protection error is overwritten by the later blank or integrity check.
        if (!strict && ex == 1 && item.err inside {3, 4} &&
            (rd_f || wr_f || int'(e.addr) * 2 >= DataBytes)) begin
          n_known_prec++;
          ex = item.err;
        end
        if (e.cmd inside {CmdWrite, CmdWriteRaw} && ex == 0 && item.err == 0 && e.old != 0)
          n_rsw_more_bits_ok++;
        expect_ok(item.err == ex, $sformatf("%s size %0d addr %03h wdata %016h%s: error %0d, the model's %0d [%s]",
                                            cmd_name(e.cmd), e.size, e.addr, e.wdata,
                                            f == FNone ? "" : $sformatf(" (fault %s applied rd %0d wr %0d readback %0d)",
                                                                        f.name(), rd_f, wr_f, rb_f),
                                            item.err, ex, {otp_bench::words_of(int'(e.addr) * 2),
                                                           f == FNone ? "" : otp_bench::last_ops()}));
        if (rmask != 0) begin
          n_rdata_checked++;
          expect_ok((item.rdata & rmask) == (rdata & rmask),
                    $sformatf("%s size %0d addr %03h: read data %016h, the model's %016h (mask %016h) [%s]",
                              cmd_name(e.cmd), e.size, e.addr, item.rdata, rdata, rmask,
                              otp_bench::words_of(int'(e.addr) * 2)));
        end
        if (ex == 4) n_blank++;
        if (ex == 3) n_ecc++;
        if (ex == 1) n_macro++;
      end
      // A command past the OTP pages: its syndrome byte lies in the unused part of the integrity
      // page (blocks 320 and up). The documentation does not say whether the plug touches it after
      // the data access was rejected (it writes it: status/rtl_tickets.md, the error-precedence
      // ticket); not predicted, copied from the bench memory.
      if (int'(e.addr) * 2 >= DataBytes && e.cmd inside {CmdWrite, CmdZeroize}) begin
        for (int unsigned k = lo; k < hi && k < IntgBytes; k++)
          if (eg2_rram_otp_peek(h, k) != otp_bench::mem[k]) begin
            poke(k, otp_bench::mem[k]);
            n_resync++;
            n_oor_intg_write++;
          end
      end
      // The command's blocks.
      for (int unsigned k = lo; k < hi; k++) begin
        if (!mem_pred) resync_block(k);
        else void'(cmp_block(k, $sformatf("after %s size %0d addr %03h", cmd_name(e.cmd), e.size, e.addr)));
      end
      // What the command did not address must not change: the other 64-bit block of its RRAM word
      // and the other syndromes of its integrity-page word ("each operate on a single OTP word").
      // Known finding (status/rtl_tickets.md, "rram_ctrl_otp writes back the whole RRAM word after
      // an RRAM read error"): after a read error the plug writes the controller's all-ones fill back
      // over them.
      if (q.size() == 0) neighbours(e, lo, hi, rd_f || int'(e.addr) * 2 >= DataBytes);
      if (n_rsp % 64 == 0 && q.size() == 0) final_compare();
      // Coverage.
      c_cmd = cmd_index(e.cmd);
      c_size = e.size;
      c_err = item.err;
      c_fault = f;
      c_alone = e.alone;
      c_aligned = (int'(e.addr) * 2 % 8) + 2 * (int'(e.size) + 1) <= 8;
      c_lat = ot_cycles - e.accepted_at;
      begin
        int unsigned b = int'(e.addr) * 2;
        c_where = b >= DataBytes ? 4 : (int'(e.addr) >= 1032 ? 3 : (b < 8 ? 0 : 2));
      end
      if (cfg.en_cov) cg_rsp.sample();
    endfunction

    // controller side, every clk_i cycle: fatal outputs.
    // Before the fatal event (in the random test: always) both fatal outputs stay low; with
    // +fatal=2, intg_err must come after the broken read word.
    virtual function void predict_ct(rram_ctrl_otp_ct_item item);
      if (item.intg_err === 1'b1) begin
        if (otp_bench::intg_ever) intg_err_seen = 1;
        else if (!intg_reported) begin
          intg_reported = 1;
          expect_ok(0, "intg_err raised without a bus-integrity fault");
        end
      end
      if (item.fatal_err === 1'b1) begin
        fatal_err_seen = 1;
        if (!fatal_started && !fatal_err_seen_reported) begin
          fatal_err_seen_reported = 1;
          expect_ok(0, "fatal_err raised before any fatal event");
        end
      end
    endfunction

    virtual function void check_phase(uvm_phase phase);
      super.check_phase(phase);
      expect_ok(q.size() == 0, $sformatf("%0d command(s) never answered", q.size()));
      if (fatal_kind == 2) expect_ok(intg_err_seen, "a bus-integrity error on a read word did not raise intg_err");
      if (fatal_kind < 0) begin
        if (n_err_checked < 200 || n_rdata_checked < 50 || n_blank < 3 || n_ecc < 3 || n_macro < 3)
          `uvm_error(get_type_name(), $sformatf("too little checked: %0d responses, %0d read data, %0d blank, %0d ECC, %0d MacroError",
                                                n_err_checked, n_rdata_checked, n_blank, n_ecc, n_macro))
      end
      if (inject_fail && !injected) `uvm_error(get_type_name(), "inject_fail: nothing was flipped")
    endfunction

    virtual function void report_phase(uvm_phase phase);
      string s = "";
      super.report_phase(phase);
      foreach (n_cmd[k]) s = {s, $sformatf(" %s %0d", k, n_cmd[k])};
      `uvm_info(get_type_name(), $sformatf({"checked against the model: %0d responses (%0d errors, %0d read data; ",
          "%0d MacroWriteBlankError, %0d MacroEccUncorrError, %0d MacroError), %0d any-error, %0d not ",
          "predicted, %0d block resyncs, %0d memory compares, %0d stored bits corrupted, %0d after the ",
          "fatal event; commands:%s; controller path: %0d reads, %0d writes, %0d rewrites, %0d outside ",
          "the OTP pages, %0d op_err, %0d while disabled, %0d wr_busy cycles; %0d checks, %0d failed; ",
          "known findings: %0d RRAM errors answered with the blank or integrity code instead of MacroError%s; ",
          "%0d writes to an already programmed word that only set bits answered NoError; %0d syndrome bytes ",
          "written by commands past the OTP pages; %0d unaddressed blocks or syndromes overwritten after an RRAM ",
          "read error"},
          n_rsp, n_err_checked, n_rdata_checked, n_blank, n_ecc, n_macro, n_any_err, n_unpred, n_resync,
          n_mem_cmp, n_corrupt, n_after_fatal, s, otp_bench::n_reads, otp_bench::n_writes,
          otp_bench::n_rewrites, otp_bench::n_outside, otp_bench::n_op_err, otp_bench::n_disabled_req,
          otp_bench::n_wr_busy_cycles, n_checks, n_err, n_known_prec, strict ? " (strict: counted as failures)" : "",
          n_rsw_more_bits_ok, n_oor_intg_write, n_known_neigh), UVM_LOW)
      // The functional coverage of this run, per item (Xcelium collects only the DUT's coverage,
      // -covdut, so the bench's covergroups are reported here).
      if (cfg.en_cov)
        `uvm_info(get_type_name(), $sformatf({"functional coverage (instance): cg_rsp %0.2f%%: cmd %0.1f%%, ",
            "size %0.1f%%, err %0.1f%%, where %0.1f%%, fault %0.1f%%, alone %0.1f%%, aligned %0.1f%%, ",
            "latency %0.1f%%, cmd x size %0.1f%%, cmd x err %0.1f%%, cmd x where %0.1f%%, cmd x alone %0.1f%%"},
            cg_rsp.get_inst_coverage(), cg_rsp.cp_cmd.get_inst_coverage(), cg_rsp.cp_size.get_inst_coverage(),
            cg_rsp.cp_err.get_inst_coverage(), cg_rsp.cp_where.get_inst_coverage(),
            cg_rsp.cp_fault.get_inst_coverage(), cg_rsp.cp_alone.get_inst_coverage(),
            cg_rsp.cp_aligned.get_inst_coverage(), cg_rsp.cp_lat.get_inst_coverage(),
            cg_rsp.x_cmd_size.get_inst_coverage(), cg_rsp.x_cmd_err.get_inst_coverage(),
            cg_rsp.x_cmd_where.get_inst_coverage(), cg_rsp.x_cmd_alone.get_inst_coverage()), UVM_LOW)
      if (h != null) eg2_rram_otp_release(h);
    endfunction
  endclass

  // ---------------------------------------------------------------------------------------------
  class rram_ctrl_otp_cmodel_random_test extends rram_ctrl_otp_base_test;
    `uvm_component_utils(rram_ctrl_otp_cmodel_random_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      rram_ctrl_otp_scoreboard::type_id::set_type_override(rram_ctrl_otp_cmodel_scoreboard::get_type());
      test_seq = "rram_ctrl_otp_cmodel_vseq";
      timeout_ns = 500_000_000;
      super.build_phase(phase);
    endfunction
  endclass

  // Must fail: the scoreboard flips one bit of the first predicted read data.
  class rram_ctrl_otp_cmodel_random_inject_fail_test extends rram_ctrl_otp_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_otp_cmodel_random_inject_fail_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      uvm_config_db#(bit)::set(this, "env.scoreboard", "inject_fail", 1'b1);
      super.build_phase(phase);
    endfunction
  endclass

  // A shorter random run that ends with a fatal event (+fatal=0|1|2, random if not given).
  class rram_ctrl_otp_cmodel_fatal_test extends rram_ctrl_otp_cmodel_random_test;
    `uvm_component_utils(rram_ctrl_otp_cmodel_fatal_test)
    function new(string name, uvm_component parent);
      super.new(name, parent);
    endfunction
    virtual function void build_phase(uvm_phase phase);
      int f;
      if (!$value$plusargs("fatal=%d", f)) f = $urandom_range(0, 2);
      uvm_config_db#(int)::set(null, "", "fatal_kind", f);
      uvm_config_db#(int unsigned)::set(null, "", "num_cmds", 600);
      super.build_phase(phase);
    endfunction
  endclass
endpackage
