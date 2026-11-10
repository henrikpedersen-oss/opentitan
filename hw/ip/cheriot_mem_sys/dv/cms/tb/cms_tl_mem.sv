// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// TL-UL device model for the subsystem's host ports: the data memory behind cored_tl_h and
// tbre_tl_h (two instances sharing one cms_store, as the interconnect leads both to the same SRAM),
// and the meta SRAM behind meta_sram_tl.
//
// In order, up to Depth requests outstanding, a_ready withheld at random (a_ready_pct), response
// latency lat_min..lat_max cycles (>= 1). Writes are applied, and reads return data, at the A
// handshake. Responses carry response and data integrity, as OpenTitan's SRAM adapters generate
// them (the subsystem checks both on the meta SRAM and TBRE paths). The NVM is read-only on the
// interconnect, as in the system (programmers_guide.md "Storing Capabilities in the NVM"): a write
// to it is answered with d_error and not applied; tests program it through the backdoor.
//
// Checks on every request: command integrity, and data integrity of writes, are correct. The meta
// SRAM additionally requires CHERIoT mode, an address inside the meta SRAM, a full-word Get or
// PutFullData (what the access checkers let through); the TBRE port forwards each request, with
// the cycle it was first presented, to the scoreboard, which checks the sweep's address sequence
// and that it never writes, and times core writes against the engine's reads.
//
// Fault injection (inject()): answer the next `count` requests in [lo, hi) of the selected kind
// (op: 0 any, 1 reads, 2 writes) with d_error (the write is then not applied), a corrupted
// rsp_intg, or a corrupted data_intg.

module cms_tl_mem
  import cms_pkg::*;
