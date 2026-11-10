// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Block-level testbench for the OpenTitan CHERIoT memory subsystem (hw/ip/cheriot_mem_sys,
// top module `cheriot_mem_sys`): constants, transaction type, the TB's own storage, and the reference
// model / scoreboard.
//
// The reference model is written from the design documents
// (hw/ip/cheriot_mem_sys/doc/theory_of_operation.md, registers.md, programmers_guide.md),
// cheriot_mem_sys.hjson and the CHERIoT ISA's bounds encoding, not from the RTL: nothing here reads a
// DUT-internal signal, calls an RTL package
// function (ibex_cheriot_pkg is not imported), or copies state back from the DUT. The only DUT-made
// state it looks at is the content of the TB's own meta SRAM model, compared against its prediction.
// tlul_pkg is used for the bus types and the integrity encoding only.

package cms_pkg;

  import tlul_pkg::*;

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Address map (cheriot_mem_sys.sv parameters the testbench instantiates the DUT with)
  ////////////////////////////////////////////////////////////////////////////////////////////////

`ifdef CMS_EARLGREY_MAP
  // The DUT's defaults: Earl Grey's 192 KiB SRAM and 2 MiB NVM. Sweeps of the whole SRAM are slow.
  parameter logic [31:0] MainSramBase = 32'h1000_0000;
  parameter logic [31:0] MainSramTop  = 32'h1003_0000;
  parameter logic [31:0] NvmBase      = 32'h3000_0000;
  parameter logic [31:0] NvmTop       = 32'h3020_0000;
`else
  // Small map (default): 16 KiB SRAM, 8 KiB NVM, so a whole-SRAM sweep and a full tag-map compare
  // are cheap. Every size is a multiple of the 256 bytes one 32-bit metadata word covers.
  parameter logic [31:0] MainSramBase = 32'h1000_0000;
  parameter logic [31:0] MainSramTop  = 32'h1000_4000;
  parameter logic [31:0] NvmBase      = 32'h3000_0000;
  parameter logic [31:0] NvmTop       = 32'h3000_2000;
