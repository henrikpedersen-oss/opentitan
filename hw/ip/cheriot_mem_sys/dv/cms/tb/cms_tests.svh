// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Tests of the CHERIoT memory subsystem block-level bench. Included in cms_tb (they use its
// helpers). Every test leaves the checking to the scoreboard where it can, and adds explicit
// checks where the scoreboard cannot know the expectation (fault injection, CSR reset values).
// The testplan is hw/ip/cheriot_mem_sys/data/cheriot_mem_sys_cms_testplan.hjson.

// ---------------------------------------------------------------------------------------------
// Shared stimulus
// ---------------------------------------------------------------------------------------------

function automatic bit rbit();
  return 1'($urandom_range(1));
endfunction

task automatic cheriot_on();
  set_ena(prim_mubi_pkg::MuBi4True);
endtask

task automatic throttle(int unsigned max_inflight = 8);
  while (u_core.in_flight() > max_inflight) @(posedge clk);
endtask

function automatic logic [31:0] rand_granule(logic [31:0] base, logic [31:0] top);
  return base + 8 * $urandom_range((top - base) / 8 - 1);
endfunction

// A capability of a random kind with base `base`. kind: 0 memory RW, 1 read-only, 2 executable,
// 3 sealing, 4 no permissions, 5 sealed (memory, otype != 0).
task automatic make_kind_cap(logic [31:0] base, int unsigned kind, int unsigned e,
                             output logic [31:0] w0, output logic [31:0] w1);
  logic [5:0] p;
  case (kind)
    0: p = PermsMemRw;
    1: p = PermsMemRo;
    2: p = PermsExec;
    3: p = PermsSealing;
    4: p = PermsNone;
    default: p = PermsMemRw;
  endcase
  make_cap(base, e, p, w0, w1);
  if (kind == 5) w1[24:22] = 3'(1 + $urandom_range(6));
  else if (kind != 3) w1[24:22] = 3'b000;
endtask

// A heap base whose revocation bit is set (value 1) or clear (0) when the call returns.
task automatic heap_base(bit revoked, output logic [31:0] base);
  base = rand_granule(MainSramBase, MainSramTop);
  revbm_set_base(base, revoked);
endtask