#(
  parameter string       Name  = "mem",
  parameter int unsigned Kind  = 0,  // 0 data (core port), 1 data (TBRE port), 2 meta SRAM
  parameter int unsigned Depth = 4
) (
  input  logic                  clk_i,
  input  logic                  rst_ni,
  input  prim_mubi_pkg::mubi4_t ena_i,
  input  tlul_pkg::tl_h2d_t     tl_i,
  output tlul_pkg::tl_d2h_t     tl_o
);

  localparam int unsigned KindCore = 0;
  localparam int unsigned KindTbre = 1;
  localparam int unsigned KindMeta = 2;


  typedef struct {
    inj_kind_e   kind;
    logic [31:0] lo, hi;
    int unsigned op;
    int unsigned count;
  } inj_t;

  typedef struct {
    tlul_pkg::tl_a_op_e opcode;
    logic [1:0]         size;
    logic [7:0]         source;
    logic [31:0]        addr;
    logic [31:0]        rdata;
    logic               err;
    int                 inj;   // -1 none, else inj_kind_e
    longint unsigned    ready_cycle;
  } rsp_t;

  // Knobs
  int unsigned a_ready_pct = 100;
  int unsigned lat_min     = 1;
  int unsigned lat_max     = 1;

  cms_store store;
  cms_sb    sb;

  inj_t        inj_q[$];
  int unsigned n_inj_applied = 0;
  rsp_t        rsp_q[$];
  int unsigned n_reads = 0, n_writes = 0;
  // Last response handed over, for tests that trigger on it
  logic [31:0] last_rsp_addr;
  tlul_pkg::tl_a_op_e last_rsp_op;
  event        rsp_ev;
  // The request presented now: since when, and at which address (for tests that trigger on it)
  bit          pres_valid = 0;
  longint unsigned pres_cycle;
  logic [31:0] pres_addr;
  // The cycle each address was last first presented in. pres_addr can move on in the next cycle (a
  // request accepted at once), so a test polling it can miss a presentation; this cannot. A test
  // deletes the entry of the address it waits for before starting the traffic.
  longint unsigned pres_seen [logic [31:0]];

  function automatic void inject(inj_kind_e kind, logic [31:0] lo, logic [31:0] hi,
                                 int unsigned op, int unsigned count);
    inj_t i;
    i.kind = kind; i.lo = lo; i.hi = hi; i.op = op; i.count = count;
    inj_q.push_back(i);
  endfunction

  function automatic void clear_injections();
    inj_q.delete();
  endfunction

  function automatic bit idle();
    return rsp_q.size() == 0;
  endfunction

  function automatic int match_injection(logic [31:0] addr, bit wr);
    foreach (inj_q[i]) begin
      if (inj_q[i].count > 0 && addr >= inj_q[i].lo && addr < inj_q[i].hi &&
          (inj_q[i].op == 0 || (inj_q[i].op == 1 && !wr) || (inj_q[i].op == 2 && wr))) begin
        inj_q[i].count--;
        n_inj_applied++;
        $display("CMS_INJECT @%0d [%s] %s on %s 0x%08x", cms_cycle, Name, inj_q[i].kind.name(),
                 wr ? "write" : "read", addr);
        return int'(inj_q[i].kind);
      end
    end
    return -1;
  endfunction

  function automatic tlul_pkg::tl_d2h_t make_rsp(rsp_t r);
    tlul_pkg::tl_d2h_t d;
    logic [63:0]       enc;
    d          = tlul_pkg::TL_D2H_DEFAULT;
    d.d_valid  = 1'b1;
    d.d_opcode = (r.opcode == tlul_pkg::Get) ? tlul_pkg::AccessAckData : tlul_pkg::AccessAck;
    d.d_param  = 3'b000;
    d.d_size   = r.size;
    d.d_source = r.source;
    d.d_sink   = '0;
    d.d_data   = (r.opcode == tlul_pkg::Get) ? r.rdata : 32'h0;
    d.d_error  = r.err;
    enc = prim_secded_pkg::prim_secded_inv_64_57_enc(
            tlul_pkg::D2HRspMaxWidth'(tlul_pkg::extract_d2h_rsp_intg(d)));
    d.d_user.rsp_intg  = enc[63:57];
    d.d_user.data_intg = tlul_pkg::get_data_intg(d.d_data);
    if (r.inj == int'(InjRspIntg))  d.d_user.rsp_intg[0]  = ~d.d_user.rsp_intg[0];
    if (r.inj == int'(InjDataIntg)) d.d_user.data_intg[0] = ~d.d_user.data_intg[0];
    d.a_ready = 1'b0;
    return d;
  endfunction

  function automatic void check_req(tlul_pkg::tl_h2d_t h, prim_mubi_pkg::mubi4_t ena);
    bit wr;
    wr = h.a_opcode != tlul_pkg::Get;
    if (h.a_user.cmd_intg != tlul_pkg::get_cmd_intg(h))
      cms_error(Name, $sformatf("bad command integrity on %s 0x%08x", h.a_opcode.name(),
                                h.a_address));
    if (wr && h.a_user.data_intg != tlul_pkg::get_data_intg(h.a_data))
      cms_error(Name, $sformatf("bad data integrity on write 0x%08x data 0x%08x", h.a_address,
                                h.a_data));
    if (Kind == KindMeta) begin
      if (ena != prim_mubi_pkg::MuBi4True)
        cms_error(Name, $sformatf("meta SRAM access 0x%08x outside CHERIoT mode (ena %0h)",
                                  h.a_address, ena));
      if (h.a_address < MetaBase || h.a_address >= MetaTop)
        cms_error(Name, $sformatf("meta SRAM access 0x%08x outside [0x%08x, 0x%08x)",
                                  h.a_address, MetaBase, MetaTop));
      if (h.a_size != 2'd2 || h.a_mask != 4'hf || h.a_address[1:0] != 2'b00 ||
          !(h.a_opcode inside {tlul_pkg::Get, tlul_pkg::PutFullData}))
        cms_error(Name, $sformatf("meta SRAM access not a full-word Get/PutFullData: %s 0x%08x "
                                  , h.a_opcode.name(), h.a_address));
    end
    if (Kind == KindTbre && sb != null) begin
      tbre_rd_t rd;
      rd.addr    = h.a_address;
      rd.p_cycle = pres_cycle;
      rd.a_cycle = cms_cycle;
      sb.on_tbre_read(rd, h.a_opcode);
    end
  endfunction

  always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
      rsp_q.delete();
      pres_valid = 0;
      tl_o <= '{a_ready: 1'b0, d_opcode: tlul_pkg::AccessAck, d_user: tlul_pkg::TL_D_USER_DEFAULT,
                default: '0};
    end else begin
      // First cycle of the presented request (a new address restarts it)
      if (tl_i.a_valid && (!pres_valid || pres_addr != tl_i.a_address)) begin
        pres_valid = 1;
        pres_cycle = cms_cycle;
        pres_addr  = tl_i.a_address;
        pres_seen[tl_i.a_address] = cms_cycle;
      end else if (!tl_i.a_valid) begin
        pres_valid = 0;
      end
      // D channel handshake
      if (tl_o.d_valid && tl_i.d_ready) begin
        rsp_t r;
        r = rsp_q.pop_front();
        last_rsp_addr = r.addr;
        last_rsp_op   = r.opcode;
        -> rsp_ev;
      end
      // A channel handshake
      if (tl_i.a_valid && tl_o.a_ready) begin
        rsp_t        r;
        bit          wr;
        logic [31:0] old_w;
        check_req(tl_i, ena_i);
        pres_valid = 0;
        wr = tl_i.a_opcode != tlul_pkg::Get;
        r.opcode = tl_i.a_opcode;
        r.size   = tl_i.a_size;
        r.source = tl_i.a_source;
        r.addr   = tl_i.a_address;
        r.inj    = match_injection(tl_i.a_address, wr);
        r.err    = r.inj == int'(InjErr) ||
                   (Kind != KindMeta && tl_i.a_address >= DataErrBase && tl_i.a_address < UntaggedTop) ||
                   (Kind != KindMeta && wr && in_nvm(tl_i.a_address)) ||
                   (Kind == KindMeta && (tl_i.a_address < MetaBase || tl_i.a_address >= MetaTop));
        r.rdata  = 32'h0;
        if (wr) begin
          n_writes++;
          if (!r.err && !(Kind == KindTbre)) begin
            old_w = store.read(tl_i.a_address);
            store.write(tl_i.a_address, tl_i.a_data, tl_i.a_mask);
            if (Kind == KindMeta && sb != null)
              sb.on_meta_write({tl_i.a_address[31:2], 2'b00}, old_w, store.read(tl_i.a_address));
          end
        end else begin
          n_reads++;
          r.rdata = r.err ? 32'hffff_ffff : store.read(tl_i.a_address);
        end
        r.ready_cycle = cms_cycle + 64'((lat_max > lat_min) ? $urandom_range(lat_max, lat_min) : lat_min);
        rsp_q.push_back(r);
      end
      // Drive the next cycle
      if (rsp_q.size() > 0 && cms_cycle + 1 >= rsp_q[0].ready_cycle) tl_o <= make_rsp(rsp_q[0]);
      else tl_o.d_valid <= 1'b0;
      tl_o.a_ready <= (rsp_q.size() < Depth) && ($urandom_range(99) < a_ready_pct);
    end
  end

endmodule