`endif
  parameter logic [31:0] MetaBase     = 32'h1100_0000;

  parameter int unsigned SramBytes    = MainSramTop - MainSramBase;
  parameter int unsigned NvmBytes     = NvmTop - NvmBase;
  // One metadata bit per 8 bytes, 32 bits per word: one 32-bit word per 256 bytes.
  parameter int unsigned RevbmBytes   = SramBytes / 64;
  parameter int unsigned NvmTagBytes  = NvmBytes / 64;
  parameter int unsigned SramTagBytes = SramBytes / 64;

  // Meta SRAM map, theory_of_operation.md "Meta SRAM Address Map": bitmap, NVM tags, SRAM tags.
  parameter logic [31:0] MetaRevbmBase   = MetaBase;
  parameter logic [31:0] MetaNvmTagBase  = MetaRevbmBase + RevbmBytes;
  parameter logic [31:0] MetaSramTagBase = MetaNvmTagBase + NvmTagBytes;
  parameter logic [31:0] MetaTop         = MetaSramTagBase + SramTagBytes;

  parameter int unsigned SramCaps    = SramBytes / 8;
  parameter int unsigned NvmCaps     = NvmBytes / 8;
  parameter int unsigned NumGranules = SramCaps + NvmCaps;

  // Untagged memory behind cored_tl_h (a peripheral window); the top of it answers d_error.
  parameter logic [31:0] UntaggedBase = 32'h2000_0000;
  parameter logic [31:0] UntaggedTop  = 32'h2000_1000;
  parameter logic [31:0] DataErrBase  = 32'h2000_0f00;

  // CSR offsets (registers.md; cross-checked against cheriot_mem_sys_reg_pkg in cms_tb)
  parameter logic [5:0] CsrIntrState  = 6'h00;
  parameter logic [5:0] CsrIntrEnable = 6'h04;
  parameter logic [5:0] CsrIntrTest   = 6'h08;
  parameter logic [5:0] CsrAlertTest  = 6'h0c;
  parameter logic [5:0] CsrRegwen     = 6'h10;
  parameter logic [5:0] CsrTbreBase   = 6'h14;
  parameter logic [5:0] CsrTbreNum    = 6'h18;
  parameter logic [5:0] CsrTbreStart  = 6'h1c;
  parameter logic [5:0] CsrTbreStatus = 6'h20;
  parameter logic [5:0] CsrTbreEpoch  = 6'h24;
  // TBRE_STATUS fields
  parameter int unsigned StatusBusy     = 0;
  parameter int unsigned StatusStartErr = 8;
  parameter int unsigned StatusSweepErr = 9;
  // ALERT_TEST.regwen (rw0c, reset 1): writing 0 to it disables alert testing until reset
  parameter int unsigned AlertTestRegwen = 31;

  function automatic bit csr_mapped(logic [5:0] off);
    return off inside {CsrIntrState, CsrIntrEnable, CsrIntrTest, CsrAlertTest, CsrRegwen,
                       CsrTbreBase, CsrTbreNum, CsrTbreStart, CsrTbreStatus, CsrTbreEpoch};
  endfunction

  // reggen refuses a write that leaves out a byte holding one of the register's fields (counted
  // up to its highest field bit): byte 0 for the one-bit registers, bytes 0-1 for TBRE_STATUS
  // (bits 9:8), all four for TBRE_BASE_ADDR, TBRE_NUM_CAPS, TBRE_EPOCH and ALERT_TEST (regwen in
  // bit 31).
  function automatic logic [3:0] csr_write_bytes(logic [5:0] off);
    case (off)
      CsrTbreBase, CsrTbreNum, CsrTbreEpoch, CsrAlertTest: return 4'b1111;
      CsrTbreStatus:                         return 4'b0011;
      default:                               return 4'b0001;
    endcase
  endfunction

  // The core port's a_source values. Three socket levels inside the subsystem take source bits
  // (tag mux, meta mux); keep them small.
  parameter int unsigned CoreSrcIds = 4;

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Global error count and reporting
  ////////////////////////////////////////////////////////////////////////////////////////////////

  int unsigned cms_err_count = 0;
  int unsigned cms_warn_count = 0;
  longint unsigned cms_cycle = 0;   // advanced by cms_tb

  // A test that has upset the DUT on purpose (cms_lockstep_glitch) sets cms_err_masked while the
  // DUT's behaviour is not judged by the model: errors are then logged as CMS_MASKED and not
  // counted. The test's own verdicts on the upset are reported unmasked.
  bit          cms_err_masked = 1'b0;
  int unsigned cms_err_masked_count = 0;

  function automatic void cms_error(string src, string msg);
    if (cms_err_masked) begin
      cms_err_masked_count++;
      $display("CMS_MASKED @%0d [%s] %s", cms_cycle, src, msg);
      return;
    end
    cms_err_count++;
    $display("CMS_ERROR @%0d [%s] %s", cms_cycle, src, msg);
  endfunction

  function automatic void cms_info(string src, string msg);
    $display("CMS_INFO  @%0d [%s] %s", cms_cycle, src, msg);
  endfunction

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Transactions
  ////////////////////////////////////////////////////////////////////////////////////////////////

  typedef enum int { PortCore, PortRevbm, PortCoreRevbm, PortCsr } port_e;

  // Fault injection kinds of the memory models (cms_tl_mem::inject)
  typedef enum int { InjErr = 0, InjRspIntg = 1, InjDataIntg = 2 } inj_kind_e;

  // One read of the revocation engine on tbre_tl_h: address, the first cycle it was presented
  // (a_valid) and the cycle of its handshake.
  typedef struct {
    logic [31:0]     addr;
    longint unsigned p_cycle;
    longint unsigned a_cycle;
  } tbre_rd_t;

  typedef struct {
    int unsigned           id;
    port_e                 port;
    tl_a_op_e              opcode;
    logic [31:0]           addr;
    logic [1:0]            size;
    logic [3:0]            mask;
    logic [31:0]           wdata;
    logic                  tag;          // tag_h2d: capability-store tag / capability-load hint
    logic                  bad_cmd_intg; // drive an inverted cmd_intg (fault injection)
    logic                  hold_after;   // issue nothing else until this one is answered (as Ibex
                                         // does after the second word of a capability store)
    // filled in by the host
    logic [7:0]            source;
    prim_mubi_pkg::mubi4_t ena;          // cheriot_ena_i at the A handshake
    longint unsigned       a_cycle;
    longint unsigned       d_cycle;
    logic [31:0]           rdata;
    logic                  rtag;
    logic                  err;
    tl_d_op_e              d_opcode;
    logic [1:0]            d_size;
  } cms_txn_t;

  function automatic cms_txn_t txn_new();
    cms_txn_t t;
    t.id = 0; t.port = PortCore; t.opcode = Get; t.addr = 0; t.size = 0; t.mask = 0; t.wdata = 0;
    t.tag = 0; t.bad_cmd_intg = 0; t.hold_after = 0; t.source = 0; t.ena = prim_mubi_pkg::MuBi4False;
    t.a_cycle = 0; t.d_cycle = 0; t.rdata = 0; t.rtag = 0; t.err = 0; t.d_opcode = AccessAck;
    t.d_size = 0;
    return t;
  endfunction

  function automatic bit is_write(cms_txn_t t);
    return t.opcode != Get;
  endfunction

  function automatic logic [3:0] size_mask(logic [31:0] addr, logic [1:0] size);
    case (size)
      2'd0:    return 4'b0001 << addr[1:0];
      2'd1:    return 4'b0011 << {addr[1], 1'b0};
      default: return 4'b1111;
    endcase
  endfunction

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Regions and granules
  ////////////////////////////////////////////////////////////////////////////////////////////////

  function automatic bit in_sram(logic [31:0] a);
    return a >= MainSramBase && a < MainSramTop;
  endfunction
  function automatic bit in_nvm(logic [31:0] a);
    return a >= NvmBase && a < NvmTop;
  endfunction
  function automatic bit is_tagged(logic [31:0] a);
    return in_sram(a) || in_nvm(a);
  endfunction

  // Granule (8-byte capability slot) index over SRAM then NVM.
  function automatic int unsigned granule(logic [31:0] a);
    if (in_sram(a)) return (a - MainSramBase) >> 3;
    return SramCaps + ((a - NvmBase) >> 3);
  endfunction
  function automatic logic [31:0] granule_addr(int unsigned g);
    if (g < SramCaps) return MainSramBase + g * 8;
    return NvmBase + (g - SramCaps) * 8;
  endfunction
  // Where the doc's map puts a granule's tag: word address in the meta SRAM and bit in it.
  function automatic logic [31:0] tag_word_addr(int unsigned g);
    if (g < SramCaps) return MetaSramTagBase + (g / 32) * 4;
    return MetaNvmTagBase + ((g - SramCaps) / 32) * 4;
  endfunction
  function automatic int unsigned tag_bit(int unsigned g);
    return (g < SramCaps) ? g % 32 : (g - SramCaps) % 32;
  endfunction

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // CHERIoT capabilities (CHERIoT ISA, in-memory format; independent of ibex_cheriot_pkg)
  //   word 0: address
  //   word 1: [31] R, [30:25] compressed perms, [24:22] otype, [21:18] exponent (15 means 24),
  //           [17:9] top mantissa T, [8:0] base mantissa B
  ////////////////////////////////////////////////////////////////////////////////////////////////

  function automatic int unsigned cap_exp(logic [31:0] meta);
    return (meta[21:18] == 4'hf) ? 24 : int'(meta[21:18]);
  endfunction

  // base = ((a >> (E+9)) + c_b) << (E+9) | B << E, with c_b = -1 when a[E+8:E] < B
  // (the address lies in the 2^(E+9) block above the base's). Arithmetic in 64 bits, result mod 2^32.
  function automatic logic [31:0] cap_base(logic [31:0] addr, logic [31:0] meta);
    longint unsigned a, b, amid, atop, base;
    int unsigned e;
    e    = cap_exp(meta);
    a    = 64'(addr);
    b    = 64'(meta[8:0]);
    amid = (a >> e) & 64'h1ff;
    atop = a >> (e + 9);
    if (amid < b) atop = atop - 64'd1;
    base = (atop << (e + 9)) + (b << e);
    return base[31:0];
  endfunction

  // Sealing capabilities: compressed permission format 2'b00 in [4:3] with any of U0/SE/US set.
  function automatic bit cap_is_sealing(logic [31:0] meta);
    logic [5:0] cp;
    cp = meta[30:25];
    return cp[4:3] == 2'b00 && cp[2:0] != 3'b000;
  endfunction

  // Revocation bit of a base address: bit b of word w covers heap bytes
  // MainSramBase + (w*32 + b)*8 (theory_of_operation.md). Bases outside the bitmap have none.
  function automatic bit revbm_index(logic [31:0] base, output int unsigned bit_idx);
    logic [31:0] off;
    off     = base - MainSramBase;
    bit_idx = off >> 3;
    return base >= MainSramBase && (off >> 3) < RevbmBytes * 8;
  endfunction

  // Encode word 1 of a capability whose base is `base` (aligned to 2^E for the chosen exponent).
  function automatic logic [31:0] cap_meta(logic [31:0] base, int unsigned e, logic [5:0] cperms,
                                           logic [2:0] otype, logic [8:0] top);
    logic [3:0] cexp;
    cexp = (e >= 24) ? 4'hf : 4'(e);
    return {1'b0, cperms, otype, cexp, top, 9'((base >> ((e >= 24) ? 24 : e)) & 32'h1ff)};
  endfunction

  // Permission encodings used by the tests (compressed formats)
  parameter logic [5:0] PermsMemRw   = 6'b011111;  // memory, read-write, all extras
  parameter logic [5:0] PermsMemRo   = 6'b010110;  // memory, read-only
  parameter logic [5:0] PermsExec    = 6'b001111;  // executable
  parameter logic [5:0] PermsSealing = 6'b000111;  // sealing: U0, SE, US
  parameter logic [5:0] PermsNone    = 6'b000000;  // no permissions (not a sealing capability)

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // TB storage: a word-addressed sparse memory shared between model ports and the scoreboard
  ////////////////////////////////////////////////////////////////////////////////////////////////

  class cms_store;
    string name;
    logic [31:0] mem [logic [31:0]];

    function new(string n);
      name = n;
    endfunction

    function logic [31:0] read(logic [31:0] a);
      logic [31:0] wa;
      wa = {a[31:2], 2'b00};
      return mem.exists(wa) ? mem[wa] : 32'h0;
    endfunction

    function void write(logic [31:0] a, logic [31:0] d, logic [3:0] be);
      logic [31:0] wa, old, m;
      wa  = {a[31:2], 2'b00};
      old = read(wa);
      m   = {{8{be[3]}}, {8{be[2]}}, {8{be[1]}}, {8{be[0]}}};
      mem[wa] = (old & ~m) | (d & m);
    endfunction
  endclass

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Functional coverage seen from the reference model (Xcelium; Verilator ignores covergroups)
  ////////////////////////////////////////////////////////////////////////////////////////////////

  class cms_cov;
    // Core access as the scoreboard judged it
    logic [1:0] c_op;      // 0 read, 1 full write, 2 partial write
    logic [1:0] c_size;
    logic [1:0] c_region;  // 0 sram, 1 nvm, 2 untagged
    logic       c_tag;
    logic       c_ena;
    logic       c_aligned;
    logic       c_rtag;
    logic       c_err;

    covergroup cms_core_access_cg;
      cp_op:      coverpoint c_op { bins read = {0}; bins full = {1}; bins partial = {2}; }
      cp_size:    coverpoint c_size { bins byte_ = {0}; bins half = {1}; bins word = {2}; }
      cp_region:  coverpoint c_region { bins sram = {0}; bins nvm = {1}; bins untagged = {2}; }
      cp_tag:     coverpoint c_tag;
      cp_ena:     coverpoint c_ena;
      cp_aligned: coverpoint c_aligned;
      cp_rtag:    coverpoint c_rtag iff (c_op == 0);
      cp_err:     coverpoint c_err;
      x_op_region_tag_ena: cross cp_op, cp_region, cp_tag, cp_ena;
      x_write_size_region: cross cp_op, cp_size, cp_region {
        ignore_bins rd = binsof(cp_op.read);
      }
    endgroup

    // One swept capability
    logic       s_tag;
    logic       s_sealing;
    logic [1:0] s_base_class;  // 0 in bitmap range, 1 below the heap, 2 above the bitmap
    logic       s_bit;
    logic       s_revoked;
    int         s_exp;
    logic       s_corr;        // a[E+8:E] < B: the base's block is below the address's

    covergroup cms_sweep_cap_cg;
      cp_tag:     coverpoint s_tag;
      cp_sealing: coverpoint s_sealing iff (s_tag);
      cp_base:    coverpoint s_base_class iff (s_tag) {
        bins in_range = {0}; bins below = {1}; bins above = {2};
      }
      cp_bit:     coverpoint s_bit iff (s_tag && s_base_class == 0);
      cp_revoked: coverpoint s_revoked;
      cp_exp:     coverpoint s_exp iff (s_tag) {
        bins e0 = {0}; bins low = {[1:4]}; bins mid = {[5:13]}; bins e14 = {14}; bins e24 = {24};
      }
      cp_corr:    coverpoint s_corr iff (s_tag);
      x_outcome:  cross cp_sealing, cp_base, cp_bit;
    endgroup

    // TBRE_START outcome
    logic [2:0] st_outcome;  // 0 accepted, 1 num 0, 2 base below the SRAM, 3 base above it and
                             // not in the NVM, 4 not CHERIoT, 5 while busy, 6 wrote 0
    logic       st_clamped;
    logic       st_nvm;      // accepted sweep of the NVM

    covergroup cms_tbre_start_cg;
      cp_outcome: coverpoint st_outcome {
        bins accepted = {0}; bins num_zero = {1}; bins base_low = {2}; bins base_high = {3};
        bins not_cheriot = {4}; bins while_busy = {5}; bins wrote_zero = {6};
      }
      cp_clamped: coverpoint st_clamped iff (st_outcome == 0);
      cp_nvm:     coverpoint st_nvm iff (st_outcome == 0);
      x_region_clamped: cross cp_nvm, cp_clamped;
    endgroup

    // End of a sweep, as the reference model saw it
    logic e_failed;     // a fault the test injected ended it with TBRE_STATUS.sweep_err
    logic e_denied;     // ... and that fault was only an error response to a read of the swept
                        // memory, which raises no alert
    logic e_nvm;
    logic e_intr_en;    // INTR_ENABLE.tbre_done set as it ended

    covergroup cms_tbre_end_cg;
      cp_failed:  coverpoint e_failed;
      cp_denied:  coverpoint e_denied iff (e_failed);
      cp_nvm:     coverpoint e_nvm;
      cp_intr_en: coverpoint e_intr_en;
      x_failed_intr: cross cp_failed, cp_intr_en;
    endgroup

    // A core write to a swept capability, by when it was answered relative to the engine's reads
    // of that capability (theory_of_operation.md "Revocation Engine": watched from the
    // presentation of the lower word's read)
    logic [1:0] w_window;  // 0 before the lower word's read could be presented, 1 around it
                           // (either outcome), 2 after it and before the upper word's read,
                           // 3 after the upper word's read was presented
    logic       w_upper;   // the write is to the upper word
    logic       w_rev;     // it stores a tagged capability the sweep would revoke
    logic       w_nvm;

    covergroup cms_sweep_snoop_cg;
      cp_window: coverpoint w_window {
        bins early = {0}; bins around = {1}; bins between = {2}; bins late = {3};
      }
      cp_upper:  coverpoint w_upper;
      cp_rev:    coverpoint w_rev;
      cp_nvm:    coverpoint w_nvm;
      x_window_word: cross cp_window, cp_upper;
      x_window_rev:  cross cp_window, cp_rev;
    endgroup

    // A capability store to the NVM (theory_of_operation.md "Write-to-Read-and-Compare Filter")
    logic       n_upper;
    logic [1:0] n_outcome;  // 0 matched, 1 data differs, 2 not a full-word PutFullData,
                            // 3 out of the W0-W1 sequence
    logic       n_old_tag;  // the tag before the store (as predicted)

    covergroup cms_nvm_cap_cg;
      cp_upper:   coverpoint n_upper;
      cp_outcome: coverpoint n_outcome {
        bins match = {0}; bins differs = {1}; bins partial = {2}; bins out_of_seq = {3};
      }
      cp_old_tag: coverpoint n_old_tag;
      x_word_outcome: cross cp_upper, cp_outcome;
      x_outcome_tag:  cross cp_outcome, cp_old_tag;
    endgroup

    // Meta SRAM window access (revbm / corerevbm)
    logic       a_port;      // 0 revbm, 1 corerevbm
    logic [2:0] a_outcome;   // 0 allowed, 1 not CHERIoT, 2 below, 3 tag region / above, 4 misaligned,
                             // 5 sub-word, 6 PutPartialData
    logic       a_write;

    covergroup cms_meta_access_cg;
      cp_port:    coverpoint a_port;
      cp_outcome: coverpoint a_outcome {
        bins allowed = {0}; bins not_cheriot = {1}; bins below = {2}; bins above = {3};
        bins misaligned = {4}; bins subword = {5}; bins partial = {6};
      }
      cp_write:   coverpoint a_write;
      x_all:      cross cp_port, cp_outcome, cp_write {
        ignore_bins partial_read = binsof(cp_outcome.partial) && binsof(cp_write) intersect {0};
      }
    endgroup

    // Core store during a sweep, relative to the sweep
    logic [1:0] r_kind;  // 0 outside the sweep range, 1 in range other meta word, 2 same meta word,
                         // 3 a capability the sweep revokes
    covergroup cms_sweep_race_cg;
      cp_kind: coverpoint r_kind {
        bins outside = {0}; bins in_range = {1}; bins same_word = {2}; bins revoked_cap = {3};
      }
    endgroup

    function new();
      cms_core_access_cg = new();
      cms_sweep_cap_cg   = new();
      cms_tbre_start_cg  = new();
      cms_meta_access_cg = new();
      cms_sweep_race_cg  = new();
      cms_tbre_end_cg    = new();
      cms_sweep_snoop_cg = new();
      cms_nvm_cap_cg     = new();
    endfunction
  endclass

  ////////////////////////////////////////////////////////////////////////////////////////////////
  // Scoreboard / reference model
  ////////////////////////////////////////////////////////////////////////////////////////////////

  // A granule's predicted tag is a set: bit 0 "may be 0", bit 1 "may be 1". Exact predictions
  // have one bit; concurrency (core vs TBRE) and reset leave a two-value set, which the next
  // observation (a capability load or a meta SRAM compare) resolves.
  parameter logic [1:0] Tag0   = 2'b01;
  parameter logic [1:0] Tag1   = 2'b10;
  parameter logic [1:0] TagAny = 2'b11;

  function automatic logic [1:0] tag_exact(logic t);
    return t ? Tag1 : Tag0;
  endfunction
  function automatic string tagset_str(logic [1:0] s);
    case (s)
      Tag0:    return "0";
      Tag1:    return "1";
      TagAny:  return "{0,1}";
      default: return "{}";
    endcase
  endfunction

  typedef struct {
    int unsigned     g;
    logic [31:0]     addr;
    logic            tag;
    bit              notag;   // writes no tag: an NVM capability store other than a verified W1
    logic [1:0]      nset;    // the granule's tag the store leaves, before any clear
    logic [31:0]     w0, w1;  // shadow data of the granule after the store
    longint unsigned a_cycle;
    longint unsigned d_cycle;
  } store_rec_t;

  class cms_sb;
    cms_store meta;       // the meta SRAM model's storage
    cms_store data;       // the data memory model's storage
    cms_cov   cov;

    // Reference state
    logic [1:0]  tagset [int unsigned];   // granule -> predicted tag set; absent = exactly 0
    logic [31:0] sdata  [logic [31:0]];   // word address -> data
    logic [31:0] srevbm [int unsigned];   // bitmap word index -> data

    // CSR shadow. A field written while a sweep may be running is unknown until rewritten.
    logic [31:0] csr_base, csr_num;
    bit          csr_base_known, csr_num_known;
    // Interrupt (registers.md INTR_*; programmers_guide.md: a level interrupt, raised when the
    // engine stops being active) and TBRE_STATUS's sticky flags. Sets, as the tags are: a flag a
    // running sweep may set at any time is a two-value set until the sweep is seen to end.
    bit          intr_enable;
    logic [1:0]  intr_state;
    bit          intr_clr_in_sweep;     // INTR_STATE cleared while a sweep may have been ending
    bit          start_err;
    logic [1:0]  sweep_err;
    bit          sweep_err_clr_in_sweep;
    // TBRE_EPOCH.count: sweeps that ended without an error, modulo 2^31
    logic [30:0] epoch_count;
    // ALERT_TEST.regwen (registers.md): cleared by a write of 0 to it, set again only by reset. The
    // tests check the alert a write of ALERT_TEST.fatal_fault raises while it is set.
    bit          alert_test_regwen;

    // Sweep
    bit              sweep_active;
    logic [31:0]     sweep_base;
    int unsigned     sweep_caps;
    longint unsigned sweep_a_cycle;
    bit              sweep_fail_exp;   // a directed test injected a fault into this sweep
    bit              sweep_fail_denied; // ... only error responses to reads of the swept memory
    logic [31:0]     tbre_exp_reads[$];
    // TBRE reads seen before the TBRE_START write's response. The engine starts when it accepts
    // the write (A channel) but the scoreboard arms the sweep on the D response, so the first reads
    // can arrive first. Held here and replayed by start_write(); left over means no sweep started.
    tbre_rd_t        tbre_early_reads[$];
    localparam int unsigned TbreEarlyMax = 8;
    int unsigned     tbre_reads_seen;
    longint unsigned tbre_prev_a;      // handshake of the engine's previous read in this sweep
    // Per swept granule: the earliest cycle the engine can have presented its lower word's read,
    // and when that read, and the upper word's, were first seen presented on tbre_tl_h. The path
    // from the engine to tbre_tl_h only passes requests on (theory_of_operation.md "Timing"), so
    // the engine presented the read no later than it is seen, and no earlier than the cycle after
    // the handshake of its previous read.
    longint unsigned rd_lb [int unsigned];
    longint unsigned rd_p0 [int unsigned];
    longint unsigned rd_p1 [int unsigned];
    logic [31:0]     snap_revbm [int unsigned];
    logic [1:0]      snap_tag   [int unsigned];   // swept granules' predicted tags at the start
    store_rec_t      race_q     [int unsigned][$]; // core writes to a swept granule during the sweep
    bit              racy_revbm [int unsigned];   // granule whose base bit changed during the sweep
    bit              trace_en;                    // +cms_trace
    logic [31:0]     trace_ga;
    bit              sweep_dc   [int unsigned];   // granule a directed test declared don't-care
    bit              sweep_keep [int unsigned];   // granule whose clear a fault must suppress
    bit              sweep_force0 [int unsigned]; // granule a fault must revoke
    store_rec_t      recent_stores[$];

    // The WTRC's capability store sequence, by the responses (which come in request order): a
    // W0 opens a capability, W1's answer closes it.
    bit          wtrc_open;
    int unsigned wtrc_g;
    bit          wtrc_w0_ok;

    // Expectations set by directed tests
    int unsigned exp_core_err;       // the next N core transactions must be answered with d_error
    int          err_tag_effect;     // tag effect of such an errored write: 0 keep, 1 update, 2 unknown
    bit          err_data_written;   // ... and its data write reached memory (a tag-path fault)
    int unsigned core_err_any;       // the next N core transactions may be answered with or without
                                     // d_error (the documents leave it open)

    // The tag filter's sticky tag (doc:72-77): an aligned read response captures the tag it
    // presents, the next unaligned read response presents it again, whatever the read, and clears
    // it. sticky_known 0: not known (after a reset or an error response); the next is not checked.
    bit          sticky_known;
    logic        sticky_tag;

    // The C reference model (cms_tb, +cms_cmodel), told what the tests declare about the faults
    // they inject; cm_directed_err: the last core transaction carried a directed (injected) error.
    chandle      cm;
    bit          cm_directed_err;

    // Statistics
    int unsigned n_core, n_tag_checks, n_data_checks, n_err_checks, n_revbm, n_csr;
    int unsigned n_sweeps, n_sweep_caps, n_sweep_revoked, n_backdoor, n_ignored_starts;
    int unsigned n_meta_writes, n_nvm_cap, n_nvm_cap_tagged, n_wtrc_faults;
    // Swept capabilities with a core write: watched (kept), not watched (resolved by the engine),
    // or written around the start of the watch (either)
    int unsigned n_snoop_kept, n_snoop_before, n_snoop_around;

    // Scoreboard fault injection (+cms_sb_corrupt=<kind>[,<n>]): corrupt the n-th prediction of the
    // given kind; the run must then fail. Kinds: load_tag, sweep, revbm, err, data, race (the tag
    // of a swept capability a watched core write keeps), epoch (a TBRE_EPOCH read).
    // The tests of cms_tests_eg2.svh call corrupt() on their own expectations: mubi (the tag a
    // valid-encoding capability store leaves), lockstep (fatal_fault after an upset), latency (the
    // cycles the subsystem may add to one access).
    string       corrupt_kind;
    int unsigned corrupt_n;
    bit          corrupt_done;
    int unsigned corrupt_cnt;

    function new(cms_store meta_store, cms_store data_store);
      string s;
      int    comma;
      meta = meta_store;
      data = data_store;
      cov  = new();
      csr_base = 0; csr_num = 0; csr_base_known = 1; csr_num_known = 1;
      intr_enable = 0; intr_state = Tag0; intr_clr_in_sweep = 0;
      start_err = 0; sweep_err = Tag0; sweep_err_clr_in_sweep = 0; epoch_count = 0;
      alert_test_regwen = 1;
      sweep_active = 0; sweep_fail_exp = 0; sweep_fail_denied = 0;
      wtrc_open = 0; wtrc_g = 0; wtrc_w0_ok = 0;
      exp_core_err = 0; err_tag_effect = 2; err_data_written = 0; core_err_any = 0;
      sticky_known = 0;
      corrupt_kind = ""; corrupt_n = 1; corrupt_done = 0; corrupt_cnt = 0;
      if ($value$plusargs("cms_sb_corrupt=%s", s)) begin
        comma = -1;
        for (int i = 0; i < s.len(); i++) if (s[i] == ",") comma = i;
        if (comma > 0) begin
          corrupt_kind = s.substr(0, comma - 1);
          corrupt_n    = s.substr(comma + 1, s.len() - 1).atoi();
        end else begin
          corrupt_kind = s;
        end
        cms_info("sb", $sformatf("scoreboard fault injection armed: %s, occurrence %0d",
                                 corrupt_kind, corrupt_n));
      end
      // +cms_trace=<hex address>: log every event that decides that granule's tag (debug only).
      trace_en = $value$plusargs("cms_trace=%h", trace_ga);
      if (trace_en) cms_info("sb", $sformatf("tracing granule 0x%08x", trace_ga & ~32'h7));
    endfunction

    function void err(string msg);
      cms_error("sb", msg);
    endfunction

    function bit traced(logic [31:0] ga);
      return trace_en && granule(ga) == granule(trace_ga);
    endfunction
    function void trace(string msg);
      cms_info("trace", msg);
    endfunction

    // True once, on the corrupt_n-th call for `kind`: the caller then inverts its prediction.
    function bit corrupt(string kind, string what);
      if (corrupt_done || corrupt_kind != kind) return 0;
      corrupt_cnt++;
      if (corrupt_cnt != corrupt_n) return 0;
      corrupt_done = 1;
      $display("CMS_CORRUPT @%0d +cms_sb_corrupt: corrupted %s (%s)", cms_cycle, kind, what);
      return 1;
    endfunction

    // ---- reference state accessors ----
    function logic [1:0] get_tag(int unsigned g);
      return tagset.exists(g) ? tagset[g] : Tag0;
    endfunction
    function void set_tag(int unsigned g, logic [1:0] s);
      tagset[g] = s;
    endfunction
    function logic [31:0] get_data(logic [31:0] a);
      logic [31:0] wa;
      wa = {a[31:2], 2'b00};
      return sdata.exists(wa) ? sdata[wa] : 32'h0;
    endfunction
    function void put_data(logic [31:0] a, logic [31:0] d, logic [3:0] be);
      logic [31:0] wa, m;
      wa = {a[31:2], 2'b00};
      m  = {{8{be[3]}}, {8{be[2]}}, {8{be[1]}}, {8{be[0]}}};
      sdata[wa] = (get_data(wa) & ~m) | (d & m);
    endfunction
    function logic [31:0] get_revbm(int unsigned w);
      return srevbm.exists(w) ? srevbm[w] : 32'h0;
    endfunction
    function bit revbm_bit(int unsigned b, bit snapshot);
      int unsigned w;
      w = b / 32;
      if (snapshot) return snap_revbm.exists(w) ? snap_revbm[w][b % 32] : 1'b0;
      return get_revbm(w)[b % 32];
    endfunction

    // Would the TRVK load barrier revoke a tagged capability with these two words?
    function bit revocable(logic [31:0] w0, logic [31:0] w1, bit snapshot);
      int unsigned b;
      if (cap_is_sealing(w1)) return 0;
      if (!revbm_index(cap_base(w0, w1), b)) return 0;
      return revbm_bit(b, snapshot);
    endfunction

    // ---- CHERIoT mode ----
    function bit ena_true(prim_mubi_pkg::mubi4_t e);
      return e == prim_mubi_pkg::MuBi4True;
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Core data port (cored_tl_d)
    ////////////////////////////////////////////////////////////////////////////////////////////

    function void check_core(cms_txn_t t);
      bit          wr, in_tagged, lookup, exp_err, mem_err, aligned, nvm_cap, cap_tag;
      bit          directed_err;
      int unsigned g;
      logic [1:0]  exp_set;
      logic [31:0] exp_data, m;
      string       ctx;

      n_core++;
      wr      = is_write(t);
      in_tagged  = is_tagged(t.addr);
      aligned = !t.addr[2];
      lookup  = ena_true(t.ena) && in_tagged;
      g       = in_tagged ? granule(t.addr) : 0;
      // A capability store to the NVM: a write in strict CHERIoT mode to the NVM with the tag
      // sideband set (theory_of_operation.md "Write-to-Read-and-Compare Filter"). It is not
      // written: it reads the word and compares; only a verified W1 writes the tag.
      nvm_cap = wr && ena_true(t.ena) && in_nvm(t.addr) && t.tag;
      ctx     = $sformatf("core %s addr=0x%08x size=%0d mask=%b tag=%b ena=%0h",
                          t.opcode.name(), t.addr, t.size, t.mask, t.tag, t.ena);

      // Response shape (TL-UL)
      if (t.d_opcode != (wr ? AccessAck : AccessAckData))
        err($sformatf("%s: d_opcode %s", ctx, t.d_opcode.name()));
      if (t.d_size != t.size) err($sformatf("%s: d_size %0d", ctx, t.d_size));

      // Error: the TB memory answers d_error in the error window, and to every write to the NVM,
      // which is read-only on the interconnect (programmers_guide.md "Storing Capabilities in the
      // NVM"); a capability store to the NVM errs unless verified; a directed test may expect more.
      mem_err = (t.addr >= DataErrBase && t.addr < UntaggedTop) || (wr && in_nvm(t.addr) && !nvm_cap);
      exp_err = mem_err;
      cap_tag = 0;
      if (nvm_cap) exp_err = nvm_cap_store(t, g, cap_tag);
      directed_err = exp_core_err > 0;
      cm_directed_err = directed_err;
      if (directed_err) begin
        exp_err = 1;
        exp_core_err--;
      end
      if (core_err_any > 0) begin
        core_err_any--;
        if (t.err) exp_err = 1;
      end
      if (corrupt("err", ctx)) exp_err = !exp_err;
      n_err_checks++;
      if (t.err != exp_err) err($sformatf("%s: d_error=%b, expected %b", ctx, t.err, exp_err));

      cov.c_op      = !wr ? 2'd0 : (t.opcode == PutFullData ? 2'd1 : 2'd2);
      cov.c_size    = t.size;
      cov.c_region  = in_sram(t.addr) ? 2'd0 : (in_nvm(t.addr) ? 2'd1 : 2'd2);
      cov.c_tag     = t.tag;
      cov.c_ena     = ena_true(t.ena);
      cov.c_aligned = aligned;
      cov.c_rtag    = t.rtag;
      cov.c_err     = t.err;
      cov.cms_core_access_cg.sample();

      if (wr) begin
        // The data write is forwarded unchanged; the tag of the granule takes the store's tag on
        // every write to tagged memory in CHERIoT mode (a non-capability store has tag 0), also
        // when the data write fails ("a failed data write still updates the tag"). A capability
        // store to the NVM writes no data, and only a verified W1 changes the tag.
        logic [31:0] old_w0, old_w1;
        logic [1:0]  nset;
        // Only read responses carry a tag (theory_of_operation.md "Tag Filter").
        if (t.rtag) err($sformatf("%s: write answered with tag 1", ctx));
        old_w0 = in_tagged ? get_data(granule_addr(g)) : 32'h0;
        old_w1 = in_tagged ? get_data(granule_addr(g) + 4) : 32'h0;
        if (!nvm_cap && (!t.err || err_data_written)) put_data(t.addr, t.wdata, t.mask);
        if (lookup) begin
          if (nvm_cap)             nset = cap_tag ? Tag1 : unchanged_tag(g);
          else                     nset = tag_exact(t.tag);
          if (t.err && directed_err) begin
            case (err_tag_effect)
              0:       nset = nvm_cap ? unchanged_tag(g) : get_tag(g);
              1:       nset = tag_exact(t.tag);
              default: nset = TagAny;
            endcase
          end
          note_store(g, t, nset, old_w0, old_w1);
        end
        if (corrupt("data", ctx) && !t.err && !nvm_cap) put_data(t.addr, ~t.wdata, t.mask);
        return;
      end

      // Read: data from the shadow
      if (!t.err) begin
        m        = {{8{t.mask[3]}}, {8{t.mask[2]}}, {8{t.mask[1]}}, {8{t.mask[0]}}};
        exp_data = get_data(t.addr);
        n_data_checks++;
        if ((t.rdata & m) != (exp_data & m))
          err($sformatf("%s: rdata=0x%08x, expected 0x%08x", ctx, t.rdata & m, exp_data & m));
      end

      // Tag
      if (!t.err) begin
        if (aligned && t.tag && lookup && t.size == 2'd2) begin
          exp_set = get_tag(g);
          if (corrupt("load_tag", ctx)) exp_set = ~exp_set;
          n_tag_checks++;
          if (!exp_set[t.rtag])
            err($sformatf("%s: tag %b, expected %s", ctx, t.rtag, tagset_str(exp_set)));
          else if (!(t.rtag && sweep_active && in_sweep(g, sweep_base, sweep_caps)))
            // Resolve an ambiguous prediction, except a 1 during a sweep over the granule: the
            // engine may not have reached it yet (sweep_done resolves it).
            set_tag(g, tag_exact(t.rtag));
        end else if (aligned) begin
          // Not a capability load (no hint, CHERIoT off, untagged memory): never a tag.
          n_tag_checks++;
          if (t.rtag) err($sformatf("%s: tag 1 on a load that is not a capability load", ctx));
        end else if (sticky_known) begin
          // An unaligned read: the captured tag (a capability's second word gets its first word's
          // tag; any other unaligned read gets whatever the last aligned read captured).
          exp_set = sticky_tag ? Tag1 : Tag0;
          if (corrupt("sticky_tag", ctx)) exp_set = ~exp_set;
          n_tag_checks++;
          if (!exp_set[t.rtag])
            err($sformatf("%s: tag %b, the captured (sticky) tag is %b", ctx, t.rtag, sticky_tag));
        end
      end
      // A write leaves the captured tag alone, so only reads update it. Whether an error response
      // captures or clears it is not documented: the next unaligned read is then not checked.
      sticky_known = !t.err;
      sticky_tag   = !t.err && aligned && t.rtag;
    endfunction

    // A capability store to the NVM (theory_of_operation.md "Write-to-Read-and-Compare Filter").
    // W0 is answered with d_error if it is not a full-word PutFullData, is out of sequence, or its
    // data differs from the NVM's; W1 sets the tag only if it and its W0 both matched, and is
    // answered with d_error otherwise. Out of sequence: a W1 with no capability open, a W0 while
    // one is open (the core issues nothing between W0 and W1). Returns the expected d_error; tag_set
    // is the verified W1's tag write.
    function bit nvm_cap_store(cms_txn_t t, int unsigned g, output bit tag_set);
      bit full, match, upper, seq_bad, ok;
      full    = t.opcode == PutFullData && t.mask == 4'hf && t.size == 2'd2;
      match   = full && t.wdata == get_data(t.addr);
      upper   = t.addr[2];
      tag_set = 0;
      n_nvm_cap++;
      cov.n_upper   = upper;
      cov.n_old_tag = get_tag(g)[1];
      if (!upper) begin
        seq_bad    = wtrc_open;
        wtrc_open  = 1;
        wtrc_g     = g;
        wtrc_w0_ok = match && !seq_bad;
        ok         = wtrc_w0_ok;
      end else begin
        seq_bad   = !wtrc_open || wtrc_g != g;
        ok        = !seq_bad && wtrc_w0_ok && match;
        wtrc_open = 0;
        tag_set   = ok;
        if (ok) n_nvm_cap_tagged++;
      end
      // A partial or out-of-sequence capability store is not something the core produces: it also
      // raises fatal_fault (the test that drives one expects the alert).
      if (!full || seq_bad) n_wtrc_faults++;
      cov.n_outcome = seq_bad ? 2'd3 : (!full ? 2'd2 : (!match ? 2'd1 : 2'd0));
      cov.cms_nvm_cap_cg.sample();
      return !ok;
    endfunction

    // The tag a write leaves that does not change it (a failed capability store to the NVM): what
    // it was before any clear of a running sweep, which the write's watch supersedes.
    function logic [1:0] unchanged_tag(int unsigned g);
      if (sweep_active && race_q.exists(g) && race_q[g].size() > 0) return race_q[g][$].nset;
      if (sweep_active && snap_tag.exists(g)) return snap_tag[g];
      return get_tag(g);
    endfunction

    // A core write to a tagged granule in CHERIoT mode, with the tag set it leaves.
    function void note_store(int unsigned g, cms_txn_t t, logic [1:0] nset,
                             logic [31:0] old_w0, logic [31:0] old_w1);
      store_rec_t r;
      logic [31:0] ga;
      ga = granule_addr(g);
      r.g = g; r.addr = t.addr; r.tag = t.tag; r.nset = nset;
      r.notag = ena_true(t.ena) && in_nvm(t.addr) && t.tag && !(t.addr[2] && !t.err);
      r.a_cycle = t.a_cycle; r.d_cycle = t.d_cycle;
      r.w0 = get_data(ga); r.w1 = get_data(ga + 4);
      recent_stores.push_back(r);
      if (recent_stores.size() > 16) void'(recent_stores.pop_front());
      if (traced(ga)) trace($sformatf("core write 0x%08x tag %0d accepted @%0d answered @%0d -> %s%s",
                                      t.addr, t.tag, t.a_cycle, t.d_cycle, tagset_str(nset),
                                      sweep_active ? " (sweep running)" : ""));

      if (sweep_active) sample_race(g, old_w0, old_w1);
      if (sweep_active && in_sweep(g, sweep_base, sweep_caps)) begin
        // Whether the engine resolved the capability before the write or watched it is decided
        // when the sweep is seen to end (sweep_done), from when the write was answered. Until then
        // a revocable capability written now may be read either way.
        race_q[g].push_back(r);
        if (nset[1] && (revocable(r.w0, r.w1, 0) || revocable(r.w0, r.w1, 1)))
          nset = nset | Tag0;
      end
      set_tag(g, nset);
    endfunction

    function bit in_sweep(int unsigned g, logic [31:0] base, int unsigned caps);
      logic [31:0] a;
      a = granule_addr(g);
      return a >= base && a < base + caps * 8;
    endfunction

    // Where a core store during a sweep lands: outside the range; in range; in the meta word of the
    // capability the engine is at; or over a in_tagged capability the sweep is to revoke.
    function void sample_race(int unsigned g, logic [31:0] old_w0, logic [31:0] old_w1);
      logic [31:0] ga, cur;
      ga  = granule_addr(g);
      cur = sweep_base + (tbre_reads_seen / 2) * 8;
      if (!in_sweep(g, sweep_base, sweep_caps))
        cov.r_kind = 2'd0;
      else if (snap_tag.exists(g) && snap_tag[g][1] && revocable(old_w0, old_w1, 1))
        cov.r_kind = 2'd3;
      else if ((ga >> 8) == (cur >> 8))
        cov.r_kind = 2'd2;
      else
        cov.r_kind = 2'd1;
      cov.cms_sweep_race_cg.sample();
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Meta SRAM windows: revbm_tl_d (software) and corerevbm_tl (the core's TRVK)
    ////////////////////////////////////////////////////////////////////////////////////////////

    function void check_revbm(cms_txn_t t, bit core_port);
      bit          allowed, exp_err;
      int unsigned w;
      logic [31:0] exp, old;
      logic [2:0]  outcome;
      string       ctx;

      n_revbm++;
      ctx = $sformatf("%s %s addr=0x%08x size=%0d ena=%0h", core_port ? "corerevbm" : "revbm",
                      t.opcode.name(), t.addr, t.size, t.ena);
      // theory_of_operation.md "Access Checkers": CHERIoT mode, inside the bitmap, word-aligned
      // full word, Get or PutFullData. The tag regions above the bitmap are never reachable.
      if (!ena_true(t.ena))                                       outcome = 3'd1;
      else if (t.addr < MetaRevbmBase)                            outcome = 3'd2;
      else if (t.addr >= MetaNvmTagBase)                          outcome = 3'd3;
      else if (t.opcode == PutPartialData)                        outcome = 3'd6;
      else if (t.addr[1:0] != 2'b00)                              outcome = 3'd4;
      else if (t.size != 2'd2)                                    outcome = 3'd5;
      else                                                        outcome = 3'd0;
      allowed = outcome == 3'd0;
      exp_err = !allowed;
      if (corrupt("err", ctx)) exp_err = !exp_err;
      n_err_checks++;
      if (t.err != exp_err) err($sformatf("%s: d_error=%b, expected %b", ctx, t.err, exp_err));
      if (t.d_opcode != (is_write(t) ? AccessAck : AccessAckData))
        err($sformatf("%s: d_opcode %s", ctx, t.d_opcode.name()));

      cov.a_port = core_port; cov.a_outcome = outcome; cov.a_write = is_write(t);
      cov.cms_meta_access_cg.sample();

      if (!allowed || t.err) return;
      w = (t.addr - MetaRevbmBase) >> 2;
      if (is_write(t)) begin
        old = get_revbm(w);
        srevbm[w] = t.wdata;
        if (trace_en) begin
          int unsigned tb;
          logic [31:0] tga;
          tga = trace_ga & ~32'h7;
          if (revbm_index(cap_base(get_data(tga), get_data(tga + 4)), tb) && tb / 32 == w &&
              old[tb % 32] != t.wdata[tb % 32])
            trace($sformatf("bitmap: base bit %0d of 0x%08x %0d -> %0d, write accepted @%0d%s", tb,
                            tga, old[tb % 32], t.wdata[tb % 32], t.a_cycle,
                            sweep_active ? " (sweep running)" : ""));
        end
        if (sweep_active && old != t.wdata) note_revbm_change(w, old ^ t.wdata);
      end else begin
        exp = get_revbm(w);
        if (corrupt("revbm", ctx)) exp = ~exp;
        n_data_checks++;
        if (t.rdata != exp)
          err($sformatf("%s: rdata=0x%08x, expected 0x%08x", ctx, t.rdata, exp));
      end
    endfunction

    // A bitmap word changed while a sweep runs: every swept capability whose base bit changed may
    // be revoked under either value.
    function void note_revbm_change(int unsigned w, logic [31:0] changed);
      int unsigned b;
      logic [31:0] ga;
      for (int unsigned i = 0; i < sweep_caps; i++) begin
        ga = sweep_base + i * 8;
        if (revbm_index(cap_base(get_data(ga), get_data(ga + 4)), b) && b / 32 == w &&
            changed[b % 32]) begin
          racy_revbm[granule(ga)] = 1;
          set_tag(granule(ga), get_tag(granule(ga)) | Tag0);
          if (traced(ga)) trace($sformatf("0x%08x: base bit %0d changed during the sweep, now %0d",
                                          ga, b, get_revbm(w)[b % 32]));
        end
      end
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // CSRs (regs_tl_d)
    ////////////////////////////////////////////////////////////////////////////////////////////

    function void check_csr(cms_txn_t t);
      logic [5:0]  off;
      bit          exp_err, known;
      logic [31:0] exp;
      logic [1:0]  s;
      string       ctx;

      n_csr++;
      off = t.addr[5:0];
      ctx = $sformatf("csr %s off=0x%02x data=0x%08x mask=%b", t.opcode.name(), off,
                      is_write(t) ? t.wdata : t.rdata, t.mask);
      // reggen: unmapped offsets, and writes that leave out a byte holding a field, are errors; a
      // command integrity error is an error too (and a fatal alert, checked by the test).
      exp_err = !csr_mapped(off) || t.bad_cmd_intg ||
                (is_write(t) && (csr_write_bytes(off) & ~t.mask) != 4'h0);
      n_err_checks++;
      if (t.err != exp_err) err($sformatf("%s: d_error=%b, expected %b", ctx, t.err, exp_err));
      if (t.err) return;

      if (is_write(t)) begin
        case (off)
          // rw1c; a sweep ending in the cycle it is cleared does not set it again
          // (programmers_guide.md), so a clear while a sweep may be ending leaves it open.
          CsrIntrState: if (t.wdata[0]) begin
            intr_state = Tag0;
            if (sweep_active) intr_clr_in_sweep = 1;
          end
          CsrIntrEnable: intr_enable = t.wdata[0];
          CsrIntrTest: if (t.wdata[0]) begin
            intr_state        = Tag1;
            intr_clr_in_sweep = 0;
          end
          CsrTbreBase: begin
            if (sweep_active) csr_base_known = 0;
            else begin csr_base = t.wdata & 32'hffff_fff8; csr_base_known = 1; end
          end
          CsrTbreNum: begin
            if (sweep_active) csr_num_known = 0;
            else begin csr_num = t.wdata & 32'h7fff_ffff; csr_num_known = 1; end
          end
          CsrTbreStart: start_write(t);
          CsrTbreStatus: begin
            if (t.wdata[StatusStartErr]) start_err = 0;
            if (t.wdata[StatusSweepErr]) begin
              sweep_err = Tag0;
              if (sweep_active && sweep_fail_exp) sweep_err_clr_in_sweep = 1;
            end
          end
          // rw0c regwen; the alert a write of fatal_fault raises is the test's to check
          CsrAlertTest: if (!t.wdata[AlertTestRegwen]) alert_test_regwen = 0;
          default: ;  // the read-only registers
        endcase
        return;
      end

      known = 1;
      case (off)
        CsrIntrState: begin
          // A running sweep may end, and raise tbre_done, at any time.
          known = 0;
          s = sweep_active ? (intr_state | Tag1) : intr_state;
          if (t.rdata[31:1] != 0) err($sformatf("%s: reserved bits set", ctx));
          if (!s[t.rdata[0]])
            err($sformatf("%s: tbre_done %b, expected %s", ctx, t.rdata[0], tagset_str(s)));
          else if (!sweep_active) intr_state = tag_exact(t.rdata[0]);
        end
        CsrIntrEnable: exp = {31'h0, intr_enable};
        CsrIntrTest:   exp = 32'h0;
        CsrAlertTest:  exp = 32'(alert_test_regwen) << AlertTestRegwen;  // fatal_fault reads 0
        CsrTbreStart:  exp = 32'h0;
        CsrTbreBase:   begin exp = csr_base; known = csr_base_known; end
        CsrTbreNum:    begin exp = csr_num;  known = csr_num_known;  end
        CsrRegwen:     begin
          // Low exactly while the engine is active: high with a sweep running means it is over.
          known = 0;
          if (t.rdata[31:1] != 0) err($sformatf("%s: reserved bits set", ctx));
          if (t.rdata[0] == 1'b0 && !sweep_active)
            err($sformatf("%s: REGWEN low with no sweep running", ctx));
          if (t.rdata[0] && sweep_active) sweep_done(t, "TBRE_REGWEN high");
        end
        CsrTbreStatus: begin
          known = 0;
          if ((t.rdata & ~((32'h1 << StatusBusy) | (32'h1 << StatusStartErr) |
                           (32'h1 << StatusSweepErr))) != 0)
            err($sformatf("%s: reserved bits set", ctx));
          if (t.rdata[StatusBusy] && !sweep_active) err($sformatf("%s: busy with no sweep running", ctx));
          if (!t.rdata[StatusBusy] && sweep_active) sweep_done(t, "TBRE_STATUS.busy low");
          if (t.rdata[StatusStartErr] != start_err)
            err($sformatf("%s: start_err %b, expected %b", ctx, t.rdata[StatusStartErr], start_err));
          // A fault injected into the running sweep sets sweep_err when it hits.
          s = (sweep_active && sweep_fail_exp) ? (sweep_err | Tag1) : sweep_err;
          if (!s[t.rdata[StatusSweepErr]])
            err($sformatf("%s: sweep_err %b, expected %s", ctx, t.rdata[StatusSweepErr],
                          tagset_str(s)));
          else if (!(sweep_active && sweep_fail_exp)) sweep_err = tag_exact(t.rdata[StatusSweepErr]);
        end
        CsrTbreEpoch:  begin
          // Odd exactly while the engine is active: even with a sweep running means it is over.
          if (sweep_active && !t.rdata[0]) sweep_done(t, "TBRE_EPOCH even");
          exp = {epoch_count, sweep_active};
          if (corrupt("epoch", ctx)) exp = exp ^ 32'h2;
          n_data_checks++;
        end
        default: known = 0;
      endcase
      if (known && t.rdata != exp)
        err($sformatf("%s: read 0x%08x, expected 0x%08x", ctx, t.rdata, exp));
    endfunction

    // TBRE_START written: theory_of_operation.md "Revocation Engine".
    function void start_write(cms_txn_t t);
      logic [31:0] top_caps;
      int unsigned caps;
      if (!t.wdata[0])                                    cov.st_outcome = 3'd6;
      else if (sweep_active)                              cov.st_outcome = 3'd5;
      else if (!csr_num_known || !csr_base_known) begin
        err("TBRE_START with BASE/NUM unknown (written during a sweep): test sequencing error");
        return;
      end
      else if (csr_num == 0)                              cov.st_outcome = 3'd1;
      else if (csr_base < MainSramBase)                   cov.st_outcome = 3'd2;
      else if (!in_sram(csr_base) && !in_nvm(csr_base))   cov.st_outcome = 3'd3;
      else if (!ena_true(t.ena))                          cov.st_outcome = 3'd4;
      else                                                cov.st_outcome = 3'd0;

      if (cov.st_outcome != 3'd0) begin
        cov.st_clamped = 0;
        cov.st_nvm     = 0;
        cov.cms_tbre_start_cg.sample();
        // An ignored start sets start_err; a write of 0 is no start, and one while the engine is
        // active is dropped by TBRE_REGWEN.
        if (cov.st_outcome inside {3'd1, 3'd2, 3'd3, 3'd4}) begin
          n_ignored_starts++;
          start_err = 1;
        end
        if (!sweep_active) flush_tbre_early_reads("TBRE_START was ignored");
        return;
      end
      // A sweep that would reach past the top of its region ends there.
      top_caps = ((in_nvm(csr_base) ? NvmTop : MainSramTop) - csr_base) / 8;
      caps     = (csr_num > top_caps) ? top_caps : csr_num;
      cov.st_clamped = csr_num > top_caps;
      cov.st_nvm     = in_nvm(csr_base);
      cov.cms_tbre_start_cg.sample();

      sweep_active    = 1;
      sweep_base      = csr_base;
      sweep_caps      = caps;
      sweep_a_cycle   = t.a_cycle;
      sweep_fail_exp  = 0;
      sweep_fail_denied = 0;
      intr_clr_in_sweep      = 0;
      sweep_err_clr_in_sweep = 0;
      tbre_reads_seen = 0;
      tbre_prev_a     = 0;
      tbre_exp_reads.delete();
      for (int unsigned i = 0; i < caps; i++) begin
        tbre_exp_reads.push_back(csr_base + i * 8);
        tbre_exp_reads.push_back(csr_base + i * 8 + 4);
      end
      snap_revbm = srevbm;
      race_q.delete(); racy_revbm.delete(); sweep_dc.delete();
      sweep_keep.delete(); sweep_force0.delete();
      rd_lb.delete(); rd_p0.delete(); rd_p1.delete();
      snap_tag.delete();
      for (int unsigned i = 0; i < caps; i++) snap_tag[granule(csr_base + i * 8)] =
                                                get_tag(granule(csr_base + i * 8));
      // Writes answered after the sweep started but before this write's response was seen
      foreach (recent_stores[i]) begin
        if (recent_stores[i].d_cycle >= t.a_cycle &&
            in_sweep(recent_stores[i].g, sweep_base, sweep_caps))
          race_q[recent_stores[i].g].push_back(recent_stores[i]);
      end
      // While the sweep runs, a revocable tagged capability in range may be read either way.
      for (int unsigned i = 0; i < caps; i++) begin
        int unsigned g;
        logic [31:0] ga;
        ga = csr_base + i * 8;
        g  = granule(ga);
        if (get_tag(g)[1] && revocable(get_data(ga), get_data(ga + 4), 1))
          set_tag(g, get_tag(g) | Tag0);
      end
      n_sweeps++;
      cms_info("sb", $sformatf("sweep %0d: %0d capabilities from 0x%08x (TBRE_NUM_CAPS %0d)",
                               n_sweeps, caps, csr_base, csr_num));
      // Reads issued between the TBRE accepting this write and its response.
      while (tbre_early_reads.size() > 0) check_tbre_read(tbre_early_reads.pop_front());
    endfunction

    // Every read the TBRE issues on tbre_tl_h, in order.
    function void on_tbre_read(tbre_rd_t rd, tl_a_op_e op);
      if (op != Get) err($sformatf("TBRE issued %s to 0x%08x: the engine must never write memory",
                                   op.name(), rd.addr));
      if (!sweep_active) begin
        // Possibly ahead of the TBRE_START response; checked when start_write() runs.
        tbre_early_reads.push_back(rd);
        if (tbre_early_reads.size() > TbreEarlyMax)
          err($sformatf("TBRE read 0x%08x with no sweep started (%0d reads without a TBRE_START)",
                        rd.addr, tbre_early_reads.size()));
        return;
      end
      check_tbre_read(rd);
    endfunction

    function void check_tbre_read(tbre_rd_t rd);
      logic [31:0] exp;
      int unsigned g;
      if (tbre_exp_reads.size() == 0) begin
        err($sformatf("TBRE read 0x%08x with no sweep word outstanding", rd.addr));
        return;
      end
      exp = tbre_exp_reads.pop_front();
      if (rd.addr != exp) err($sformatf("TBRE read 0x%08x, expected 0x%08x", rd.addr, exp));
      // When the watch of the capability's core writes starts (sweep_done)
      g = granule(exp);
      if (!exp[2]) begin
        rd_lb[g] = (tbre_reads_seen == 0) ? sweep_a_cycle : tbre_prev_a + 1;
        rd_p0[g] = rd.p_cycle;
      end else begin
        rd_p1[g] = rd.p_cycle;
      end
      if (traced(exp)) trace($sformatf("TBRE read 0x%08x presented @%0d accepted @%0d", rd.addr,
                                       rd.p_cycle, rd.a_cycle));
      tbre_prev_a = rd.a_cycle;
      tbre_reads_seen++;
    endfunction

    // Reads held by on_tbre_read() that no sweep accounted for.
    function void flush_tbre_early_reads(string why);
      foreach (tbre_early_reads[i])
        err($sformatf("TBRE read 0x%08x with no sweep started (%s)", tbre_early_reads[i].addr, why));
      tbre_early_reads.delete();
    endfunction

    // A directed test declares one swept capability's outcome unpredictable (a fault on its read).
    function void sweep_dont_care(int unsigned g);
      sweep_dc[g] = 1;
      if (cm != null) cms_cmodel_pkg::cms_sweep_dont_care(cm, granule_addr(g));
    endfunction
    // ... or as kept (a read of either word answered with an error, or failing its checks: that
    // capability is not cleared, programmers_guide.md "Errors") or revoked (a failed bitmap
    // lookup).
    function void sweep_expect_kept(int unsigned g);
      sweep_keep[g] = 1;
      if (cm != null) cms_cmodel_pkg::cms_sweep_keep(cm, granule_addr(g));
    endfunction
    function void sweep_expect_revoked(int unsigned g);
      sweep_force0[g] = 1;
      if (cm != null) cms_cmodel_pkg::cms_sweep_revoke(cm, granule_addr(g));
    endfunction
    // ... and that a fault it injected ends the running sweep with TBRE_STATUS.sweep_err, so
    // TBRE_EPOCH does not count it. `denied`: the only faults are error responses to reads of the
    // swept memory (e.g. a read-protected NVM page), which raise no alert (for coverage; the test
    // checks the alert).
    function void sweep_expect_fail(bit denied = 1'b0);
      if (!sweep_active) err("sweep_expect_fail with no sweep running: test sequencing error");
      sweep_fail_denied = (sweep_fail_exp ? sweep_fail_denied : 1'b1) && denied;
      sweep_fail_exp    = 1;
      if (cm != null) cms_cmodel_pkg::cms_sweep_error(cm, int'(!denied));
    endfunction

    // The engine is seen inactive: the sweep is over. Every word must have been read; predict the
    // swept tags, the epoch, sweep_err and the interrupt.
    function void sweep_done(cms_txn_t t, string why);
      logic [31:0] ga, w0, w1;
      int unsigned g, b;
      bit          rv_old, rv_new, inr;
      logic [1:0]  s, s_eval;
      if (tbre_exp_reads.size() != 0)
        err($sformatf("%s with %0d of %0d sweep reads not issued", why,
                      tbre_exp_reads.size(), 2 * sweep_caps));
      for (int unsigned i = 0; i < sweep_caps; i++) begin
        ga = sweep_base + i * 8;
        g  = granule(ga);
        w0 = get_data(ga);
        w1 = get_data(ga + 4);
        s  = get_tag(g);
        rv_old = revocable(w0, w1, 1);
        rv_new = revocable(w0, w1, 0);
        // What the engine decides from the words it reads: revocable with the bitmap as it was
        // and as it is, cleared; with only one of them (a bitmap write during the sweep), either;
        // otherwise the tag is untouched. A base bit that changed during the sweep may have changed
        // back by its end (set, cleared, set again): the engine read it at some point in between,
        // so either outcome, even though the start and end values agree (note_revbm_change).
        s_eval = s;
        if (racy_revbm.exists(g))  s_eval = s | Tag0;
        else if (rv_old && rv_new) s_eval = Tag0;
        else if (rv_old || rv_new) s_eval = s | Tag0;
        if (sweep_force0.exists(g)) begin
          s = Tag0;
        end else if (sweep_keep.exists(g)) begin
          s = snap_tag.exists(g) ? snap_tag[g] : s;
        end else if (sweep_dc.exists(g)) begin
          s = TagAny;
        end else if (race_q.exists(g) && race_q[g].size() > 0) begin
          // theory_of_operation.md "Revocation Engine": a core write is watched from its
          // presentation until its answer, and a capability written from the presentation of
          // its lower word's read on is not cleared. So a write answered at or after that read
          // was seen presented leaves the tag it wrote; one answered before the read could be
          // presented was resolved by the engine like any capability; in between, either.
          bit         watched, around;
          store_rec_t last;
          watched = 0;
          around  = 0;
          last    = race_q[g][race_q[g].size() - 1];
          foreach (race_q[g][k]) begin
            store_rec_t r;
            r = race_q[g][k];
            // A write that writes no tag keeps it only if watched before the clear reached the RMW
            // filter (theory_of_operation.md); the clear follows the upper word's read, so one
            // accepted by then was. A later one falls to "around" below.
            if (rd_p0.exists(g) && r.d_cycle >= rd_p0[g] &&
                (!r.notag || (rd_p1.exists(g) && r.a_cycle <= rd_p1[g]))) watched = 1;
            else if (!rd_lb.exists(g) || r.d_cycle >= rd_lb[g]) around = 1;
            cov.w_window = (rd_lb.exists(g) && r.d_cycle < rd_lb[g]) ? 2'd0 :
                           (!rd_p0.exists(g) || r.d_cycle < rd_p0[g]) ? 2'd1 :
                           (!rd_p1.exists(g) || r.d_cycle < rd_p1[g]) ? 2'd2 : 2'd3;
            cov.w_upper  = r.addr[2];
            cov.w_rev    = r.nset[1] && revocable(r.w0, r.w1, 1);
            cov.w_nvm    = in_nvm(r.addr);
            cov.cms_sweep_snoop_cg.sample();
          end
          if (watched) begin
            s = last.nset;
            n_snoop_kept++;
            if (corrupt("race", $sformatf("granule 0x%08x", ga))) s = (s == Tag0) ? Tag1 : Tag0;
          end else if (around) begin
            s = last.nset | s_eval;
            n_snoop_around++;
          end else begin
            s = s_eval;
            n_snoop_before++;
          end
        end else begin
          s = s_eval;
          if (corrupt("sweep", $sformatf("granule 0x%08x", ga))) s = (s == Tag0) ? Tag1 : Tag0;
        end
        set_tag(g, s);
        if (traced(ga))
          trace($sformatf({"0x%08x sweep done: words 0x%08x 0x%08x, revocable at start %0d / end %0d,",
                           " base bit changed during the sweep %0d, racing core writes %0d,",
                           " expected tag %s"},
                          ga, w0, w1, rv_old, rv_new, racy_revbm.exists(g),
                          race_q.exists(g) ? race_q[g].size() : 0, tagset_str(s)));
        // Coverage of what the sweep saw
        cov.s_tag        = snap_tag.exists(g) ? snap_tag[g][1] : 1'b0;
        cov.s_sealing    = cap_is_sealing(w1);
        inr              = revbm_index(cap_base(w0, w1), b);
        cov.s_base_class = inr ? 2'd0 : (cap_base(w0, w1) < MainSramBase ? 2'd1 : 2'd2);
        cov.s_bit        = inr && revbm_bit(b, 1);
        cov.s_revoked    = cov.s_tag && rv_old;
        cov.s_exp        = cap_exp(w1);
        cov.s_corr       = ((w0 >> cap_exp(w1)) & 32'h1ff) < w1[8:0];
        cov.cms_sweep_cap_cg.sample();
        n_sweep_caps++;
        if (cov.s_revoked) n_sweep_revoked++;
      end
      // registers.md TBRE_EPOCH, TBRE_STATUS; programmers_guide.md: a sweep that ends with an
      // error sets sweep_err and is not counted; tbre_done is raised either way.
      if (sweep_fail_exp) sweep_err = sweep_err_clr_in_sweep ? TagAny : Tag1;
      else                epoch_count = epoch_count + 31'd1;
      intr_state = intr_clr_in_sweep ? TagAny : Tag1;
      cov.e_failed  = sweep_fail_exp;
      cov.e_denied  = sweep_fail_denied;
      cov.e_nvm     = in_nvm(sweep_base);
      cov.e_intr_en = intr_enable;
      cov.cms_tbre_end_cg.sample();
      sweep_active   = 0;
      sweep_fail_exp = 0;
      sweep_fail_denied = 0;
      cms_info("sb", $sformatf("sweep %0d done (%s; %0d revoked so far in this run, epoch %0d)",
                               n_sweeps, why, n_sweep_revoked, epoch_count));
    endfunction

    // The level of intr_tbre_done_o, as far as the model knows it (cms_tb checks it whenever the
    // CSR port has been quiet for a few cycles).
    function logic [1:0] intr_pin_expect();
      if (!intr_enable) return Tag0;
      return sweep_active ? (intr_state | Tag1) : intr_state;
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Meta SRAM model hooks
    ////////////////////////////////////////////////////////////////////////////////////////////

    // Every write the subsystem makes to a tag word changes exactly one bit (RMW: the write-back
    // is skipped when the tag already has its value, and only the addressed bit is modified).
    function void on_meta_write(logic [31:0] addr, logic [31:0] old_w, logic [31:0] new_w);
      int unsigned tg;
      if (trace_en) begin
        tg = granule(trace_ga);
        if (addr == tag_word_addr(tg) && old_w[tag_bit(tg)] != new_w[tag_bit(tg)])
          trace($sformatf("meta SRAM: tag of 0x%08x %0d -> %0d", granule_addr(tg),
                          old_w[tag_bit(tg)], new_w[tag_bit(tg)]));
      end
      if (addr >= MetaNvmTagBase && addr < MetaTop) begin
        n_meta_writes++;
        if ($countones(old_w ^ new_w) != 1)
          cms_error("meta", $sformatf("tag word 0x%08x written 0x%08x -> 0x%08x: %0d bits changed",
                                      addr, old_w, new_w, $countones(old_w ^ new_w)));
      end
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Backdoor comparison of the whole tag map, the bitmap and the data memory
    ////////////////////////////////////////////////////////////////////////////////////////////

    // skip: granules with a core write in flight (their meta update may precede the response).
    function void check_backdoor(bit skip [int unsigned], string why);
      logic [31:0] w;
      logic        obs;
      logic [1:0]  s;
      int unsigned bad;
      bad = 0;
      n_backdoor++;
      for (int unsigned g = 0; g < NumGranules; g++) begin
        if (skip.exists(g)) continue;
        w   = meta.read(tag_word_addr(g));
        obs = w[tag_bit(g)];
        s   = get_tag(g);
        if (!s[obs]) begin
          if (bad < 16)
            err($sformatf("%s: tag of 0x%08x is %b in the meta SRAM, expected %s", why,
                          granule_addr(g), obs, tagset_str(s)));
          bad++;
        end else begin
          set_tag(g, tag_exact(obs));
        end
      end
      if (bad > 16) err($sformatf("%s: %0d tag mismatches in all", why, bad));
      // The bitmap is written only through the windows
      for (int unsigned i = 0; i < RevbmBytes / 4; i++) begin
        w = meta.read(MetaRevbmBase + i * 4);
        if (w != get_revbm(i))
          err($sformatf("%s: bitmap word %0d is 0x%08x, expected 0x%08x", why, i, w, get_revbm(i)));
      end
    endfunction

    // Data memory: the subsystem forwards core writes unchanged and the TBRE never writes.
    function void check_data(string why);
      int unsigned bad;
      bad = 0;
      foreach (data.mem[a]) begin
        if (data.mem[a] != get_data(a)) begin
          if (bad < 8) err($sformatf("%s: data at 0x%08x is 0x%08x, expected 0x%08x", why, a,
                                     data.mem[a], get_data(a)));
          bad++;
        end
      end
      foreach (sdata[a]) begin
        if (!data.mem.exists(a) && sdata[a] != 0) begin
          if (bad < 8) err($sformatf("%s: data at 0x%08x never written, expected 0x%08x", why, a,
                                     sdata[a]));
          bad++;
        end
      end
    endfunction

    ////////////////////////////////////////////////////////////////////////////////////////////
    // Reset of the subsystem (the TB memories keep their content)
    ////////////////////////////////////////////////////////////////////////////////////////////

    // inflight: granules of core writes that were in flight, with the tag they carried.
    function void on_reset(logic inflight [int unsigned]);
      logic [31:0] ga;
      if (sweep_active) begin
        // Partly swept: every revocable capability in range may or may not have lost its tag.
        for (int unsigned i = 0; i < sweep_caps; i++) begin
          ga = sweep_base + i * 8;
          if (revocable(get_data(ga), get_data(ga + 4), 1) ||
              revocable(get_data(ga), get_data(ga + 4), 0))
            set_tag(granule(ga), get_tag(granule(ga)) | Tag0);
        end
      end
      foreach (inflight[g]) set_tag(g, get_tag(g) | tag_exact(inflight[g]));
      sweep_active = 0;
      sweep_fail_exp = 0;
      sweep_fail_denied = 0;
      tbre_exp_reads.delete();
      tbre_early_reads.delete();
      csr_base = 0; csr_num = 0; csr_base_known = 1; csr_num_known = 1;
      intr_enable = 0; intr_state = Tag0; intr_clr_in_sweep = 0;
      start_err = 0; sweep_err = Tag0; sweep_err_clr_in_sweep = 0; epoch_count = 0;
      alert_test_regwen = 1;
      wtrc_open = 0;
      exp_core_err = 0;
      core_err_any = 0;
      sticky_known = 0;
    endfunction

    function string stats();
      return $sformatf({"core=%0d tag_checks=%0d data_checks=%0d err_checks=%0d revbm=%0d csr=%0d ",
                        "sweeps=%0d swept_caps=%0d revocable=%0d ignored_starts=%0d ",
                        "meta_tag_writes=%0d backdoor_compares=%0d nvm_cap_stores=%0d ",
                        "nvm_cap_tagged=%0d snoop_kept=%0d snoop_before=%0d snoop_around=%0d"},
                       n_core, n_tag_checks, n_data_checks, n_err_checks, n_revbm, n_csr,
                       n_sweeps, n_sweep_caps, n_sweep_revoked, n_ignored_starts, n_meta_writes,
                       n_backdoor, n_nvm_cap, n_nvm_cap_tagged, n_snoop_kept, n_snoop_before,
                       n_snoop_around);
    endfunction
  endclass

endpackage