// Fill [base, base + 8n) with capabilities of every class the sweep must tell apart, in either
// tagged region (in the NVM, a tagged capability is programmed first and then stored).
task automatic fill_caps(logic [31:0] base, int unsigned n);
  logic [31:0] w0, w1, cb;
  int unsigned cls;
  for (int unsigned i = 0; i < n; i++) begin
    cls = $urandom_range(9);
    case (cls)
      0: begin heap_base(1, cb); make_kind_cap(cb, 0, 0, w0, w1); end           // revoked
      1: begin heap_base(0, cb); make_kind_cap(cb, 0, 0, w0, w1); end           // live
      2: begin heap_base(1, cb); make_kind_cap(cb, 3, 0, w0, w1); end           // sealing: exempt
      3: begin heap_base(1, cb); make_kind_cap(cb, 5, 0, w0, w1); end           // sealed: revoked
      4: begin make_kind_cap(32'h0800_0000 + 8 * $urandom_range(255), 0, 0, w0, w1); end  // below
      5: begin make_kind_cap(NvmBase + 8 * $urandom_range(255), 0, 0, w0, w1); end         // above
      6: begin heap_base(1, cb); make_kind_cap(cb, $urandom_range(3), $urandom_range(3), w0, w1); end
      7: begin heap_base(1, cb); make_kind_cap(cb, 2, 0, w0, w1); end           // executable
      default: begin heap_base(1, cb); make_kind_cap(cb, 0, 0, w0, w1); end
    endcase
    // One in eight is data that merely looks like a revoked capability: tag 0.
    store_cap(base + 8 * i, w0, w1, ($urandom_range(7) != 0));
    throttle();
  end
  wait_quiet();
endtask

task automatic load_all_caps(logic [31:0] base, int unsigned n);
  logic t;
  for (int unsigned i = 0; i < n; i++) core_load_cap(base + 8 * i, t);
endtask

// ---------------------------------------------------------------------------------------------
// Tag filter
// ---------------------------------------------------------------------------------------------

task automatic t_smoke();
  logic [31:0] d, w0, w1;
  logic        t, e;
  // Reset values and quiet ePMP mode
  csr_read(CsrRegwen, d);
  if (d != 32'h1) cms_error("test", $sformatf("TBRE_REGWEN reset value 0x%08x", d));
  csr_read(CsrTbreStatus, d);
  if (d != 32'h0) cms_error("test", $sformatf("TBRE_STATUS reset value 0x%08x", d));
  csr_read(CsrTbreEpoch, d);
  if (d != 32'h0) cms_error("test", $sformatf("TBRE_EPOCH reset value 0x%08x", d));
  csr_read(CsrIntrState, d);
  if (d != 32'h0) cms_error("test", $sformatf("INTR_STATE reset value 0x%08x", d));
  csr_read(CsrIntrEnable, d);
  if (d != 32'h0) cms_error("test", $sformatf("INTR_ENABLE reset value 0x%08x", d));
  if (intr_tbre_done) cms_error("test", "intr_tbre_done_o high out of reset");
  csr_read(CsrTbreBase, d);
  csr_read(CsrTbreNum, d);
  make_cap(MainSramBase + 32'h100, 0, PermsMemRw, w0, w1);
  core_store_cap(MainSramBase, w0, w1, 1'b1);
  core_load_cap(MainSramBase, t);
  wait_quiet();
  if (u_meta.n_reads + u_meta.n_writes != 0)
    cms_error("test", "meta SRAM accessed in ePMP mode");
  if (t) cms_error("test", "tag returned in ePMP mode");
  // CHERIoT mode: tags stored, loaded, cleared by a data store
  cheriot_on();
  core_store_cap(MainSramBase + 8, w0, w1, 1'b1);
  wait_quiet();
  core_load_cap(MainSramBase + 8, t);
  if (!t) cms_error("test", "capability stored with tag 1 loaded with tag 0");
  core_store(MainSramBase + 12, 2'd2, 32'h1234_5678, e);
  core_load_cap(MainSramBase + 8, t);
  if (t) cms_error("test", "data store did not clear the tag");
  expect_no_alert("smoke");
endtask

task automatic t_tag_store_load();
  logic [31:0] base[3], top[3], a, w0, w1;
  logic        t;
  int unsigned m0;
  cheriot_on();
  randomize_timing(0);
  base = '{MainSramBase, NvmBase, UntaggedBase};
  top  = '{MainSramTop, NvmTop, DataErrBase};
  for (int r = 0; r < 3; r++) begin
    m0 = u_meta.n_reads + u_meta.n_writes;
    // Boundaries and random granules. In the NVM a tagged store is verified against the NVM
    // contents: half of them are programmed first and set the tag, the others are answered with
    // d_error and leave it.
    for (int unsigned i = 0; i < 80; i++) begin
      a = (i == 0) ? base[r] : (i == 1) ? top[r] - 8 : rand_granule(base[r], top[r]);
      make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
      store_cap(a, w0, w1, rbit(), rbit() || i < 2);
      throttle();
    end
    wait_quiet();
    for (int unsigned i = 0; i < 80; i++) core_load_cap(rand_granule(base[r], top[r]), t);
    core_load_cap(base[r], t);
    core_load_cap(top[r] - 8, t);
    if (r == 2 && u_meta.n_reads + u_meta.n_writes != m0)
      cms_error("test", "untagged memory accesses reached the meta SRAM");
  end
  // Just outside the tagged regions: no tag
  core_store_cap(MainSramTop, w0, w1, 1'b1);
  core_store_cap(MainSramBase - 8, w0, w1, 1'b1);
  core_load_cap(MainSramTop, t);
  core_load_cap(MainSramBase - 8, t);
  check_tags("tag_store_load");
endtask

task automatic t_tag_clear_subword();
  logic [31:0] g, w0, w1;
  logic        t, e;
  cheriot_on();
  for (int unsigned k = 0; k < 24; k++) begin
    g = MainSramBase + 32'h200 + 8 * 4 * k;
    make_cap(MainSramBase + 8 * k, 0, PermsMemRw, w0, w1);
    // The granule and both neighbours tagged
    core_store_cap(g - 8, w0, w1, 1'b1);
    core_store_cap(g,     w0, w1, 1'b1);
    core_store_cap(g + 8, w0, w1, 1'b1);
    wait_quiet();
    if (k < 8)       core_store(g + k, 2'd0, 32'h5a, e);                    // byte at each offset
    else if (k < 12) core_store(g + 2 * (k - 8), 2'd1, 32'h5a5a, e);        // half at each offset
    else if (k < 14) core_store(g + 4 * (k - 12), 2'd2, 32'h5a5a_5a5a, e);  // word
    else if (k < 20) begin                                                   // PutPartialData
      cms_txn_t r;
      logic [3:0] m;
      m = 4'($urandom_range(14, 1));
      run(PortCore, mk(PutPartialData, g + 4 * (k % 2), 2'd2, $urandom, 1'b0, m), r);
    end else begin
      // A store carrying tag 1 on one word only: the subsystem trusts the core's tag (the tag of
      // a capability is the tag of its last store), so the granule keeps tag 1 -- or gets it.
      cms_txn_t r;
      run(PortCore, mk(PutFullData, g + 4 * (k % 2), 2'd2, $urandom, 1'b1), r);
    end
    wait_quiet();
    core_load_cap(g - 8, t);
    if (!t) cms_error("test", $sformatf("store to 0x%08x cleared the tag of its neighbour below", g));
    core_load_cap(g + 8, t);
    if (!t) cms_error("test", $sformatf("store to 0x%08x cleared the tag of its neighbour above", g));
    core_load_cap(g, t);
    if (k < 20 && t) cms_error("test", $sformatf("sub-word/non-capability store kind %0d kept the tag", k));
  end
  check_tags("tag_clear_subword");
endtask

task automatic t_cap_load_hint();
  logic [31:0] a, w0, w1, d;
  logic        t, e;
  cheriot_on();
  for (int unsigned i = 0; i < 64; i++) begin
    make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
    core_store_cap(MainSramBase + 8 * i, w0, w1, 1'b1);
  end
  wait_quiet();
  for (int unsigned pass = 0; pass < 3; pass++) begin
    randomize_timing(pass == 0);
    // Back-to-back capability loads, both words outstanding
    for (int unsigned i = 0; i < 40; i++) begin
      a = MainSramBase + 8 * $urandom_range(63);
      core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1));
      core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1));
      // Interleaved non-capability loads: no tag, whatever the granule holds
      if (rbit()) core_issue(mk(Get, MainSramBase + 8 * $urandom_range(63), 2'd2));
      if (rbit()) core_issue(mk(Get, MainSramBase + $urandom_range(511), 2'd0));
      if ($urandom_range(3) == 0) core_issue(mk(Get, MainSramBase + 2 * $urandom_range(255), 2'd1));
      throttle(4);
    end
    wait_quiet();
  end
  // A hinted read of the first word alone, and of untagged memory
  core_load(MainSramBase, 2'd2, 1'b1, d, t, e);
  core_load(UntaggedBase, 2'd2, 1'b1, d, t, e);
  core_load_cap(UntaggedBase + 8, t);
  check_tags("cap_load_hint");
endtask

// ---------------------------------------------------------------------------------------------
// Access checkers and mode gating
// ---------------------------------------------------------------------------------------------

task automatic t_mode_gating();
  logic [31:0] w0, w1, d;
  logic        t, e;
  int unsigned m0, tr0;
  cms_txn_t    r;
  cheriot_on();
  for (int unsigned i = 0; i < 8; i++) begin
    make_cap(MainSramBase + 8 * i, 0, PermsMemRw, w0, w1);
    core_store_cap(MainSramBase + 8 * i, w0, w1, 1'(i % 2));
  end
  revbm_write(0, 32'hdead_beef);
  wait_quiet();
  // Every value other than MuBi4True, including MuBi4False and the invalid encodings
  for (int unsigned v = 0; v < 16; v++) begin
    if (4'(v) == 4'(prim_mubi_pkg::MuBi4True)) continue;
    set_ena(prim_mubi_pkg::mubi4_t'(v));
    m0  = u_meta.n_reads + u_meta.n_writes;
    tr0 = u_tbre_mem.n_reads;
    for (int unsigned i = 0; i < 8; i++) core_load_cap(MainSramBase + 8 * i, t);
    core_store_cap(MainSramBase + 8 * 8, w0, w1, 1'b1);  // no tag set outside CHERIoT mode
    core_store(MainSramBase + 8, 2'd2, 32'h1, e);        // no tag cleared either
    run(PortRevbm, mk(Get, MetaRevbmBase), r);
    run(PortRevbm, mk(PutFullData, MetaRevbmBase + 4, 2'd2, 32'h1), r);
    run(PortCoreRevbm, mk(Get, MetaRevbmBase), r);
    tbre_sweep(MainSramBase, 16, 1'b1);                  // ignored
    wait_quiet();
    wait_cycles(50);
    if (u_meta.n_reads + u_meta.n_writes != m0)
      cms_error("test", $sformatf("meta SRAM accessed with cheriot_ena_i = %0h", v));
    if (u_tbre_mem.n_reads != tr0)
      cms_error("test", $sformatf("sweep started with cheriot_ena_i = %0h", v));
  end
  cheriot_on();
  run(PortRevbm, mk(Get, MetaRevbmBase), r);
  for (int unsigned i = 0; i < 9; i++) core_load_cap(MainSramBase + 8 * i, t);
  check_tags("mode_gating");
  expect_no_alert("mode gating (software-reachable errors raise no alert)");
endtask

task automatic t_access_check();
  cms_txn_t    t, r;
  logic [31:0] a;
  logic [1:0]  sz;
  port_e       p;
  cheriot_on();
  randomize_timing(0);
  for (int unsigned i = 0; i < 600; i++) begin
    // Mostly CHERIoT mode; sometimes another encoding
    if ($urandom_range(15) == 0) set_ena(prim_mubi_pkg::mubi4_t'($urandom_range(15)));
    else if (ena != prim_mubi_pkg::MuBi4True) cheriot_on();
    p  = rbit() ? PortRevbm : PortCoreRevbm;
    sz = 2'($urandom_range(2));
    case ($urandom_range(5))
      0:       a = MetaBase - 32'h40 + $urandom_range(32'h3f);
      1:       a = MetaNvmTagBase + $urandom_range(MetaTop - MetaNvmTagBase + 32'h3f);
      2:       a = MetaNvmTagBase - 4;
      default: a = MetaRevbmBase + $urandom_range(RevbmBytes - 1);
    endcase
    t = mk(Get, a, sz);
    case ($urandom_range(3))
      0:       t = mk(PutFullData, a, sz, $urandom);
      1:       t = mk(PutPartialData, a, sz, $urandom, 1'b0,
                      size_mask(t.addr, sz) & 4'($urandom_range(15, 1)));
      default: ;
    endcase
    if (t.opcode == PutPartialData && t.mask == 4'h0) t.mask = size_mask(t.addr, sz);
    run(p, t, r);
  end
  cheriot_on();
  check_tags("access_check: the tag regions are unreachable from the windows");
  expect_no_alert("access check (software-reachable errors raise no alert)");
endtask

// ---------------------------------------------------------------------------------------------
// Read-modify-write filter
// ---------------------------------------------------------------------------------------------

task automatic t_rmw_same_word();
  logic [31:0] blk, a, w0, w1;
  logic        t;
  int unsigned w_before;
  cheriot_on();
  for (int unsigned rep = 0; rep < 4; rep++) begin
    randomize_timing(rep < 2);
    blk = (rep < 3) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
    for (int unsigned i = 0; i < 300; i++) begin
      a = blk + 8 * $urandom_range(31);
      case ($urandom_range(5))
        0, 1: begin
          make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
          store_cap(a, w0, w1, rbit(), rbit());
        end
        2: core_issue(mk(PutFullData, a + 4 * rbit(), 2'd2, $urandom));
        3: core_issue(mk(PutFullData, a + $urandom_range(7), 2'd0, $urandom));
        default: begin
          core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1));
          core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1));
        end
      endcase
      throttle(rep < 2 ? 4 : 8);
    end
    wait_quiet();
    check_tags($sformatf("rmw_same_word block 0x%08x", blk));
  end
  // The write-back is skipped when the tag already has its value.
  a = MainSramBase + 32'h40;
  make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
  core_store_cap(a, w0, w1, 1'b1);
  wait_quiet();
  w_before = u_meta.n_writes;
  core_store_cap(a, w0, w1, 1'b1);
  wait_quiet();
  if (u_meta.n_writes != w_before)
    cms_error("test", $sformatf("re-storing tag 1 wrote the meta SRAM %0d time(s)",
                                u_meta.n_writes - w_before));
  core_issue(mk(PutFullData, a, 2'd2, 32'h0));
  wait_quiet();
  if (u_meta.n_writes != w_before + 1)
    cms_error("test", $sformatf("clearing a tag wrote the meta SRAM %0d time(s), expected 1",
                                u_meta.n_writes - w_before));
  core_load_cap(a, t);
endtask

// The core and the TBRE update different bits of the same tag words at the same time: a lost
// update in the shared RMW filter shows as a wrong bit in the meta SRAM.
task automatic t_rmw_core_tbre_same_word();
  logic [31:0] blk, cb, w0, w1;
  bit          done;
  cheriot_on();
  for (int unsigned rep = 0; rep < 8; rep++) begin
    randomize_timing(1'(rep % 2));
    blk = MainSramBase + 256 * (1 + $urandom_range(SramCaps / 32 - 2));
    // Lower half: tagged, revoked; upper half: tagged, never revoked (base in NVM)
    for (int unsigned i = 0; i < 16; i++) begin
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, w0, w1);
      core_store_cap(blk + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    done = 0;
    fork
      begin
        tbre_sweep(blk, 16, 1'b1);
        done = 1;
      end
      begin
        while (!done) begin
          make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
          core_store_cap(blk + 128 + 8 * $urandom_range(15), w0, w1, $urandom_range(3) != 0);
          throttle(2);
        end
      end
    join
    wait_quiet();
    check_tags($sformatf("core and TBRE on tag word 0x%08x", blk));
  end
endtask

// ---------------------------------------------------------------------------------------------
// Revocation engine
// ---------------------------------------------------------------------------------------------

task automatic t_tbre_sweep();
  logic [31:0] b;
  int unsigned n;
  cheriot_on();
  randomize_timing(0);
  fill_caps(MainSramBase, SramCaps);
  // The whole SRAM, then random sub-ranges, then single capabilities at both ends
  tbre_sweep(MainSramBase, SramCaps);
  check_tags("whole-SRAM sweep");
  for (int unsigned i = 0; i < 6; i++) begin
    b = rand_granule(MainSramBase, MainSramTop);
    n = 1 + $urandom_range((MainSramTop - b) / 8 - 1);
    fill_caps(b, n);
    tbre_sweep(b, n);
    check_tags($sformatf("sweep of %0d from 0x%08x", n, b));
  end
  fill_caps(MainSramBase, 1);
  tbre_sweep(MainSramBase, 1);
  fill_caps(MainSramTop - 8, 1);
  tbre_sweep(MainSramTop - 8, 1);
  check_tags("single-capability sweeps");
  load_all_caps(MainSramBase, 128);
  // The NVM: the whole of it, a random sub-range, and single capabilities at both ends
  fill_caps(NvmBase, NvmCaps);
  tbre_sweep(NvmBase, NvmCaps);
  check_tags("whole-NVM sweep");
  b = rand_granule(NvmBase, NvmTop);
  n = 1 + $urandom_range((NvmTop - b) / 8 - 1);
  fill_caps(b, n);
  tbre_sweep(b, n);
  fill_caps(NvmBase, 1);
  tbre_sweep(NvmBase, 1);
  fill_caps(NvmTop - 8, 1);
  tbre_sweep(NvmTop - 8, 1);
  check_tags("NVM sweeps");
  load_all_caps(NvmBase, 64);
endtask

// Base decoding: every exponent, both corrections, bit selects at both ends of a bitmap word, and a
// set bit next to (not at) the base.
task automatic t_tbre_base_decode();
  logic [31:0] slot, base, w0, w1;
  int unsigned exps[16], e, k;
  cheriot_on();
  exps = '{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 24};
  slot = MainSramBase + SramBytes / 2;  // capabilities stored in the upper half
  k = 0;
  foreach (exps[i]) begin
    e = exps[i];
    for (int unsigned v = 0; v < 4; v++) begin
      if (e >= 14) base = MainSramBase;
      else         base = MainSramBase + ((32'($urandom_range(((SramBytes / 2) >> e) - 1))) << e);
      case (v)
        0: revbm_set_base(base, 1);                                    // own bit set
        1: begin revbm_set_base(base, 0); revbm_set_base(base + 8, 1); end  // only the next bit
        2: begin revbm_set_base(base, 0); if (base > MainSramBase) revbm_set_base(base - 8, 1); end
        default: revbm_set_base(base, rbit());
      endcase
      make_cap(base, e, PermsMemRw, w0, w1);
      core_store_cap(slot + 8 * k, w0, w1, 1'b1);
      k++;
      throttle();
    end
  end
  // Bit selects 0 and 31 of bitmap words, and the last heap granule
  for (int unsigned w = 0; w < 4; w++) begin
    base = MainSramBase + 256 * $urandom_range(RevbmBytes / 4 - 1) + (1'(w % 2) ? 31 * 8 : 0);
    revbm_set_base(base, w < 2);
    make_cap(base, 0, PermsMemRw, w0, w1);
    core_store_cap(slot + 8 * k, w0, w1, 1'b1);
    k++;
  end
  revbm_set_base(MainSramTop - 8, 1);
  make_cap(MainSramTop - 8, 0, PermsMemRw, w0, w1);
  core_store_cap(slot + 8 * k, w0, w1, 1'b1); k++;
  make_cap(MainSramTop, 0, PermsMemRw, w0, w1);     // first base past the bitmap: out of range
  core_store_cap(slot + 8 * k, w0, w1, 1'b1); k++;
  wait_quiet();
  tbre_sweep(slot, k);
  check_tags("base decode");
  load_all_caps(slot, k);
endtask

task automatic t_tbre_csr();
  logic [31:0] d, tr0;
  logic        e;
  cms_txn_t    r;
  cheriot_on();
  // Reset values
  csr_read(CsrRegwen, d);     if (d != 1) cms_error("test", "TBRE_REGWEN reset value");
  csr_read(CsrTbreStatus, d); if (d != 0) cms_error("test", "TBRE_STATUS reset value");
  csr_read(CsrTbreBase, d);   if (d != 0) cms_error("test", "TBRE_BASE_ADDR reset value");
  csr_read(CsrTbreNum, d);    if (d != 0) cms_error("test", "TBRE_NUM_CAPS reset value");
  csr_read(CsrTbreEpoch, d);  if (d != 0) cms_error("test", "TBRE_EPOCH reset value");
  csr_read(CsrTbreStart, d);
  csr_read(CsrAlertTest, d);  if (d != 32'h8000_0000) cms_error("test", "ALERT_TEST reset value");
  csr_read(CsrIntrTest, d);
  // Field widths (the scoreboard checks the read-back)
  csr_write(CsrTbreBase, 32'hffff_ffff, e); csr_read(CsrTbreBase, d);
  csr_write(CsrTbreNum, 32'hffff_ffff, e);  csr_read(CsrTbreNum, d);
  csr_write(CsrIntrEnable, 32'hffff_ffff, e); csr_read(CsrIntrEnable, d);
  csr_write(CsrIntrEnable, 32'h0, e);
  // Bus errors: writes that leave out a byte holding a field, unmapped offsets; writes to RO
  // registers (ignored)
  run(PortCsr, mk(PutPartialData, 32'(CsrTbreBase), 2'd2, 32'h0, 1'b0, 4'b0011), r);
  run(PortCsr, mk(PutPartialData, 32'(CsrTbreNum), 2'd1, 32'h0), r);
  run(PortCsr, mk(PutPartialData, 32'(CsrTbreStatus), 2'd2, 32'h300, 1'b0, 4'b0001), r);
  run(PortCsr, mk(PutPartialData, 32'(CsrTbreEpoch), 2'd2, 32'h0, 1'b0, 4'b0111), r);
  run(PortCsr, mk(PutPartialData, 32'(CsrIntrEnable), 2'd2, 32'h1, 1'b0, 4'b1110), r);
  // ALERT_TEST without the byte of regwen (bit 31): refused, so regwen stays set
  run(PortCsr, mk(PutPartialData, 32'(CsrAlertTest), 2'd2, 32'h0, 1'b0, 4'b0001), r);
  csr_read(CsrAlertTest, d);
  run(PortCsr, mk(Get, 32'h28), r);
  run(PortCsr, mk(PutFullData, 32'h3c, 2'd2, 32'h1), r);
  run(PortCsr, mk(PutFullData, 32'(CsrTbreStatus), 2'd2, 32'h1), r);
  run(PortCsr, mk(PutFullData, 32'(CsrTbreEpoch), 2'd2, 32'h5), r);
  run(PortCsr, mk(PutFullData, 32'(CsrRegwen), 2'd2, 32'h0), r);
  csr_read(CsrRegwen, d); if (d != 1) cms_error("test", "TBRE_REGWEN is writable");
  csr_read(CsrTbreEpoch, d); if (d != 0) cms_error("test", "TBRE_EPOCH is writable");
  fill_caps(MainSramBase, SramCaps);
  // Ignored starts: no sweep, busy stays low, start_err set (the scoreboard checks it on every
  // TBRE_STATUS read) until cleared
  tr0 = u_tbre_mem.n_reads;
  tbre_sweep(MainSramBase, 0);
  csr_read(CsrTbreStatus, d);
  if (d != 32'h100) cms_error("test", $sformatf("TBRE_STATUS 0x%08x after an ignored start", d));
  csr_write(CsrTbreStatus, 32'h100, e);
  csr_read(CsrTbreStatus, d); if (d != 0) cms_error("test", "TBRE_STATUS.start_err not cleared");
  tbre_sweep(MainSramBase - 8, 4);
  tbre_sweep(MainSramTop, 4);
  tbre_sweep(NvmBase - 8, 4);
  tbre_sweep(NvmTop, 4);
  tbre_sweep(32'h0, 4);
  set_ena(prim_mubi_pkg::MuBi4False);
  tbre_sweep(MainSramBase, 4);
  tbre_sweep(NvmBase, 4);
  cheriot_on();
  csr_write(CsrTbreStatus, 32'h100, e);
  csr_write(CsrTbreBase, MainSramBase, e);
  csr_write(CsrTbreNum, 4, e);
  csr_write(CsrTbreStart, 32'h0, e);
  csr_write(CsrTbreStart, 32'h2, e);
  wait_cycles(100);
  tbre_wait_idle();
  if (u_tbre_mem.n_reads != tr0) cms_error("test", "an ignored TBRE_START started a sweep");
  // A sweep past the top of its region ends there; TBRE_NUM_CAPS keeps the value written
  tbre_sweep(MainSramTop - 8 * 5, 1000);
  csr_read(CsrTbreNum, d); if (d != 1000) cms_error("test", "TBRE_NUM_CAPS changed by the clamp");
  tbre_sweep(MainSramBase + 8, 32'h7fff_ffff);
  fill_caps(NvmTop - 8 * 5, 5);
  tbre_sweep(NvmTop - 8 * 5, 1000);
  tbre_sweep(NvmBase + 8, 32'h7fff_ffff);
  check_tags("clamped sweeps");
  // While a long sweep runs: busy high, REGWEN low, epoch odd, the three registers locked
  fill_caps(MainSramBase, SramCaps);
  tbre_sweep(MainSramBase, SramCaps, 1'b0);
  csr_read(CsrTbreStatus, d); if (!d[0]) cms_error("test", "TBRE_STATUS.busy low during a sweep");
  csr_read(CsrRegwen, d);     if (d != 0) cms_error("test", "TBRE_REGWEN high during a sweep");
  csr_read(CsrTbreEpoch, d);  if (!d[0]) cms_error("test", "TBRE_EPOCH even during a sweep");
  csr_write(CsrTbreBase, MainSramBase + 32'h800, e);
  csr_write(CsrTbreNum, 3, e);
  csr_write(CsrTbreStart, 32'h1, e);                 // ignored: a second sweep would show
  csr_read(CsrTbreStatus, d);
  if (d != 32'h1) cms_error("test", $sformatf("TBRE_STATUS 0x%08x during a sweep (2)", d));
  tbre_wait_idle();
  csr_read(CsrTbreBase, d);
  if (d != MainSramBase) cms_error("test", $sformatf("TBRE_BASE_ADDR written while locked: 0x%08x", d));
  csr_read(CsrTbreNum, d);
  if (d != SramCaps) cms_error("test", $sformatf("TBRE_NUM_CAPS written while locked: %0d", d));
  csr_read(CsrRegwen, d); if (d != 1) cms_error("test", "TBRE_REGWEN low after the sweep");
  // The scoreboard marked BASE/NUM unknown; make them known again
  csr_write(CsrTbreBase, MainSramBase, e);
  csr_write(CsrTbreNum, SramCaps, e);
  wait_cycles(200);
  check_tags("locked registers");
  expect_no_alert("CSR test");
endtask

// Core traffic, and bitmap writes, while sweeps run.
task automatic t_tbre_concurrent();
  logic [31:0] b, a, w0, w1, cb;
  int unsigned n;
  bit          done;
  cheriot_on();
  for (int unsigned rep = 0; rep < 4; rep++) begin
    randomize_timing(1'(rep % 2));
    n = SramCaps / 4;
    b = MainSramBase + 8 * $urandom_range(SramCaps - n);
    fill_caps(b, n);
    done = 0;
    fork
      begin
        tbre_sweep(b, n, 1'b1);
        done = 1;
      end
      begin
        while (!done) begin
          a = rand_granule(MainSramBase, MainSramTop);
          case ($urandom_range(7))
            0, 1: begin core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1)); core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1)); end
            2: begin   // a live capability, anywhere (strict: it must keep its tag)
              make_cap(NvmBase + 8 * $urandom_range(NvmCaps - 1), 0, PermsMemRw, w0, w1);
              core_store_cap(a, w0, w1, 1'b1);
            end
            3: core_issue(mk(PutFullData, a + $urandom_range(7), 2'd0, $urandom));
            4: core_issue(mk(Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2'd2, 32'h0, 1'b1));
            5: begin heap_base(rbit(), cb); end   // bitmap write during the sweep
            default: core_issue(mk(Get, UntaggedBase + 4 * $urandom_range(255), 2'd2));
          endcase
          throttle(4);
        end
      end
    join
    wait_quiet();
    check_tags($sformatf("concurrent sweep %0d", rep));
  end
endtask

// A fresh capability stored over a revoked one while the engine is between reading it and clearing
// its tag. The fresh capability is live (its base has no revocation bit); it must keep its tag: the
// subsystem watches core writes to a capability from the engine's read of its lower word on
// (theory_of_operation.md "Revocation Engine"), and the scoreboard expects the tag exactly.
// Two triggers, each 0-23 cycles before the store: the engine's read response of a word (most
// stores land after the clear), and the engine presenting the read of a word -- the window the watch
// exists for, between the read and the clear. Without the second, a watch that is off went unseen
// (0 of 48 racing stores lost their tag with the snoop_off mutation, 2026-10-11 and 2026-10-14).
task automatic t_tbre_store_race();
  logic [31:0] g, cb, w0, w1, f0, f1, trig;
  logic        t;
  int unsigned lost, hits;
  longint unsigned t0;
  bit          started, on_pres;
  cheriot_on();
  randomize_timing(1);
  lost = 0;
  hits = 0;
  for (int unsigned mode = 0; mode < 2; mode++) begin
    on_pres = mode == 1;
    for (int unsigned word = 0; word < 2; word++) begin
      for (int unsigned d = 0; d < 24; d++) begin
        g = MainSramBase + 8 * (64 + 48 * mode + 24 * word + d);
        heap_base(1, cb);
        make_cap(cb, 0, PermsMemRw, w0, w1);
        core_store_cap(g, w0, w1, 1'b1);
        make_cap(NvmBase + 8 * d, 0, PermsMemRw, f0, f1);
        wait_quiet();
        trig = g + 4 * word;
        u_tbre_mem.pres_seen.delete(trig);
        // Watch from before START: a one-capability sweep can read both words before the START
        // write's response is back, which left last_rsp_addr at g + 4 and word 0 never matched.
        started = 0;
        fork begin tbre_sweep(g, 1, 1'b0); started = 1; end join_none
        t0 = cms_cycle;
        // Trigger on the engine presenting the read of that word, or on its read response (!==: X
        // before the first response)
        if (on_pres)
          while (!u_tbre_mem.pres_seen.exists(trig) && cms_cycle - t0 < 2000) @(posedge clk);
        else
          while (u_tbre_mem.last_rsp_addr !== trig && cms_cycle - t0 < 2000) @(posedge clk);
        if (cms_cycle - t0 >= 2000)
          cms_error("test", "store race: the engine never read the capability");
        wait_cycles(d);
        core_store_cap(g, f0, f1, 1'b1);
        wait (started);
        tbre_wait_idle();
        wait_quiet();
        hits++;
        core_load_cap(g, t);
        if (!t) begin
          lost++;
          cms_info("test", $sformatf({"store race: live capability at 0x%08x lost its tag ",
                                      "(store %0d cycles after the engine %s word %0d)"}, g, d,
                                     on_pres ? "presented the read of" : "read", word));
        end
      end
    end
  end
  cms_info("test", $sformatf("store race: %0d of %0d racing stores lost their tag", lost, hits));
  check_tags("store race");
endtask

// The watch of core writes during a sweep (theory_of_operation.md "Revocation Engine"): a core
// write to a capability answered at or after the engine presents the read of its lower word keeps
// the tag it wrote, also when what it writes is itself a revoked capability; one answered before
// that is resolved by the engine like any capability. Writes at random points of sweeps over
// revoked capabilities: capability stores of a revoked capability, tag-1 stores of only the upper
// or only the lower word, data stores; in the SRAM and in the NVM, where the capability store's
// data differs from the NVM's, so it is answered with d_error and writes nothing. The scoreboard
// times each write against the engine's reads it saw on tbre_tl_h and expects the tag exactly,
// except for a write answered in the cycles where the engine may or may not have presented the
// read yet.
task automatic t_tbre_snoop_window();
  logic [31:0] b, g, cb, w0, w1, f0, f1;
  logic        t;
  int unsigned k, n, kept_nvm, failed_nvm;
  bit          nvm;
  cheriot_on();
  kept_nvm   = 0;
  failed_nvm = 0;
  for (int unsigned rep = 0; rep < 64; rep++) begin
    randomize_timing(rep % 3 == 0);
    nvm = rep % 4 == 3;
    n   = 2 + $urandom_range(6);
    b   = nvm ? NvmBase + 8 * $urandom_range(NvmCaps - n)
              : MainSramBase + 8 * $urandom_range(SramCaps - n);
    // Revoked, tagged capabilities
    for (int unsigned i = 0; i < n; i++) begin
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, w0, w1);
      store_cap(b + 8 * i, w0, w1, 1'b1);
      throttle(2);
    end
    wait_quiet();
    k = $urandom_range(n - 1);
    g = b + 8 * k;
    heap_base(1, cb);
    make_cap(cb, 0, PermsMemRw, f0, f1);   // revoked as well
    tbre_sweep(b, n, 1'b0);
    wait_cycles($urandom_range(8 * k + 12));
    if (nvm) begin
      core_store_cap(g, f0, f1, 1'b1);   // differs from the NVM: d_error, nothing written
      failed_nvm++;
    end else begin
      case ($urandom_range(3))
        0:       core_store_cap(g, f0, f1, 1'b1);
        1:       core_issue(mk(PutFullData, g + 4, 2'd2, f1, 1'b1));   // upper word only, tag 1
        2:       core_issue(mk(PutFullData, g, 2'd2, f0, 1'b1));       // lower word only, tag 1
        default: core_issue(mk(PutFullData, g + 4 * rbit(), 2'd2, $urandom));
      endcase
    end
    tbre_wait_idle();
    wait_quiet();
    check_tags($sformatf("snoop window, sweep %0d", rep));
    if (nvm) begin
      core_load_cap(g, t);
      if (t) kept_nvm++;
    end
  end
  // Not judged here (the scoreboard does, from the document): a capability store to the NVM
  // that fails writes nothing, yet as a watched write it stops the clear.
  cms_info("test", $sformatf({"snoop window: %0d failed capability store(s) to the NVM, %0d ",
                              "revoked capability(ies) there still tagged after the sweep"},
                             failed_nvm, kept_nvm));
endtask

// ---------------------------------------------------------------------------------------------
// Epoch, status and interrupt (registers.md, programmers_guide.md)
// ---------------------------------------------------------------------------------------------

// TBRE_EPOCH is odd from a taken start until the engine is inactive and counts the sweeps that
// ended without an error; ignored starts and starts while active leave it; start_err and
// sweep_err stay set until written 1. The scoreboard checks every read exactly; the test checks
// the values it knows as well.
task automatic t_tbre_epoch();
  logic [31:0] d, ep, b, a, cb, w0, w1;
  logic        e;
  int unsigned n, k;
  cheriot_on();
  randomize_timing(0);
  fill_caps(MainSramBase, 128);
  fill_caps(NvmBase, 64);
  ep = 0;
  for (int unsigned i = 0; i < 9; i++) begin
    // Sweeps of either region, waited for by polling busy, by the epoch, or by the interrupt
    b = rbit() ? MainSramBase + 8 * $urandom_range(96) : NvmBase + 8 * $urandom_range(32);
    n = 1 + $urandom_range(31);
    tbre_sweep(b, n, 1'b0);
    csr_read(CsrTbreEpoch, d);   // odd, or already the next even value
    case (i % 3)
      0: tbre_wait_idle();
      1: for (int unsigned p = 0; p < 100000 && d[0]; p++) csr_read(CsrTbreEpoch, d);
      default: tbre_wait_intr();
    endcase
    csr_read(CsrTbreEpoch, d);
    ep += 2;
    if (d != ep) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x after %0d sweeps", d, i + 1));
  end
  check_tags("epoch sweeps");
  // A start while the engine is active and ignored starts leave the epoch alone
  tbre_sweep(MainSramBase, SramCaps, 1'b0);
  csr_write(CsrTbreStart, 32'h1, e);
  csr_read(CsrTbreEpoch, d);
  if (d != ep + 1) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x during a sweep, expected 0x%08x",
                                               d, ep + 1));
  tbre_wait_idle();
  ep += 2;
  tbre_sweep(MainSramBase, 0);
  tbre_sweep(NvmTop, 4);
  set_ena(prim_mubi_pkg::MuBi4False);
  tbre_sweep(MainSramBase, 4);
  cheriot_on();
  csr_read(CsrTbreEpoch, d);
  if (d != ep) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x after ignored starts", d));
  csr_read(CsrTbreStatus, d);
  if (d != 32'h100) cms_error("test", $sformatf("TBRE_STATUS 0x%08x after ignored starts", d));
  csr_write(CsrTbreStatus, 32'h1 << StatusStartErr, e);
  // A sweep with an error is not counted: the epoch goes back to its even value. The error is an
  // error response to one of the engine's reads, which fails the sweep without an alert.
  b = MainSramBase + 32'h800;
  for (int unsigned i = 0; i < 4; i++) begin
    heap_base(1, cb);
    make_cap(cb, 0, PermsMemRw, w0, w1);
    core_store_cap(b + 8 * i, w0, w1, 1'b1);
  end
  wait_quiet();
  k = 1 + $urandom_range(2);
  a = b + 8 * k + 4;
  u_tbre_mem.inject(InjErr, a, a + 4, 1, 1);
  tbre_sweep(b, 4, 1'b0);
  sb.sweep_expect_fail(1'b1);
  sb.sweep_expect_kept(granule(b + 8 * k));
  tbre_wait_idle();
  csr_read(CsrTbreEpoch, d);
  if (d != ep) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x after a failed sweep, expected 0x%08x",
                                           d, ep));
  csr_read(CsrTbreStatus, d);
  if (d != 32'h200) cms_error("test", $sformatf("TBRE_STATUS 0x%08x after a failed sweep", d));
  wait_cycles(200);
  expect_no_alert("error response to a TBRE read");
  // sweep_err stays set over a sweep without an error, which counts again
  tbre_sweep(MainSramBase, 16);
  ep += 2;
  csr_read(CsrTbreEpoch, d);
  if (d != ep) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x after a good sweep, expected 0x%08x",
                                           d, ep));
  csr_read(CsrTbreStatus, d);
  if (d != 32'h200) cms_error("test", "TBRE_STATUS.sweep_err not sticky");
  csr_write(CsrTbreStatus, 32'h1 << StatusSweepErr, e);
  csr_read(CsrTbreStatus, d);
  if (d != 32'h0) cms_error("test", "TBRE_STATUS.sweep_err not cleared");
  check_tags("epoch");
  do_reset();
  csr_read(CsrTbreEpoch, d);
  if (d != 0) cms_error("test", $sformatf("TBRE_EPOCH 0x%08x after reset", d));
endtask

task automatic wait_intr_pin(bit level, int unsigned max_cycles, string why);
  for (int unsigned i = 0; i < max_cycles && intr_tbre_done != level; i++) @(posedge clk);
  if (intr_tbre_done != level)
    cms_error("test", $sformatf("%s: intr_tbre_done_o not %b after %0d cycles", why, level,
                                max_cycles));
endtask

// The tbre_done interrupt: raised when the engine stops being active (every capability of the
// sweep resolved), a level until INTR_STATE is written 1, masked by INTR_ENABLE, forced by
// INTR_TEST, not raised by an ignored start; raised by a sweep that ends with an error as well.
// cms_tb compares the pin with the model throughout.
// "Every tag is final when it is raised" is judged when the scoreboard sees the engine idle, at the
// TBRE_STATUS read that follows the interrupt. Each swept range therefore ends with a revoked, tagged
// capability, whose clear is the last thing the engine does, and the meta SRAM is slow, so the clear
// (a read and a write of a meta word) takes longer than that CSR read: an interrupt raised before the
// clears are answered then leaves a set tag behind at the check. With a one-cycle meta SRAM the
// clears always beat the read, and an interrupt raised at the last read went unseen
// (tbre_done_early mutation, 2026-10-11 and 2026-10-14).
task automatic revoked_cap_at(logic [31:0] addr);
  logic [31:0] cb, w0, w1;
  heap_base(1, cb);
  make_cap(cb, 0, PermsMemRw, w0, w1);
  store_cap(addr, w0, w1, 1'b1);
  wait_quiet();
endtask

task automatic t_tbre_intr();
  logic [31:0] d, a, b, cb, w0, w1;
  logic        e;
  int unsigned n;
  cheriot_on();
  u_meta.a_ready_pct = 50;
  u_meta.lat_min     = 4;
  u_meta.lat_max     = 12;
  fill_caps(MainSramBase, 64);
  fill_caps(NvmBase, 32);
  revoked_cap_at(MainSramBase + 8 * 63);
  // Enabled: every tag is final when it is raised
  csr_write(CsrIntrEnable, 32'h1, e);
  tbre_sweep(MainSramBase, 64, 1'b0);
  wait_intr_pin(1, 200000, "sweep with the interrupt enabled");
  csr_read(CsrTbreStatus, d);
  if (d[StatusBusy]) cms_error("test", "TBRE_STATUS.busy high at the tbre_done interrupt");
  check_tags("at the tbre_done interrupt");
  csr_read(CsrIntrState, d);
  if (d != 32'h1) cms_error("test", $sformatf("INTR_STATE 0x%08x at the interrupt", d));
  // A level: it stays until acknowledged
  wait_cycles(200);
  if (!intr_tbre_done) cms_error("test", "intr_tbre_done_o dropped before it was acknowledged");
  csr_write(CsrIntrState, 32'h1, e);
  wait_intr_pin(0, 4, "INTR_STATE written 1");
  // Masked: the state is set, the pin stays low until enabled
  csr_write(CsrIntrEnable, 32'h0, e);
  tbre_sweep(NvmBase, 32);
  csr_read(CsrIntrState, d);
  if (d != 32'h1) cms_error("test", "INTR_STATE not set by a sweep with the interrupt masked");
  if (intr_tbre_done) cms_error("test", "intr_tbre_done_o high with INTR_ENABLE 0");
  csr_write(CsrIntrEnable, 32'h1, e);
  wait_intr_pin(1, 4, "INTR_ENABLE written 1 with INTR_STATE set");
  csr_write(CsrIntrState, 32'h1, e);
  // INTR_TEST forces it; writing 0 does nothing
  csr_write(CsrIntrTest, 32'h1, e);
  wait_intr_pin(1, 4, "INTR_TEST written 1");
  csr_read(CsrIntrState, d);
  if (d != 32'h1) cms_error("test", "INTR_STATE not set by INTR_TEST");
  csr_write(CsrIntrState, 32'h1, e);
  csr_write(CsrIntrTest, 32'h0, e);
  // Ignored starts raise none
  tbre_sweep(MainSramBase, 0);
  tbre_sweep(MainSramTop, 4);
  wait_cycles(100);
  if (intr_tbre_done) cms_error("test", "intr_tbre_done_o raised by an ignored start");
  csr_read(CsrIntrState, d);
  if (d != 32'h0) cms_error("test", "INTR_STATE set by an ignored start");
  csr_write(CsrTbreStatus, 32'h1 << StatusStartErr, e);
  // Acknowledged while the sweep runs: raised at its end
  tbre_sweep(MainSramBase, SramCaps, 1'b0);
  csr_write(CsrIntrState, 32'h1, e);
  tbre_wait_intr();
  // Sweeps of both regions waited for by the interrupt
  for (int unsigned i = 0; i < 6; i++) begin
    b = rbit() ? MainSramBase + 8 * $urandom_range(32) : NvmBase + 8 * $urandom_range(16);
    n = 1 + $urandom_range(15);
    revoked_cap_at(b + 8 * (n - 1));
    tbre_sweep(b, n, 1'b0);
    tbre_wait_intr();
    check_tags($sformatf("sweep %0d by interrupt", i));
  end
  // A sweep that ends with an error raises it too
  b = MainSramBase + 32'h800;
  for (int unsigned i = 0; i < 4; i++) begin
    heap_base(1, cb);
    make_cap(cb, 0, PermsMemRw, w0, w1);
    core_store_cap(b + 8 * i, w0, w1, 1'b1);
  end
  wait_quiet();
  a = b + 8 * 2 + 4;
  u_tbre_mem.inject(InjErr, a, a + 4, 1, 1);
  tbre_sweep(b, 4, 1'b0);
  sb.sweep_expect_fail(1'b1);
  sb.sweep_expect_kept(granule(b + 8 * 2));
  tbre_wait_intr();
  wait_cycles(200);
  expect_no_alert("error response to a TBRE read");
  check_tags("failed sweep by interrupt");
  do_reset();
  wait_cycles(4);
  if (intr_tbre_done) cms_error("test", "intr_tbre_done_o high after reset");
endtask

// ---------------------------------------------------------------------------------------------
// Capability stores to the NVM (theory_of_operation.md "Write-to-Read-and-Compare Filter";
// programmers_guide.md "Storing Capabilities in the NVM")
// ---------------------------------------------------------------------------------------------

// A capability store to the NVM, both words, waiting for both answers.
task automatic nvm_cap_store_wait(logic [31:0] g, logic [31:0] w0, logic [31:0] w1,
                                  output logic e0, output logic e1);
  cms_txn_t    t;
  int unsigned id0, id1;
  longint unsigned t0;
  id0 = u_core.issue(mk(PutFullData, {g[31:3], 3'b000}, 2'd2, w0, 1'b1));
  t = mk(PutFullData, {g[31:3], 3'b100}, 2'd2, w1, 1'b1);
  t.hold_after = 1'b1;
  id1 = u_core.issue(t);
  t0 = cms_cycle;
  while (!(u_core.rsp_by_id.exists(id0) && u_core.rsp_by_id.exists(id1))) begin
    @(posedge clk);
    if (cms_cycle - t0 > 20000) begin
      cms_error("test", $sformatf("no response to the capability store to 0x%08x", g));
      return;
    end
  end
  e0 = u_core.rsp_by_id[id0].err;
  e1 = u_core.rsp_by_id[id1].err;
  u_core.rsp_by_id.delete(id0);
  u_core.rsp_by_id.delete(id1);
  repeat (2) @(posedge clk);
endtask

// A capability store sets the tag if the NVM holds both of its words, and is otherwise answered
// with d_error (W0 if its word differs, W1 if either does) and leaves the tag as it was; it never
// writes data. A plain store is refused by the NVM and clears the tag. Programming the NVM leaves
// the tag. Outside CHERIoT mode a tagged store is a plain store. Then, one per reset, what the
// core does not produce: a partial capability store and a W1 with no W0, which also raise
// fatal_fault; and a request presented between a verified W1 and its tag write, which raises
// fatal_fault and changes no answer (theory_of_operation.md "Write-to-Read-and-Compare Filter").
task automatic t_nvm_cap_store();
  logic [31:0] g, w0, w1, x0, x1;
  logic        t, e0, e1;
  bit          done;
  cms_txn_t    r;
  int unsigned id1;
  cheriot_on();
  // Directed: the answer of each word
  g = NvmBase + 32'h100;
  make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
  nvm_program(g, w0, w1, done);
  nvm_cap_store_wait(g, w0, w1, e0, e1);
  if (e0 || e1) cms_error("test", $sformatf("matching capability store answered d_error %b%b", e0, e1));
  core_load_cap(g, t);
  if (!t) cms_error("test", "matching capability store to the NVM did not set the tag");
  nvm_cap_store_wait(g, w0 ^ 32'h1, w1, e0, e1);
  if (!e0 || !e1) cms_error("test", $sformatf("W0 differs: d_error %b%b, expected 11", e0, e1));
  nvm_cap_store_wait(g, w0, w1 ^ 32'h8000_0000, e0, e1);
  if (e0 || !e1) cms_error("test", $sformatf("W1 differs: d_error %b%b, expected 01", e0, e1));
  core_load_cap(g, t);
  if (!t) cms_error("test", "failed capability stores changed the tag");
  nvm_program(g, ~w0, ~w1, done);    // the NVM controller leaves the tag
  core_load_cap(g, t);
  if (!t) cms_error("test", "programming the NVM cleared the tag");
  nvm_program(g, w0, w1, done);
  run(PortCore, mk(PutFullData, g + 4, 2'd2, w1), r);   // a plain store: refused, tag cleared
  if (!r.err) cms_error("test", "a plain store to the NVM was not refused");
  core_load_cap(g, t);
  if (t) cms_error("test", "a refused plain store to the NVM kept the tag");
  nvm_cap_store_wait(g, ~w0, w1, e0, e1);
  core_load_cap(g, t);
  if (t) cms_error("test", "a failed capability store set the tag");
  set_ena(prim_mubi_pkg::MuBi4False);
  nvm_cap_store_wait(g, w0, w1, e0, e1);   // a plain store: refused, no lookup
  if (!e0 || !e1) cms_error("test", "a tagged store to the NVM in ePMP mode was not refused");
  cheriot_on();
  core_load_cap(g, t);
  if (t) cms_error("test", "a tagged store to the NVM in ePMP mode set the tag");
  // The NVM read of a matching W0 fails: W0 and W1 answered with d_error, the tag left, no alert
  nvm_program(g, w0, w1, done);
  u_dmem.inject(InjErr, g, g + 4, 1, 1);
  sb.exp_core_err = 2; sb.err_tag_effect = 0;
  nvm_cap_store_wait(g, w0, w1, e0, e1);
  sb.err_tag_effect = 2;
  if (!e0 || !e1)
    cms_error("test", $sformatf("W0's NVM read failed: d_error %b%b, expected 11", e0, e1));
  core_load_cap(g, t);
  if (t) cms_error("test", "a capability store whose NVM read failed set the tag");
  // Random: programmed or not, matching or differing in either word, plain stores and loads
  for (int unsigned rep = 0; rep < 3; rep++) begin
    randomize_timing(rep == 0);
    for (int unsigned i = 0; i < 96; i++) begin
      g = rand_granule(NvmBase, NvmTop);
      make_cap(MainSramBase + 8 * $urandom_range(SramCaps - 1), 0, PermsMemRw, w0, w1);
      x0 = w0;
      x1 = w1;
      case ($urandom_range(9))
        0: x0 = w0 ^ (32'h1 << $urandom_range(31));
        1: x1 = w1 ^ (32'h1 << $urandom_range(31));
        2: begin x0 = ~w0; x1 = ~w1; end
        default: ;
      endcase
      case ($urandom_range(9))
        0:       core_issue(mk(PutFullData, g + 4 * rbit(), 2'd2, $urandom));
        1:       core_issue(mk(PutFullData, g + $urandom_range(7), 2'd0, $urandom));
        2, 3:    begin
          core_issue(mk(Get, g, 2'd2, 32'h0, 1'b1));
          core_issue(mk(Get, g + 4, 2'd2, 32'h0, 1'b1));
        end
        4:       core_store_cap(g, w0, w1, 1'b1);   // whatever the NVM holds
        default: begin
          nvm_program(g, x0, x1, done);
          core_store_cap(g, w0, w1, 1'b1);
        end
      endcase
      throttle(4);
    end
    wait_quiet();
    check_tags($sformatf("NVM capability stores, pass %0d", rep));
    load_all_caps(NvmBase, 32);
  end
  expect_no_alert("capability stores to the NVM");
  // What the core does not produce, one per reset: d_error, the tag left as it was, fatal_fault
  for (int unsigned c = 0; c < 4; c++) begin
    g = NvmBase + 32'h200 + 8 * c;
    make_cap(MainSramBase, 0, PermsMemRw, w0, w1);
    nvm_program(g, w0, w1, done);
    nvm_cap_store_wait(g, w0, w1, e0, e1);
    expect_no_alert("before the fault");
    case (c)
      0: begin   // W0 a PutPartialData
        core_issue(mk(PutPartialData, g, 2'd2, w0, 1'b1, 4'b0111));
        run(PortCore, mk(PutFullData, g + 4, 2'd2, w1, 1'b1), r);
      end
      1: begin   // W0 a halfword
        core_issue(mk(PutFullData, g, 2'd1, w0, 1'b1));
        run(PortCore, mk(PutFullData, g + 4, 2'd2, w1, 1'b1), r);
      end
      2: run(PortCore, mk(PutFullData, g + 4, 2'd2, w1, 1'b1), r);   // W1 alone
      default: begin
        // A verified capability store whose W1 is not held: a load of untagged memory is
        // presented once W0 is answered, while W1 waits for its tag write, which the meta SRAM
        // holds back for 100 cycles. The load could otherwise reach the meta port first; the
        // WTRC flags it as soon as it is presented. Every answer is the usual one (the
        // scoreboard's): W1 sets the tag, cleared first by a plain store, and the load returns
        // its data.
        run(PortCore, mk(PutFullData, g + 4, 2'd2, w1), r);
        wait_quiet();
        wtrc_assertions(1'b0);
        u_meta.a_ready_pct = 0;
        core_issue(mk(PutFullData, g, 2'd2, w0, 1'b1));
        id1 = u_core.issue(mk(PutFullData, g + 4, 2'd2, w1, 1'b1));
        core_issue(mk(Get, UntaggedBase + 32'h40, 2'd2));
        wait_cycles(100);
        u_meta.a_ready_pct = 100;
        wait_core_quiet();
        wtrc_assertions(1'b1);
        if (!u_core.rsp_by_id.exists(id1)) begin
          cms_error("test", "fault case 3: W1 not answered");
        end else begin
          r = u_core.rsp_by_id[id1];
          u_core.rsp_by_id.delete(id1);
        end
        core_load_cap(g, t);
        if (!t) cms_error("test", "fault case 3: the verified capability store set no tag");
      end
    endcase
    if (c < 3 && !r.err)
      cms_error("test", $sformatf("fault case %0d: W1 not answered with d_error", c));
    if (c == 3 && r.err) cms_error("test", "fault case 3: verified W1 answered with d_error");
    expect_alert($sformatf("capability store to the NVM the core cannot produce, case %0d", c));
    wait_quiet();
    check_tags("NVM capability store fault");
    do_reset();
  end
endtask

// ---------------------------------------------------------------------------------------------
// Error handling (theory_of_operation.md "Error Handling")
// ---------------------------------------------------------------------------------------------

// After a sweep with an injected fault (the only one since reset): TBRE_STATUS.sweep_err set,
// TBRE_EPOCH not advanced, and sweep_err cleared by writing 1 to it (the scoreboard checks every
// read as well).
task automatic check_failed_sweep(string why);
  logic [31:0] d;
  logic        e;
  csr_read(CsrTbreStatus, d);
  if (!d[StatusSweepErr]) cms_error("test", $sformatf("%s: TBRE_STATUS.sweep_err not set", why));
  csr_read(CsrTbreEpoch, d);
  if (d != 0) cms_error("test", $sformatf("%s: TBRE_EPOCH 0x%08x, the failed sweep counted", why, d));
  csr_write(CsrTbreStatus, 32'h1 << StatusSweepErr, e);
  csr_read(CsrTbreStatus, d);
  if (d[StatusSweepErr]) cms_error("test", $sformatf("%s: TBRE_STATUS.sweep_err not cleared", why));
endtask

// A tagged capability at g, quiet.
task automatic setup_tagged(logic [31:0] g, logic tag);
  logic [31:0] w0, w1;
  make_cap(NvmBase, 0, PermsMemRw, w0, w1);
  core_store_cap(g, w0, w1, tag);
  wait_quiet();
endtask

task automatic t_err_tag_path();
  logic [31:0] g, ta, d;
  logic        t, e;
  int unsigned w0, inj0;
  cms_txn_t    r;
  cheriot_on();
  for (int unsigned c = 0; c < 3; c++) begin
    g  = MainSramBase + 32'h400 + 8 * c;
    ta = tag_word_addr(granule(g));
    setup_tagged(g, c != 2);
    expect_no_alert("before injection");
    w0   = u_meta.n_writes;
    inj0 = u_meta.n_inj_applied;
    case (c)
      0: begin  // error on the fill read of a store: aborted, no write-back, tag unchanged
        u_meta.inject(InjErr, ta, ta + 4, 1, 1);
        sb.exp_core_err = 1; sb.err_tag_effect = 0; sb.err_data_written = 1;
        run(PortCore, mk(PutFullData, g, 2'd2, 32'h1111_2222), r);
        if (u_meta.n_writes != w0) cms_error("test", "write-back after an errored fill");
      end
      1: begin  // error on a capability load's lookup: d_error and tag 0
        u_meta.inject(InjErr, ta, ta + 4, 1, 1);
        sb.exp_core_err = 1;
        core_load(g, 2'd2, 1'b1, d, t, e);
        if (t) cms_error("test", "tag 1 returned with a failed lookup");
      end
      default: begin  // error on the write-back: d_error; the tag write did not happen
        u_meta.inject(InjErr, ta, ta + 4, 2, 1);
        sb.exp_core_err = 1; sb.err_tag_effect = 0; sb.err_data_written = 1;
        run(PortCore, mk(PutFullData, g, 2'd2, 32'h3333_4444, 1'b1), r);
      end
    endcase
    if (u_meta.n_inj_applied != inj0 + 1) cms_error("test", "injection was not applied");
    if (!r.err && c != 1) cms_error("test", "no d_error to the core");
    sb.err_data_written = 0; sb.err_tag_effect = 2;
    expect_alert($sformatf("tag path device error, case %0d", c));
    wait_quiet();
    check_tags("tag path error");
    do_reset();
    expect_no_alert("after reset");
  end
endtask

task automatic t_err_meta_intg();
  logic [31:0] g, ta, d;
  logic        t, e;
  cms_txn_t    r;
  cheriot_on();
  for (int unsigned c = 0; c < 4; c++) begin
    g  = MainSramBase + 32'h600 + 8 * c;
    ta = tag_word_addr(granule(g));
    setup_tagged(g, 1'b1);
    expect_no_alert("before injection");
    u_meta.inject(1'(c % 2) ? InjDataIntg : InjRspIntg, ta, ta + 4, 1, 1);
    if (c < 2) core_load(g, 2'd2, 1'b1, d, t, e);                 // on a lookup
    else       run(PortCore, mk(PutFullData, g + 4, 2'd2, 32'h0), r);  // on a fill
    expect_alert($sformatf("meta SRAM integrity fault, case %0d", c));
    wait_quiet();
    do_reset();
  end
  // Observation only: an integrity fault on a response to the software bitmap window. The
  // subsystem passes it to the requester, which checks it; whether it also raises fatal_fault is
  // logged, not judged (the error table names "a meta SRAM response" without saying whose).
  // The corrupted integrity reaches the host unchanged, so its check is off for this read.
  u_meta.inject(InjRspIntg, MetaRevbmBase, MetaNvmTagBase, 1, 1);
  u_revbm.chk_rsp_intg = 1'b0;
  run(PortRevbm, mk(Get, MetaRevbmBase), r);
  u_revbm.chk_rsp_intg = 1'b1;
  wait_cycles(100);
  cms_info("test", $sformatf("revbm window response integrity fault: %0d alert(s)", alert_cnt));
  alert_allowed = 1;
  do_reset();
endtask

task automatic t_err_csr_intg();
  logic [31:0] d;
  logic        e;
  cms_txn_t    t, r;
  int unsigned n0;
  cheriot_on();
  csr_write(CsrTbreBase, MainSramBase + 32'h100, e);
  expect_no_alert("before injection");
  t = mk(PutFullData, 32'(CsrTbreBase), 2'd2, MainSramBase + 32'h800);
  t.bad_cmd_intg = 1'b1;
  run(PortCsr, t, r);
  if (!r.err) cms_error("test", "CSR write with bad integrity not answered with d_error");
  csr_read(CsrTbreBase, d);      // the scoreboard expects the old value
  expect_alert("CSR command integrity");
  // Latched until reset
  n0 = alert_cnt;
  wait_cycles(500);
  if (alert_cnt <= n0) cms_error("test", "fatal_fault not latched after a CSR integrity error");
  do_reset();
  wait_cycles(300);
  expect_no_alert("after reset");
endtask

// Faults on the engine's reads of the capability words. An error response to a read of the swept
// memory (a device denying it, e.g. a read-protected NVM page) only fails the sweep; an integrity
// fault, or an error from the meta SRAM path, also raises fatal_fault (theory_of_operation.md
// "Error Handling"; programmers_guide.md "Errors").
task automatic t_err_tbre_read();
  logic [31:0] b, a, cb, w0, w1;
  int unsigned k, n;
  bit          denied;
  cheriot_on();
  for (int unsigned c = 0; c < 6; c++) begin
    // Case 5 sweeps the top of the NVM, whose upper half denies every read
    b = (c == 5) ? NvmTop - 8 * 8 : MainSramBase + 32'h800;
    n = (c == 5) ? 8 : 4;
    for (int unsigned i = 0; i < n; i++) begin
      heap_base(1, cb);
      make_cap(cb, 0, PermsMemRw, w0, w1);
      store_cap(b + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    expect_no_alert("before injection");
    k = 1 + $urandom_range(2);
    a = b + 8 * k + ((c == 1) ? 0 : 4);
    denied = c inside {0, 1, 5};
    case (c)
      0, 1:    u_tbre_mem.inject(InjErr, a, a + 4, 1, 1);
      2:       u_tbre_mem.inject(InjRspIntg, a, a + 4, 1, 1);
      3:       u_tbre_mem.inject(InjDataIntg, a, a + 4, 1, 1);
      4:       u_meta.inject(InjErr, tag_word_addr(granule(b + 8 * k)),
                             tag_word_addr(granule(b + 8 * k)) + 4, 1, 1);  // its tag lookup
      default: u_tbre_mem.inject(InjErr, NvmTop - 8 * 4, NvmTop, 1, 8);    // both words of 4
    endcase
    tbre_sweep(b, n, 1'b0);
    sb.sweep_expect_fail(denied);
    // A capability with a read that failed, on either word or on its tag lookup, is not cleared;
    // the others are exact. Case 4: the four granules share one tag word, so the one-shot fault
    // hits the first lookup of that word, which is granule b's, whatever k is.
    if (c == 5)      for (int unsigned i = 4; i < 8; i++) sb.sweep_expect_kept(granule(b + 8 * i));
    else if (c == 4) sb.sweep_expect_kept(granule(b));
    else             sb.sweep_expect_kept(granule(b + 8 * k));
    tbre_wait_idle();   // the sweep still completes, with sweep_err and not counted
    check_failed_sweep($sformatf("fault on a TBRE read, case %0d", c));
    if (denied) begin
      wait_cycles(200);
      expect_no_alert($sformatf("error response to a TBRE read, case %0d", c));
    end else begin
      expect_alert($sformatf("fault on a TBRE read, case %0d", c));
    end
    wait_quiet();
    check_tags("TBRE read fault");
    do_reset();
  end
endtask

// Faults on the engine's revocation bitmap lookup: the capability counts as revoked.
task automatic t_err_tbre_revbm();
  logic [31:0] b, cb, w0, w1;
  cheriot_on();
  for (int unsigned c = 0; c < 3; c++) begin
    b = MainSramBase + 32'h900;
    for (int unsigned i = 0; i < 4; i++) begin
      heap_base(0, cb);                          // live: only a failed lookup revokes it
      make_cap(cb, 0, PermsMemRw, w0, w1);
      core_store_cap(b + 8 * i, w0, w1, 1'b1);
    end
    wait_quiet();
    expect_no_alert("before injection");
    // The engine's lookup of the first capability is the next bitmap read.
    u_meta.inject(c == 0 ? InjErr : (c == 1 ? InjRspIntg : InjDataIntg),
                  MetaRevbmBase, MetaNvmTagBase, 1, 1);
    tbre_sweep(b, 4, 1'b0);
    sb.sweep_expect_fail();
    sb.sweep_expect_revoked(granule(b));
    tbre_wait_idle();
    check_failed_sweep($sformatf("fault on a TBRE bitmap lookup, case %0d", c));
    expect_alert($sformatf("fault on a TBRE bitmap lookup, case %0d", c));
    wait_quiet();
    check_tags("TBRE bitmap lookup fault");
    do_reset();
  end
endtask

// Errors on the data path are the requester's business: d_error, no alert, and the tag still
// updated ("a failed data write still updates the tag").
task automatic t_err_data_path();
  logic [31:0] g, d;
  logic        t, e;
  cms_txn_t    r;
  cheriot_on();
  for (int unsigned c = 0; c < 4; c++) begin
    g = MainSramBase + 32'ha00 + 8 * c;
    setup_tagged(g, 1'(c % 2));
    u_dmem.inject(InjErr, g, g + 8, 0, 1);
    sb.exp_core_err = 1; sb.err_tag_effect = 1; sb.err_data_written = 0;
    if (c < 2) run(PortCore, mk(PutFullData, g, 2'd2, $urandom, !1'(c % 2)), r);
    else       core_load(g, 2'd2, 1'b1, d, t, e);
    sb.err_tag_effect = 2;
    wait_quiet();
  end
  core_load(DataErrBase, 2'd2, 1'b0, d, t, e);
  core_store(DataErrBase + 4, 2'd2, 32'h0, e);
  core_load_cap(MainSramBase + 32'ha00, t);
  core_load_cap(MainSramBase + 32'ha08, t);
  wait_cycles(200);
  check_tags("data path errors");
  expect_no_alert("data path errors");
endtask

// ALERT_TEST: a write of fatal_fault with regwen set raises exactly one alert handshake and does
// not latch; writing 0 to regwen disables alert testing until reset, the write that clears it
// still raising its own alert (registers.md ALERT_TEST). The scoreboard checks every regwen read.
task automatic alert_test_write(logic [31:0] data, int unsigned exp_alerts, string why);
  logic        e;
  int unsigned n0;
  n0 = alert_cnt;
  csr_write(CsrAlertTest, data, e);
  wait_cycles(600);
  if (alert_cnt - n0 != exp_alerts)
    cms_error("test", $sformatf("ALERT_TEST 0x%08x (%s) gave %0d alert handshake(s), expected %0d",
                                data, why, alert_cnt - n0, exp_alerts));
  alert_allowed = 1;
endtask

task automatic t_alert_test();
  logic [31:0] d;
  cheriot_on();
  wait_cycles(20);
  expect_no_alert("before ALERT_TEST");
  csr_read(CsrAlertTest, d);
  if (d != 32'h8000_0000) cms_error("test", $sformatf("ALERT_TEST reads 0x%08x out of reset", d));
  alert_test_write(32'h8000_0001, 1, "regwen kept");
  alert_test_write(32'h8000_0001, 1, "again");
  alert_test_write(32'h8000_0000, 0, "fatal_fault 0");
  csr_read(CsrAlertTest, d);
  alert_test_write(32'h0000_0000, 0, "regwen cleared");
  csr_read(CsrAlertTest, d);
  if (d != 32'h0) cms_error("test", $sformatf("ALERT_TEST reads 0x%08x after regwen cleared", d));
  alert_test_write(32'h8000_0001, 0, "locked");
  csr_read(CsrAlertTest, d);
  // Reset sets regwen again; a write of 0x1 (fatal_fault 1, regwen 0) raises its alert and locks
  do_reset();
  csr_read(CsrAlertTest, d);
  alert_test_write(32'h0000_0001, 1, "regwen cleared by the same write");
  alert_test_write(32'h8000_0001, 0, "locked by the previous write");
  csr_read(CsrAlertTest, d);
endtask

// ---------------------------------------------------------------------------------------------
// Reset
// ---------------------------------------------------------------------------------------------

task automatic t_reset();
  logic [31:0] d, w0, w1;
  logic        t;
  cheriot_on();
  for (int unsigned rep = 0; rep < 4; rep++) begin
    randomize_timing(0);
    fill_caps(MainSramBase, SramCaps);
    tbre_sweep(MainSramBase, SramCaps, 1'b0);
    for (int unsigned i = 0; i < 16; i++) begin
      make_cap(NvmBase, 0, PermsMemRw, w0, w1);
      core_store_cap(rand_granule(MainSramBase, MainSramTop), w0, w1, rbit());
    end
    wait_cycles($urandom_range(3000, 10));
    do_reset($urandom_range(8, 1));
    // Defaults, no alert, the engine idle, no stale response on any port
    csr_read(CsrRegwen, d);     if (d != 1) cms_error("test", "TBRE_REGWEN after reset");
    csr_read(CsrTbreStatus, d); if (d != 0) cms_error("test", "TBRE_STATUS after reset");
    csr_read(CsrTbreEpoch, d);  if (d != 0) cms_error("test", "TBRE_EPOCH after reset");
    csr_read(CsrIntrState, d);  if (d != 0) cms_error("test", "INTR_STATE after reset");
    csr_read(CsrTbreBase, d);
    csr_read(CsrTbreNum, d);
    wait_cycles(200);
    expect_no_alert("after reset");
    check_tags("after reset (partly swept, in-flight stores either way)");
    // Fully functional again
    load_all_caps(MainSramBase, 64);
    tbre_sweep(MainSramBase, SramCaps);
    check_tags("sweep after reset");
  end
  // Reset in ePMP mode keeps the mode (it is the system's), and the subsystem comes up quiet
  set_ena(prim_mubi_pkg::MuBi4False);
  do_reset();
  core_load_cap(MainSramBase, t);
  cheriot_on();
endtask

// ---------------------------------------------------------------------------------------------
// Constrained random
// ---------------------------------------------------------------------------------------------

task automatic t_random();
  int unsigned num_ops;
  logic [31:0] hot[5], a, w0, w1, cb, d;
  logic        e;
  bit          stop, accepted;
  int unsigned k;
  cms_txn_t    r;
  num_ops = 4000;
  void'($value$plusargs("num_ops=%d", num_ops));
  cheriot_on();
  foreach (hot[i]) hot[i] = (i < 4) ? MainSramBase + 256 * $urandom_range(SramCaps / 32 - 1) : NvmBase;
  stop = 0;
  fork
    // Sweeps of random ranges (including ignored starts), with random gaps
    begin
      while (!stop) begin
        wait_cycles($urandom_range(3000, 200));
        if (stop) break;
        accepted = 1;
        case ($urandom_range(11))
          0:       begin tbre_sweep(MainSramBase, 0, 1'b0); accepted = 0; end
          1:       begin
                     // k = 0 starts at MainSramTop, outside the tagged regions: an ignored
                     // start, so no completion interrupt will come
                     k = $urandom_range(8);
                     tbre_sweep(MainSramTop - 8 * k, 64, 1'b0);
                     accepted = (k != 0);
                   end
          2, 3:    tbre_sweep(hot[4] + 8 * $urandom_range(31), 32 * (1 + $urandom_range(3)), 1'b0);
          default: tbre_sweep(hot[$urandom_range(3)], 32 * (1 + $urandom_range(3)), 1'b0);
        endcase
        // Wait for the end by polling or by the interrupt; read the epoch; now and then clear
        // the sticky start_err
        if (accepted && rbit()) tbre_wait_intr();
        else                    tbre_wait_idle();
        csr_read(CsrTbreEpoch, d);
        if ($urandom_range(3) == 0) csr_write(CsrTbreStatus, 32'h1 << StatusStartErr, e);
      end
    end
    begin
      for (int unsigned i = 0; i < num_ops; i++) begin
        if (i % 500 == 0) randomize_timing($urandom_range(3) == 0);
        a = ($urandom_range(3) != 0) ? hot[$urandom_range(4)] + 8 * $urandom_range(31)
                                     : rand_granule(MainSramBase, MainSramTop);
        case ($urandom_range(15))
          0, 1, 2: begin
            if (rbit()) heap_base_peek(cb); else cb = NvmBase + 8 * $urandom_range(255);
            make_kind_cap(cb, $urandom_range(5), $urandom_range(3), w0, w1);
            // To the NVM, programmed first half of the time: verified, or answered with d_error
            store_cap(a, w0, w1, $urandom_range(3) != 0, rbit());
          end
          3, 4, 5: begin
            core_issue(mk(Get, a, 2'd2, 32'h0, 1'b1));
            core_issue(mk(Get, a + 4, 2'd2, 32'h0, 1'b1));
          end
          6:  core_issue(mk(PutFullData, a + $urandom_range(7), 2'($urandom_range(2)), $urandom));
          7:  core_issue(mk(PutPartialData, a + 4 * rbit(), 2'd2, $urandom, 1'b0,
                            4'($urandom_range(15, 1))));
          8:  core_issue(mk(Get, a + $urandom_range(7), 2'($urandom_range(2))));
          9:  core_issue(mk(Get, UntaggedBase + 4 * $urandom_range(1023), 2'd2, 32'h0, rbit()));
          10: core_issue(mk(PutFullData, UntaggedBase + 4 * $urandom_range(1023), 2'd2, $urandom, rbit()));
          11: begin heap_base(rbit(), cb); end
          12: run(PortCoreRevbm, mk(Get, MetaRevbmBase + 4 * $urandom_range(RevbmBytes / 4 - 1)), r);
          13: run(PortRevbm, mk(Get, MetaBase + 4 * $urandom_range((MetaTop - MetaBase) / 4)), r);
          default: begin
            core_issue(mk(Get, NvmBase + 8 * $urandom_range(NvmCaps - 1), 2'd2, 32'h0, 1'b1));
          end
        endcase
        throttle($urandom_range(8, 1));
      end
      stop = 1;
    end
  join
  wait_quiet();
  tbre_wait_idle();
  check_tags("random");
endtask

// A heap base, with whatever revocation bit it has (no bitmap write)
task automatic heap_base_peek(output logic [31:0] base);
  base = rand_granule(MainSramBase, MainSramTop);
endtask

// ---------------------------------------------------------------------------------------------

task automatic run_test(string name);
  case (name)
    "cms_smoke":                 t_smoke();
    "cms_tag_store_load":        t_tag_store_load();
    "cms_tag_clear_subword":     t_tag_clear_subword();
    "cms_cap_load_hint":         t_cap_load_hint();
    "cms_mode_gating":           t_mode_gating();
    "cms_access_check":          t_access_check();
    "cms_rmw_same_word":         t_rmw_same_word();
    "cms_rmw_core_tbre_same_word": t_rmw_core_tbre_same_word();
    "cms_tbre_sweep":            t_tbre_sweep();
    "cms_tbre_base_decode":      t_tbre_base_decode();
    "cms_tbre_csr":              t_tbre_csr();
    "cms_tbre_concurrent":       t_tbre_concurrent();
    "cms_tbre_store_race":       t_tbre_store_race();
    "cms_tbre_snoop_window":     t_tbre_snoop_window();
    "cms_tbre_epoch":            t_tbre_epoch();
    "cms_tbre_intr":             t_tbre_intr();
    "cms_nvm_cap_store":         t_nvm_cap_store();
    "cms_err_tag_path":          t_err_tag_path();
    "cms_err_meta_intg":         t_err_meta_intg();
    "cms_err_csr_intg":          t_err_csr_intg();
    "cms_err_tbre_read":         t_err_tbre_read();
    "cms_err_tbre_revbm":        t_err_tbre_revbm();
    "cms_err_data_path":         t_err_data_path();
    "cms_alert_test":            t_alert_test();
    "cms_reset":                 t_reset();
    "cms_random":                t_random();
    // cms_tests_eg2.svh
    "cms_tag_sideband_mubi":     t_tag_sideband_mubi();
    "cms_lockstep_glitch":       t_lockstep_glitch();
    "cms_latency":               t_latency();
    "cms_trvk_fault":            t_trvk_fault();
    "cms_trvk_backpressure":     t_trvk_backpressure();
    default: cms_error("tb", $sformatf("unknown test '%s'", name));
  endcase
endtask
