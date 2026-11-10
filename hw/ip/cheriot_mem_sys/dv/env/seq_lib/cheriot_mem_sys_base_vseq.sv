// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The base of every cheriot_mem_sys vseq. dut_init() applies the reset, preloads the memories
// behind the host ports through the memory backdoor and starts the sequences that answer for
// them. The tasks below are the operations the feature vseqs are written in:
// - core accesses, on the core agent (one cheriot_mem_sys_core_access_seq each);
// - revocation bitmap accesses, on the two bitmap host agents;
// - registers, through the RAL (csr_rd / csr_wr), and malformed register accesses on the `regs`
//   agent;
// - faults injected by the memory devices, resets.
// The sequences only drive: the scoreboard checks every response against the reference model.
class cheriot_mem_sys_base_vseq extends cip_base_vseq #(
    .RAL_T               (cheriot_mem_sys_regs_reg_block),
    .CFG_T               (cheriot_mem_sys_env_cfg),
    .COV_T               (cheriot_mem_sys_env_cov),
    .VIRTUAL_SEQUENCER_T (cheriot_mem_sys_virtual_sequencer)
  );
  `uvm_object_utils(cheriot_mem_sys_base_vseq)

  // The parts of the memories that are preloaded with random words: the whole main SRAM and NVM
  // with the small map, part of them with Earl Grey 2's; the untagged memory. The meta SRAM starts
  // with zeros (tags 0, bitmap clear).
  localparam int unsigned SramWin = (SramBytes <= 'h4000) ? SramBytes : 'h2000;
  localparam int unsigned NvmWin  = (NvmBytes <= 'h2000) ? NvmBytes : 'h1000;

  // The memories behind the host ports.
  cheriot_mem_sys_mem_device_seq m_cored_h_seq, m_tbre_h_seq, m_meta_seq;
  protected bit                  mem_side_started;

  // Accesses started and not answered yet.
  protected int unsigned core_inflight, other_inflight;
  // A capability store to the NVM: the core issues nothing else until its second word is answered
  // (as Ibex does; the WTRC relies on it). Every core access takes the lock to be issued.
  protected semaphore core_lock = new(1);
  // A sweep this sequence started and has not seen end.
  protected bit sweep_running;

  `uvm_object_new

  // ---- bring-up ----

  virtual task dut_init(string reset_kind = "HARD");
    super.dut_init(reset_kind);
    if (!mem_side_started) begin
      preload_memories();
      start_mem_side();
      mem_side_started = 1'b1;
    end
  endtask

  virtual task body();
  endtask

  // A feature test ends with every access answered and no sweep running, so that the scoreboard's
  // final compare of the meta SRAM sees the end state.
  virtual task post_start();
    if (cfg.en_cmodel) begin
      wait_quiet();
      tbre_wait_idle();
    end
    super.post_start();
  endtask

  // A word written into the memory behind the crossbar through its backdoor.
  protected function void preload(bit [31:0] addr, bit [31:0] word);
    p_sequencer.mem_backdoor.write_word(addr, word);
  endfunction

  protected virtual function void preload_memories();
    for (int unsigned a = 0; a < SramWin; a += 4) preload(MainSramBase + a, $urandom());
    for (int unsigned a = 0; a < NvmWin; a += 4) preload(NvmBase + a, $urandom());
    for (bit [31:0] a = UntaggedBase; a < UntaggedTop; a += 4) preload(a, $urandom());
    for (bit [31:0] a = MetaBase; a < MetaTop; a += 4) cfg.meta_mem.write(a, 32'h0);
  endfunction

  // The devices behind the host ports answer from the memories, with random delays and no errors.
  protected virtual task start_mem_side();
    m_cored_h_seq = new_device_seq("m_cored_h_seq", cfg.data_mem);
    m_tbre_h_seq  = new_device_seq("m_tbre_h_seq", cfg.data_mem);
    m_meta_seq    = new_device_seq("m_meta_seq", cfg.meta_mem);
    // The crossbar refuses its error range, and writes to the NVM (read-only on the interconnect;
    // a capability store reaches it as reads).
    m_cored_h_seq.err_lo = DataErrBase;  m_cored_h_seq.err_hi = UntaggedTop;
    m_cored_h_seq.ro_lo  = NvmBase;      m_cored_h_seq.ro_hi  = NvmTop;
    m_tbre_h_seq.err_lo  = DataErrBase;  m_tbre_h_seq.err_hi  = UntaggedTop;
    m_tbre_h_seq.ro_lo   = NvmBase;      m_tbre_h_seq.ro_hi   = NvmTop;
    fork
      m_cored_h_seq.start(p_sequencer.cored_h_sqr);
      m_tbre_h_seq.start(p_sequencer.tbre_h_sqr);
      m_meta_seq.start(p_sequencer.meta_sqr);
    join_none
    #0;
  endtask

  protected function cheriot_mem_sys_mem_device_seq new_device_seq(string name,
                                                                   mem_model#(32) mem);
    cheriot_mem_sys_mem_device_seq s = cheriot_mem_sys_mem_device_seq::type_id::create(name);
    s.mem           = mem;
    s.min_rsp_delay = 0;
    s.max_rsp_delay = 10;
    s.rsp_abort_pct = 0;
    s.d_error_pct   = 0;
    `DV_CHECK_RANDOMIZE_FATAL(s)
    return s;
  endfunction

  // ---- timing and mode ----

  protected task wait_cycles(int unsigned n);
    cfg.clk_rst_vif.wait_clks(n);
  endtask

  // cheriot_ena_i.
  protected task set_ena(prim_mubi_pkg::mubi4_t v);
    wait_cycles(1);
    cfg.m_core_agent_cfg.vif.cheriot_ena = v;
    wait_cycles(1);
  endtask

  protected task cheriot_on();
    set_ena(prim_mubi_pkg::MuBi4True);
  endtask

  // Handshake delays on every port; `aggressive` keeps the core side and the meta SRAM fast.
  protected function void randomize_timing(bit aggressive);
    cfg.m_core_agent_cfg.a_valid_delay_max      = aggressive ? 0 : $urandom_range(3);
    cfg.m_core_agent_cfg.d_ready_delay_max      = aggressive ? 0 : $urandom_range(5);
    cfg.m_revbm_agent_cfg.a_valid_delay_max     = $urandom_range(4);
    cfg.m_revbm_agent_cfg.d_ready_delay_max     = $urandom_range(4);
    cfg.m_corerevbm_agent_cfg.a_valid_delay_max = $urandom_range(4);
    cfg.m_corerevbm_agent_cfg.d_ready_delay_max = $urandom_range(4);
    m_cored_h_seq.max_rsp_delay = aggressive ? 0 : $urandom_range(4);
    m_tbre_h_seq.max_rsp_delay  = $urandom_range(4);
    m_meta_seq.max_rsp_delay    = aggressive ? 0 : $urandom_range(3);
  endfunction

  // At most `max_inflight` core accesses unanswered.
  protected task throttle(int unsigned max_inflight = 8);
    wait (core_inflight <= max_inflight);
  endtask

  // Every port idle, and every response seen by the scoreboard.
  protected task wait_quiet();
    wait (core_inflight == 0 && other_inflight == 0);
    csr_utils_pkg::wait_no_outstanding_access();
    wait (m_cored_h_seq.req_q.size() == 0 && m_tbre_h_seq.req_q.size() == 0 &&
          m_meta_seq.req_q.size() == 0);
    wait_cycles(3);
  endtask

  protected task wait_core_quiet();
    wait (core_inflight == 0);
    wait_cycles(2);
  endtask

  // ---- the core ----

  // A core access. Sub-word data goes in the lanes of its address; the mask defaults to the lanes
  // of the size.
  protected function cheriot_mem_sys_core_item core_item(tlul_pkg::tl_a_op_e op, bit [31:0] addr,
                                                         bit [1:0] size = 2, bit [31:0] data = 0,
                                                         bit tag = 0, bit [3:0] mask = 0);
    cheriot_mem_sys_core_item it = cheriot_mem_sys_core_item::type_id::create("it");
    it.opcode = op;
    it.size   = size;
    it.addr   = (size == 2) ? {addr[31:2], 2'b00} : (size == 1) ? {addr[31:1], 1'b0} : addr;
    it.mask   = (mask != 0) ? mask : cheriot_mem_sys_core_item::lanes(it.addr, size);
    it.data   = (size == 2) ? data : data << (8 * it.addr[1:0]);
    it.tag    = tag;
    return it;
  endfunction

  // Starts a core access and returns once the core agent has taken it, so accesses are presented
  // in the order they are issued. Its response arrives in `it` later (it.done).
  protected task core_issue(cheriot_mem_sys_core_item it, bit locked = 1'b0);
    cheriot_mem_sys_core_access_seq seq =
        cheriot_mem_sys_core_access_seq::type_id::create("core_access_seq");
    seq.req = it;
    if (!locked) core_lock.get();
    core_inflight++;
    fork
      begin
        seq.start(p_sequencer.core_sqr);
        core_inflight--;
      end
    join_none
    wait (seq.granted);
    if (!locked) core_lock.put();
  endtask

  // A core access, waiting for its response.
  protected task core_access(cheriot_mem_sys_core_item it);
    core_issue(it);
    wait (it.done);
  endtask

  protected task core_store(bit [31:0] addr, bit [1:0] size, bit [31:0] data, output bit err);
    cheriot_mem_sys_core_item it = core_item(tlul_pkg::PutFullData, addr, size, data);
    core_access(it);
    err = it.err;
  endtask

  protected task core_load(bit [31:0] addr, bit [1:0] size, bit hint,
                           output bit [31:0] data, output bit tag, output bit err);
    cheriot_mem_sys_core_item it = core_item(tlul_pkg::Get, addr, size, 0, hint);
    core_access(it);
    data = it.rdata;
    tag  = it.rtag;
    err  = it.err;
  endtask

  // A capability store: two word writes back to back, both with the tag. To the NVM nothing else
  // is issued until the second word is answered; elsewhere it returns once both are issued.
  protected task core_store_cap(bit [31:0] addr, bit [31:0] w0, bit [31:0] w1, bit tag);
    cheriot_mem_sys_core_item it0 = core_item(tlul_pkg::PutFullData, granule_of(addr), 2, w0, tag);
    cheriot_mem_sys_core_item it1 = core_item(tlul_pkg::PutFullData, granule_of(addr) + 4, 2, w1,
                                              tag);
    core_lock.get();
    core_issue(it0, .locked(1'b1));
    core_issue(it1, .locked(1'b1));
    if (tag && in_nvm(addr)) wait (it1.done);
    core_lock.put();
  endtask

  // A capability store to the NVM, waiting for both answers.
  protected task nvm_cap_store_wait(bit [31:0] addr, bit [31:0] w0, bit [31:0] w1,
                                    output bit e0, output bit e1);
    cheriot_mem_sys_core_item it0 = core_item(tlul_pkg::PutFullData, granule_of(addr), 2, w0, 1);
    cheriot_mem_sys_core_item it1 = core_item(tlul_pkg::PutFullData, granule_of(addr) + 4, 2, w1,
                                              1);
    core_lock.get();
    core_issue(it0, .locked(1'b1));
    core_issue(it1, .locked(1'b1));
    wait (it0.done && it1.done);
    core_lock.put();
    e0 = it0.err;
    e1 = it1.err;
  endtask

  // A capability load: two hinted word reads back to back; the tag is valid if both carry it.
  protected task core_load_cap(bit [31:0] addr, output bit tag);
    cheriot_mem_sys_core_item it0 = core_item(tlul_pkg::Get, granule_of(addr), 2, 0, 1);
    cheriot_mem_sys_core_item it1 = core_item(tlul_pkg::Get, granule_of(addr) + 4, 2, 0, 1);
    core_lock.get();
    core_issue(it0, .locked(1'b1));
    core_issue(it1, .locked(1'b1));
    core_lock.put();
    wait (it0.done && it1.done);
    tag = it0.rtag && it1.rtag && !it0.err && !it1.err;
  endtask

  // Two hinted word reads of a capability, not waited for.
  protected task core_issue_load_cap(bit [31:0] addr);
    core_lock.get();
    core_issue(core_item(tlul_pkg::Get, granule_of(addr), 2, 0, 1), .locked(1'b1));
    core_issue(core_item(tlul_pkg::Get, granule_of(addr) + 4, 2, 0, 1), .locked(1'b1));
    core_lock.put();
  endtask

  // A capability's two words programmed into the NVM through the backdoor, as software does
  // through the NVM controller before it stores the capability (programmers_guide.md "Storing
  // Capabilities in the NVM"); skipped while a sweep runs.
  protected task nvm_program(bit [31:0] addr, bit [31:0] w0, bit [31:0] w1);
    if (!in_nvm(addr)) return;
    wait_core_quiet();
    if (sweep_running) return;
    preload(granule_of(addr), w0);
    preload(granule_of(addr) + 4, w1);
  endtask

  // A capability store; to the NVM with `prog` the words are programmed there first, so that the
  // store is verified and sets the tag.
  protected task store_cap(bit [31:0] addr, bit [31:0] w0, bit [31:0] w1, bit tag,
                           bit prog = 1'b1);
    if (prog && tag) nvm_program(addr, w0, w1);
    core_store_cap(addr, w0, w1, tag);
  endtask

  // ---- capabilities ----

  // A capability with base `base` (aligned down to 2^e) and a random address that decodes back
  // to it.
  protected function void make_cap(bit [31:0] base, int unsigned e, bit [5:0] perms,
                                   output bit [31:0] w0, output bit [31:0] w1);
    int unsigned     ee   = (e >= 24) ? 24 : e;
    longint unsigned b    = 64'(base) & ~((64'd1 << ee) - 1);
    longint unsigned span = 64'd1 << (ee + 9);
    longint unsigned off  = {$urandom, $urandom} % span;
    if (b + off > 64'hffff_ffff) off = 0;
    w0 = 32'(b + off);
    w1 = cap_meta(32'(b), ee, perms, 3'($urandom_range(7)), 9'($urandom));
    if (cap_base(w0, w1) != 32'(b)) begin
      `uvm_error(`gfn, $sformatf("make_cap: base 0x%08x decodes as 0x%08x", 32'(b),
                                 cap_base(w0, w1)))
    end
  endfunction

  typedef enum int {CapMemRw, CapMemRo, CapExec, CapSealing, CapNoPerms, CapSealed} cap_kind_e;

  protected function void make_kind_cap(bit [31:0] base, cap_kind_e kind, int unsigned e,
                                        output bit [31:0] w0, output bit [31:0] w1);
    bit [5:0] perms;
    case (kind)
      CapMemRo:   perms = PermsMemRo;
      CapExec:    perms = PermsExec;
      CapSealing: perms = PermsSealing;
      CapNoPerms: perms = PermsNone;
      default:    perms = PermsMemRw;
    endcase
    make_cap(base, e, perms, w0, w1);
    // otype: sealed capabilities have one of 1..7, the sealing kind keeps make_cap's random one,
    // every other kind is unsealed.
    if (kind == CapSealed)       w1[24:22] = 3'(1 + $urandom_range(6));
    else if (kind != CapSealing) w1[24:22] = 3'b000;
  endfunction

  protected function cap_kind_e rand_cap_kind(int unsigned max = 5);
    return cap_kind_e'($urandom_range(max));
  endfunction

  protected function bit rbit();
    return 1'($urandom_range(1));
  endfunction

  protected function bit [31:0] rand_granule(bit [31:0] base, bit [31:0] top);
    return base + 8 * $urandom_range((top - base) / 8 - 1);
  endfunction

  // A random heap base, its revocation bit set to `revoked`.
  protected task heap_base(bit revoked, output bit [31:0] base);
    base = rand_granule(MainSramBase, MainSramTop);
    revbm_set_base(base, revoked);
  endtask

  // Capabilities of every class into n granules from `base`: revoked or live heap capabilities,
  // sealing (exempt) and sealed ones, bases below and above the heap, executable ones; one in
  // eight is stored as data (tag 0).
  protected task fill_caps(bit [31:0] base, int unsigned n);
    bit [31:0] w0, w1, cb;
    for (int unsigned i = 0; i < n; i++) begin
      case ($urandom_range(9))
        0: begin heap_base(1, cb); make_kind_cap(cb, CapMemRw, 0, w0, w1); end
        1: begin heap_base(0, cb); make_kind_cap(cb, CapMemRw, 0, w0, w1); end
        2: begin heap_base(1, cb); make_kind_cap(cb, CapSealing, 0, w0, w1); end
        3: begin heap_base(1, cb); make_kind_cap(cb, CapSealed, 0, w0, w1); end
        4: make_kind_cap(32'h0800_0000 + 8 * $urandom_range(255), CapMemRw, 0, w0, w1);
        5: make_kind_cap(NvmBase + 8 * $urandom_range(255), CapMemRw, 0, w0, w1);
        6: begin
          heap_base(1, cb);
          make_kind_cap(cb, rand_cap_kind(3), $urandom_range(3), w0, w1);
        end
        7: begin heap_base(1, cb); make_kind_cap(cb, CapExec, 0, w0, w1); end
        default: begin heap_base(1, cb); make_kind_cap(cb, CapMemRw, 0, w0, w1); end
      endcase
      store_cap(base + 8 * i, w0, w1, $urandom_range(7) != 0);
      throttle();
    end
    wait_quiet();
  endtask

  protected task load_all_caps(bit [31:0] base, int unsigned n);
    bit t;
    for (int unsigned i = 0; i < n; i++) core_load_cap(base + 8 * i, t);
  endtask

  // A revoked capability stored at `addr`.
  protected task revoked_cap_at(bit [31:0] addr);
    bit [31:0] cb, w0, w1;
    heap_base(1, cb);
    make_cap(cb, 0, PermsMemRw, w0, w1);
    store_cap(addr, w0, w1, 1'b1);
    wait_quiet();
  endtask

  // A capability with tag `tag` stored at `addr`.
  protected task setup_tagged(bit [31:0] addr, bit tag);
    bit [31:0] w0, w1;
    make_cap(NvmBase, 0, PermsMemRw, w0, w1);
    core_store_cap(addr, w0, w1, tag);
    wait_quiet();
  endtask

  // ---- the revocation bitmap ----

  // One access on a bitmap port: the software window, or the core's TRVK port.
  protected task bitmap_access(bit core_port, tlul_pkg::tl_a_op_e op, bit [31:0] addr,
                               bit [1:0] size = 2, bit [31:0] data = 0, bit [3:0] mask = 0,
                               output tl_seq_item rsp);
    tl_host_access(core_port ? p_sequencer.corerevbm_sqr : p_sequencer.revbm_sqr, op, addr, size,
                   data, mask, 1'b0, rsp);
  endtask

  protected task revbm_write(int unsigned word, bit [31:0] data);
    tl_seq_item rsp;
    bitmap_access(0, tlul_pkg::PutFullData, MetaRevbmBase + 4 * word, 2, data, 0, rsp);
  endtask

  // Sets the revocation bit of `base` to `value`.
  protected task revbm_set_base(bit [31:0] base, bit value);
    int unsigned b;
    bit [31:0]   d;
    if (!revbm_index(base, b)) return;
    d = cfg.meta_mem.read(MetaRevbmBase + (b / 32) * 4);
    d[b % 32] = value;
    revbm_write(b / 32, d);
  endtask

  // ---- registers ----

  // A register access the RAL does not make (partial writes, unmapped offsets, a corrupted
  // command integrity), at `offset` in the register block.
  protected task csr_raw_access(tlul_pkg::tl_a_op_e op, bit [31:0] offset, bit [1:0] size = 2,
                                bit [31:0] data = 0, bit [3:0] mask = 0, bit bad_cmd_intg = 0,
                                output tl_seq_item rsp);
    tl_host_access(p_sequencer.tl_sequencer_h, op, ral.default_map.get_base_addr() + offset,
                   size, data, mask, bad_cmd_intg, rsp);
  endtask

  protected task tl_host_access(tl_sequencer sqr, tlul_pkg::tl_a_op_e op, bit [31:0] addr,
                                bit [1:0] size, bit [31:0] data, bit [3:0] mask,
                                bit bad_cmd_intg, output tl_seq_item rsp);
    cheriot_mem_sys_tl_host_seq seq = cheriot_mem_sys_tl_host_seq::type_id::create("tl_host_seq");
    bit [3:0] m = (mask != 0) ? mask : cheriot_mem_sys_core_item::lanes(addr, size);
    if (bad_cmd_intg) seq.tl_intg_err_type = TlIntgErrCmd;
    seq.control_addr_alignment = 1'b1;
    seq.control_rand_size      = 1'b1;
    seq.control_rand_opcode    = 1'b1;
    `DV_CHECK_RANDOMIZE_WITH_FATAL(seq,
        addr   == local::addr;
        write  == (local::op != tlul_pkg::Get);
        opcode == local::op;
        size   == local::size;
        mask   == local::m;
        data   == local::data;)
    other_inflight++;
    seq.start(sqr);
    other_inflight--;
    rsp = seq.rsp;
  endtask

  // ---- the revocation engine, as software drives it ----

  // Polls TBRE_STATUS.busy until the sweep is over (programmers_guide.md, "Revoking
  // Capabilities").
  protected task tbre_wait_idle(int unsigned max_polls = 100000);
    bit [31:0] status;
    for (int unsigned i = 0; i < max_polls; i++) begin
      csr_rd(.ptr(ral.tbre_status), .value(status));
      if (!status[StatusBusy]) begin
        sweep_running = 1'b0;
        return;
      end
      wait_cycles($urandom_range(8));
    end
    `uvm_fatal(`gfn, "timeout: TBRE_STATUS.busy never cleared")
  endtask

  // Starts a sweep of `num` capabilities from `base` (once the engine is idle).
  protected task tbre_sweep(bit [31:0] base, bit [31:0] num, bit wait_done = 1'b1);
    tbre_wait_idle();
    csr_wr(.ptr(ral.tbre_base_addr), .value(base));
    csr_wr(.ptr(ral.tbre_num_caps), .value(num));
    csr_wr(.ptr(ral.tbre_start), .value(32'h1));
    sweep_running = 1'b1;
    if (wait_done) tbre_wait_idle();
  endtask

  // Waits for the end of the running sweep by its tbre_done interrupt (programmers_guide.md,
  // "Tracking Completed Sweeps"), re-reading busy as a wake can be stale.
  protected task tbre_wait_intr();
    bit [31:0] status;
    csr_wr(.ptr(ral.intr_enable), .value(32'h1));
    for (int unsigned w = 0; w < 4; w++) begin
      wait_intr_pin(1'b1);
      csr_rd(.ptr(ral.tbre_status), .value(status));
      csr_wr(.ptr(ral.intr_state), .value(32'h1));
      if (!status[StatusBusy]) break;
      csr_rd(.ptr(ral.tbre_status), .value(status));
      if (!status[StatusBusy]) break;
    end
    sweep_running = 1'b0;
  endtask

  // Waits for intr_tbre_done_o to be `level`, as software waits for an interrupt. `what` names the
  // step in the timeout message.
  protected task wait_intr_pin(bit level, int unsigned max_cycles = 200000, string what = "");
    for (int unsigned i = 0; i < max_cycles; i++) begin
      if (cfg.intr_vif.pins[0] == level) return;
      wait_cycles(1);
    end
    `uvm_fatal(`gfn, $sformatf("timeout: intr_tbre_done_o not %0b after %0d cycles%s", level,
                               max_cycles, what == "" ? "" : {" (", what, ")"}))
  endtask

  // Reads TBRE_STATUS and TBRE_EPOCH after a failed sweep and clears sweep_err, as software does
  // (programmers_guide.md, "Errors").
  protected task clear_sweep_err();
    bit [31:0] d;
    csr_rd(.ptr(ral.tbre_status), .value(d));
    csr_rd(.ptr(ral.tbre_epoch), .value(d));
    csr_wr(.ptr(ral.tbre_status), .value(32'h1 << StatusSweepErr));
    csr_rd(.ptr(ral.tbre_status), .value(d));
  endtask

  // A write of ALERT_TEST, and time for the alert it may raise.
  protected task alert_test_write(bit [31:0] data);
    csr_wr(.ptr(ral.alert_test), .value(data));
    wait_cycles(600);
  endtask

  // ---- resets and injected faults ----

  // A reset of the subsystem at a quiet point, once an alert a fault raised has had time to come;
  // the injections are cleared.
  protected task do_reset();
    wait_quiet();
    wait_cycles(300);
    m_cored_h_seq.clear_injections();
    m_tbre_h_seq.clear_injections();
    m_meta_seq.clear_injections();
    apply_reset("HARD");
    sweep_running = 1'b0;
    wait_cycles(2);
  endtask

  // The WTRC's assertions describe the core; off around a store the core cannot produce.
  protected task wtrc_assertions(bit on);
    cfg.m_core_agent_cfg.vif.wtrc_assert_en = on;
    wait_cycles(1);
  endtask

  // The meta SRAM holds requests back (a_ready low), or takes them again.
  protected function void meta_stall(bit on);
    cfg.m_meta_agent_cfg.a_ready_delay_min = on ? 1000 : 0;
    cfg.m_meta_agent_cfg.a_ready_delay_max = on ? 1000 : 10;
  endfunction

endclass : cheriot_mem_sys_base_vseq
