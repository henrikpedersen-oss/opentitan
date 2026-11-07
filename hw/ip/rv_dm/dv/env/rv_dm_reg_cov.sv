// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Functional coverage of rv_dm's registers, field by field (debug_ctrl_verification_spec.md,
// "Register fields" and "Coverage"). One covergroup per register or register group, each reported
// per instance. Sampled by rv_dm_cmodel_sb (on with +rv_dm_cmodel=1 and coverage enabled), which sees
// every DMI request and response, every access of the core to the debug memory, every system bus
// request and response, the JTAG scans and the mode inputs.
//
// Field positions are the Debug spec 0.13.2's (dmcontrol 3.12.2, dmstatus 3.12.1, hartinfo 3.12.3,
// abstractcs 3.12.6, command 3.12.7 / 3.6.1.1, abstractauto 3.12.8, sbcs 3.12.18, dtmcs 6.1.4,
// dmi 6.1.5); the debug memory offsets are PULP's "Debug Memory Map" and rv_dm.hjson's `mem` window.
class rv_dm_reg_cov extends uvm_object;
  `uvm_object_utils(rv_dm_reg_cov)

  // Encodings of a multibit input, for the covergroups: 0 Off/False, 1 On/True, 2 any other value.
  typedef enum bit [1:0] { MbOff = 0, MbOn = 1, MbInvalid = 2 } mb_e;

  // ---- Context the samples use ----
  protected bit        halted;          // dmstatus.allhalted at the last read
  protected bit        busy;            // abstractcs.busy at the last read
  protected bit [31:0] sbcs_w;          // the last value written to sbcs (the access fields)
  protected bit [31:0] abstractauto_w;  // the last value written to abstractauto
  protected bit        cmd_pending;     // a command written, its abstractcs read still to come
  protected bit [31:0] cmd_w;
  protected bit        cmd_cheriot, cmd_halted;
  protected bit [4:0]  tap_ir = 5'h01;  // IDCODE is selected after a TAP reset (IEEE 1149.1)
  protected bit [15:0] lc_last = '1;    // the mode inputs at the last sample (sampled on change)
  protected bit        lc_seen;

  // ---- DMI accesses (REQ_DCT_03, REQ_DCT_04) ----
  covergroup dmi_access_cg with function sample(bit [6:0] addr, bit write, bit [1:0] rsp);
    option.per_instance = 1;
    cp_addr: coverpoint addr {
      bins data_impl[]      = {[7'h04:7'h05]};
      bins data_unimpl      = {[7'h06:7'h0f]};
      bins dmcontrol        = {7'h10};
      bins dmstatus         = {7'h11};
      bins hartinfo         = {7'h12};
      bins haltsum1         = {7'h13};
      bins hawindow         = {[7'h14:7'h15]};
      bins abstractcs       = {7'h16};
      bins command          = {7'h17};
      bins abstractauto     = {7'h18};
      bins confstrptr       = {[7'h19:7'h1c]};
      bins nextdm           = {7'h1d};
      bins custom           = {7'h1f};
      bins progbuf_impl[]   = {[7'h20:7'h27]};
      bins progbuf_unimpl   = {[7'h28:7'h2f]};
      bins authdata         = {7'h30};
      bins dmcs2            = {7'h32};
      bins haltsum2_3       = {[7'h34:7'h35]};
      bins sbaddress3       = {7'h37};
      bins sbcs             = {7'h38};
      bins sbaddress0       = {7'h39};
      bins sbaddress1_2     = {[7'h3a:7'h3b]};
      bins sbdata0          = {7'h3c};
      bins sbdata1_2        = {[7'h3d:7'h3e]};
      bins sbdata3          = {7'h3f};
      bins haltsum0         = {7'h40};
      bins unassigned       = {[7'h00:7'h03], 7'h1e, 7'h31, 7'h33, 7'h36, [7'h41:7'h7f]};
    }
    cp_op: coverpoint write {
      bins read  = {1'b0};
      bins write = {1'b1};
    }
    cp_rsp: coverpoint rsp {
      bins ok          = {2'd0};
      bins failed      = {2'd2};
      bins in_progress = {2'd3};
    }
    x_addr_op: cross cp_addr, cp_op;
    x_op_rsp:  cross cp_op, cp_rsp;
  endgroup

  // ---- dmcontrol (REQ_DCT_05, REQ_DCT_06, REQ_DCT_09, REQ_DCT_35) ----
  covergroup dmcontrol_cg with function sample(bit [31:0] v, bit hart_halted);
    option.per_instance = 1;
    cp_haltreq:         coverpoint v[31];
    cp_resumereq:       coverpoint v[30];
    cp_hartreset:       coverpoint v[29];
    cp_ackhavereset:    coverpoint v[28];
    cp_hasel:           coverpoint v[26];
    cp_hartsel:         coverpoint {v[15:6], v[25:16]} {
      bins zero    = {0};
      bins nonzero = {[1:$]};
    }
    cp_setresethaltreq: coverpoint v[3];
    cp_clrresethaltreq: coverpoint v[2];
    cp_ndmreset:        coverpoint v[1];
    cp_dmactive:        coverpoint v[0];
    cp_halted:          coverpoint hart_halted;
    x_req_active:  cross cp_haltreq, cp_resumereq, cp_dmactive;
    x_req_halted:  cross cp_haltreq, cp_resumereq, cp_halted;
    x_ndm_active:  cross cp_ndmreset, cp_dmactive;
    x_ack_active:  cross cp_ackhavereset, cp_dmactive;
  endgroup

  // ---- dmstatus (REQ_DCT_07, REQ_DCT_36) ----
  covergroup dmstatus_cg with function sample(bit [31:0] v);
    option.per_instance = 1;
    cp_version:         coverpoint v[3:0]  { bins v0_13 = {4'd2}; }
    cp_confstrptrvalid: coverpoint v[4]    { bins zero = {1'b0}; }
    cp_hasresethaltreq: coverpoint v[5]    { bins zero = {1'b0}; }
    cp_authbusy:        coverpoint v[6]    { bins zero = {1'b0}; }
    cp_authenticated:   coverpoint v[7]    { bins one = {1'b1}; }
    cp_impebreak:       coverpoint v[22]   { bins zero = {1'b0}; }
    cp_halted:          coverpoint v[9];
    cp_running:         coverpoint v[11];
    cp_unavail:         coverpoint v[13];
    cp_nonexistent:     coverpoint v[15]   { bins zero = {1'b0}; }
    cp_resumeack:       coverpoint v[17];
    cp_havereset:       coverpoint v[19];
    // The hart's state as dmstatus reports it: halted, running, unavailable.
    x_state:     cross cp_halted, cp_running, cp_unavail {
      ignore_bins both = binsof(cp_halted) intersect {1} && binsof(cp_running) intersect {1};
      ignore_bins none_avail = binsof(cp_halted) intersect {0} && binsof(cp_running) intersect {0} &&
                               binsof(cp_unavail) intersect {0};
      ignore_bins unavail_busy = binsof(cp_unavail) intersect {1} &&
                                 (binsof(cp_halted) intersect {1} || binsof(cp_running) intersect {1});
    }
    x_resumeack: cross cp_halted, cp_resumeack;
    x_havereset: cross cp_halted, cp_havereset;
  endgroup

  // ---- hartinfo (REQ_DCT_08) ----
  covergroup hartinfo_cg with function sample(bit [31:0] v);
    option.per_instance = 1;
    cp_dataaddr:   coverpoint v[11:0]  { bins documented = {12'h380}; }
    cp_datasize:   coverpoint v[15:12] { bins two = {4'd2}; }
    cp_dataaccess: coverpoint v[16]    { bins one = {1'b1}; }
    cp_nscratch:   coverpoint v[23:20] { bins two = {4'd2}; }
  endgroup

  // ---- abstractcs (REQ_DCT_10, REQ_DCT_37) ----
  covergroup abstractcs_cg with function sample(bit [31:0] v, bit write, bit busy_ctx);
    option.per_instance = 1;
    cp_op:          coverpoint write;
    cp_busy_ctx:    coverpoint busy_ctx;
    cp_datacount:   coverpoint v[3:0]   iff (!write) { bins two = {4'd2}; }
    cp_progbufsize: coverpoint v[28:24] iff (!write) { bins eight = {5'd8}; }
    cp_busy:        coverpoint v[12]    iff (!write);
    cp_cmderr:      coverpoint v[10:8]  iff (!write) {
      bins none          = {3'd0};
      bins busy          = {3'd1};
      bins not_supported = {3'd2};
      bins exception     = {3'd3};
      bins halt_resume   = {3'd4};
      bins other         = {3'd7};
      // 5 (bus) is for Access Memory, which rv_dm does not implement; 6 is reserved.
      ignore_bins unused = {3'd5, 3'd6};
    }
    // What a write clears (cmderr is write-1-to-clear, bit by bit).
    cp_cmderr_clear: coverpoint v[10:8] iff (write) {
      bins none    = {3'd0};
      bins partial = {[3'd1:3'd6]};
      bins all     = {3'd7};
    }
    x_op_busy: cross cp_op, cp_busy_ctx;
  endgroup

  // ---- command (REQ_DCT_11, REQ_DCT_12, REQ_DCT_29, REQ_DCT_38) ----
  covergroup command_cg with function sample(bit [31:0] v, bit cheriot, bit hart_halted,
                                             bit busy_ctx);
    option.per_instance = 1;
    cp_cmdtype: coverpoint v[31:24] {
      bins access_register = {8'd0};
      bins quick_access    = {8'd1};
      bins access_memory   = {8'd2};
      bins other           = {[8'd3:8'd255]};
    }
    cp_aarsize: coverpoint v[22:20] iff (v[31:24] == 8'd0) {
      bins s8_s16    = {[3'd0:3'd1]};
      bins s32       = {3'd2};
      bins s64       = {3'd3};
      bins s128      = {3'd4};
      bins reserved  = {[3'd5:3'd7]};
    }
    cp_postincrement: coverpoint v[19] iff (v[31:24] == 8'd0);
    cp_postexec:      coverpoint v[18] iff (v[31:24] == 8'd0);
    cp_transfer:      coverpoint v[17] iff (v[31:24] == 8'd0);
    cp_write:         coverpoint v[16] iff (v[31:24] == 8'd0);
    cp_regno: coverpoint v[15:0] iff (v[31:24] == 8'd0) {
      bins csr_low_scr = {[16'h0000:16'h001f]};  // CSRs; the SCRs with aarsize 3 in CHERIoT mode
      bins csr         = {[16'h0020:16'h0fff]};
      bins gpr_x0_x15  = {[16'h1000:16'h100f]};  // the CHERIoT GPRs
      bins gpr_x16_x31 = {[16'h1010:16'h101f]};  // RV32I only: must fail in CHERIoT mode
      bins fpr         = {[16'h1020:16'h103f]};
      bins reserved    = {[16'h1040:16'hbfff]};
      bins custom      = {[16'hc000:16'hffff]};
    }
    // CSRs CHERIoT mode treats apart (DM SW plan): mtvec and mepc fail, mshwm and mshwmb work.
    cp_csr_special: coverpoint v[15:0] iff (v[31:24] == 8'd0 && v[22:20] <= 3'd2) {
      bins mtvec  = {16'h0305};
      bins mepc   = {16'h0341};
      bins mshwm  = {16'h0bc1};
      bins mshwmb = {16'h0bc2};
    }
    cp_mode:   coverpoint cheriot { bins rv32 = {1'b0}; bins cheriot = {1'b1}; }
    cp_halted: coverpoint hart_halted;
    cp_busy:   coverpoint busy_ctx;
    x_size_reg_mode: cross cp_aarsize, cp_regno, cp_mode;
    x_flags:         cross cp_transfer, cp_write, cp_postexec;
    x_type_halted:   cross cp_cmdtype, cp_halted;
    x_type_busy:     cross cp_cmdtype, cp_busy;
    x_postinc_mode:  cross cp_postincrement, cp_mode;
    x_csr_mode:      cross cp_csr_special, cp_mode;
  endgroup

  // ---- The outcome of a command: its kind against the state, and the error it got (REQ_DCT_11) ----
  covergroup command_result_cg with function sample(int unsigned kind, bit cheriot, bit hart_halted,
                                                    bit [2:0] cmderr);
    option.per_instance = 1;
    cp_kind: coverpoint kind {
      bins supported     = {0};  // Access Register, a size and register the mode supports
      bins other_type    = {1};  // Quick Access, Access Memory or a reserved type
      bins bad_size      = {2};  // transfer with a size the mode does not support
      bins bad_register  = {3};  // aarsize 3 in CHERIoT mode, regno 0x20-0xfff or beyond the GPRs
      bins no_transfer   = {4};  // postexec only (transfer 0)
      bins gpr_x16_x31   = {5};  // CHERIoT mode, regno 0x1010-0x101f
      bins csr_replaced  = {6};  // CHERIoT mode, mtvec or mepc
      bins scr_small     = {7};  // CHERIoT mode, 8-32 bits at regno 0x0-0x1f
    }
    // The outcome the documents give for each kind (REQ_DCT_11, REQ_DCT_45); only those pairs are
    // bins: a pair the documents do not allow is a checking matter (the model), not coverage.
    cp_outcome: coverpoint (kind * 8 + int'(cmderr)) {
      bins supported_done        = {0 * 8 + 0};
      bins supported_exception   = {0 * 8 + 3};
      bins supported_not_halted  = {0 * 8 + 4};
      bins other_type_refused    = {1 * 8 + 2};
      bins bad_size_refused      = {2 * 8 + 2};
      bins bad_register_refused  = {3 * 8 + 2};
      bins no_transfer_done      = {4 * 8 + 0};
      bins gpr_x16_x31_fails     = {5 * 8 + 2, 5 * 8 + 3};
      bins csr_replaced_fails    = {6 * 8 + 3};
      bins scr_small_fails       = {7 * 8 + 2, 7 * 8 + 3};
    }
    cp_mode:   coverpoint cheriot;
    cp_halted: coverpoint hart_halted;
    cp_cmderr: coverpoint cmderr {
      bins none          = {3'd0};
      bins busy          = {3'd1};
      bins not_supported = {3'd2};
      bins exception     = {3'd3};
      bins halt_resume   = {3'd4};
      ignore_bins unused = {3'd5, 3'd6, 3'd7};
    }
    x_kind_halted: cross cp_kind, cp_halted;
    x_kind_mode:   cross cp_kind, cp_mode;
    x_err_mode:    cross cp_cmderr, cp_mode;
  endgroup

  // ---- abstractauto (REQ_DCT_12, REQ_DCT_39) ----
  covergroup abstractauto_cg with function sample(bit [31:0] v);
    option.per_instance = 1;
    cp_autoexecdata:    coverpoint v[1:0];
    cp_data_unimpl:     coverpoint (v[11:2] != 0) { bins zero = {1'b0}; bins set = {1'b1}; }
    cp_autoexecprogbuf: coverpoint v[23:16] {
      bins none = {8'h00};
      bins some = {[8'h01:8'hfe]};
      bins all  = {8'hff};
    }
    cp_progbuf_unimpl:  coverpoint (v[31:24] != 0) { bins zero = {1'b0}; bins set = {1'b1}; }
  endgroup

  // ---- data0-11 and progbuf0-15 accesses, and the accesses abstractauto re-runs on
  //      (REQ_DCT_04, REQ_DCT_12, REQ_DCT_14, REQ_DCT_39) ----
  covergroup data_progbuf_cg with function sample(bit [6:0] addr, bit write, bit busy_ctx,
                                                  bit autoexec);
    option.per_instance = 1;
    cp_word: coverpoint addr {
      bins data_impl[]    = {[7'h04:7'h05]};
      bins data_unimpl    = {[7'h06:7'h0f]};
      bins progbuf_impl[] = {[7'h20:7'h27]};
      bins progbuf_unimpl = {[7'h28:7'h2f]};
    }
    cp_op:       coverpoint write;
    cp_busy:     coverpoint busy_ctx;
    cp_autoexec: coverpoint autoexec;
    x_word_op:       cross cp_word, cp_op;
    x_word_busy:     cross cp_word, cp_busy;
    x_word_autoexec: cross cp_word, cp_op, cp_autoexec {
      ignore_bins unimpl = binsof(cp_word.data_unimpl) || binsof(cp_word.progbuf_unimpl);
    }
  endgroup

  // ---- haltsum0-3 (REQ_DCT_36) ----
  covergroup haltsum_cg with function sample(bit [6:0] addr, bit hart_halted);
    option.per_instance = 1;
    cp_reg: coverpoint addr {
      bins haltsum0 = {7'h40};
      bins haltsum1 = {7'h13};
      bins haltsum2 = {7'h34};
      bins haltsum3 = {7'h35};
    }
    cp_halted: coverpoint hart_halted;
    x_reg_halted: cross cp_reg, cp_halted;
  endgroup

  // ---- sbcs (REQ_DCT_18-20, REQ_DCT_40) ----
  covergroup sbcs_cg with function sample(bit [31:0] v, bit write);
    option.per_instance = 1;
    // Written fields.
    cp_readonaddr:    coverpoint v[20] iff (write);
    cp_sbaccess:      coverpoint v[19:17] iff (write) {
      bins b8       = {3'd0};
      bins b16      = {3'd1};
      bins b32      = {3'd2};
      bins b64      = {3'd3};
      bins b128     = {3'd4};
      bins reserved = {[3'd5:3'd7]};
    }
    cp_autoincrement: coverpoint v[16] iff (write);
    cp_readondata:    coverpoint v[15] iff (write);
    cp_sberror_clear: coverpoint v[14:12] iff (write) {
      bins none    = {3'd0};
      bins partial = {[3'd1:3'd6]};
      bins all     = {3'd7};
    }
    cp_busyerror_clear: coverpoint v[22] iff (write);
    cp_reserved_write:  coverpoint (v[28:23] != 0) iff (write) {
      bins zero = {1'b0};
      bins set  = {1'b1};
    }
    x_access_mode: cross cp_sbaccess, cp_readonaddr, cp_readondata, cp_autoincrement;
    // Read fields.
    cp_sbversion: coverpoint v[31:29] iff (!write) { bins one = {3'd1}; }
    cp_sbbusy:    coverpoint v[21]    iff (!write);
    cp_busyerror: coverpoint v[22]    iff (!write);
    cp_sberror:   coverpoint v[14:12] iff (!write) {
      bins none      = {3'd0};
      bins address   = {3'd2};
      bins alignment = {3'd3};
      bins size      = {3'd4};
      bins other     = {3'd7};
      // 1 (timeout) is never reported by rv_dm (no timeout); 5 and 6 are reserved.
      ignore_bins unused = {3'd1, 3'd5, 3'd6};
    }
    cp_sbasize:   coverpoint v[11:5]  iff (!write) { bins b32 = {7'd32}; }
    cp_sizes:     coverpoint v[4:0]   iff (!write) { bins b8_16_32 = {5'b00111}; }
    x_busy_err:   cross cp_sbbusy, cp_busyerror;
  endgroup

  // ---- What starts a system bus access, and accesses while it is busy (REQ_DCT_18, REQ_DCT_19,
  //      REQ_DCT_40) ----
  covergroup sba_trigger_cg with function sample(bit [6:0] addr, bit write, bit readonaddr,
                                                 bit readondata, bit autoincrement, bit busy_ctx);
    option.per_instance = 1;
    cp_reg: coverpoint addr {
      bins sbaddress0   = {7'h39};
      bins sbaddress1_2 = {[7'h3a:7'h3b]};
      bins sbaddress3   = {7'h37};
      bins sbdata0      = {7'h3c};
      bins sbdata1_2    = {[7'h3d:7'h3e]};
      bins sbdata3      = {7'h3f};
    }
    cp_start_reg: coverpoint addr {
      bins sbaddress0 = {7'h39};
      bins sbdata0    = {7'h3c};
    }
    cp_op:            coverpoint write;
    cp_readonaddr:    coverpoint readonaddr;
    cp_readondata:    coverpoint readondata;
    cp_autoincrement: coverpoint autoincrement;
    cp_busy:          coverpoint busy_ctx;
    x_reg_op:   cross cp_reg, cp_op;
    x_start:    cross cp_start_reg, cp_op, cp_readonaddr, cp_readondata, cp_autoincrement;
    x_reg_busy: cross cp_start_reg, cp_op, cp_busy;
  endgroup

  // ---- System bus requests and responses (REQ_DCT_18, REQ_DCT_20) ----
  covergroup sba_bus_cg with function sample(bit write, bit [3:0] mask, bit err);
    option.per_instance = 1;
    cp_dir:  coverpoint write { bins read = {1'b0}; bins write = {1'b1}; }
    cp_size: coverpoint mask {
      bins byte_lane[] = {4'b0001, 4'b0010, 4'b0100, 4'b1000};
      bins half_lane[] = {4'b0011, 4'b1100};
      bins word        = {4'b1111};
    }
    cp_err:  coverpoint err;
    x_dir_size_err: cross cp_dir, cp_size, cp_err;
  endgroup

  // ---- The core's side: the debug memory (REQ_DCT_13-17, REQ_DCT_41) ----
  covergroup mem_cg with function sample(bit [11:0] off, bit write, bit err);
    option.per_instance = 1;
    cp_region: coverpoint off {
      bins halted      = {12'h100};
      bins going       = {12'h108};
      bins resuming    = {12'h110};
      bins exception   = {12'h118};
      bins whereto     = {12'h300};
      bins abstractcmd = {[12'h338:12'h35f]};
      bins progbuf     = {[12'h360:12'h37f]};
      bins data        = {[12'h380:12'h387]};
      bins flags       = {[12'h400:12'h7ff]};
      bins rom         = {[12'h800:12'hfff]};
    }
    cp_dir: coverpoint write { bins read = {1'b0}; bins write = {1'b1}; }
    cp_err: coverpoint err;
    x_region_dir: cross cp_region, cp_dir {
      // The acknowledge words are written by the core, the command words and the ROM only read.
      ignore_bins ack_read = binsof(cp_region) intersect {12'h100, 12'h108, 12'h110, 12'h118} &&
                             binsof(cp_dir.read);
      ignore_bins ro_write = binsof(cp_region) intersect {12'h300, [12'h338:12'h37f],
                                                         [12'h400:12'hfff]} &&
                             binsof(cp_dir.write);
    }
    x_region_err: cross cp_region, cp_err;
  endgroup

  // ---- The flags the parked core reads (REQ_DCT_15, REQ_DCT_41) ----
  covergroup flags_cg with function sample(bit [7:0] flags, bit hart_halted);
    option.per_instance = 1;
    cp_go:     coverpoint flags[0];
    cp_resume: coverpoint flags[1];
    cp_halted: coverpoint hart_halted;
    x_go_resume: cross cp_go, cp_resume {
      ignore_bins both = binsof(cp_go) intersect {1} && binsof(cp_resume) intersect {1};
    }
  endgroup

  // ---- The TAP's instruction register (REQ_DCT_01, REQ_DCT_42) ----
  covergroup tap_ir_cg with function sample(bit [4:0] ir);
    option.per_instance = 1;
    cp_ir: coverpoint ir {
      bins idcode = {5'h01};
      bins dtmcs  = {5'h10};
      bins dmi    = {5'h11};
      bins bypass = {5'h1f};
      bins unused = {5'h00, [5'h02:5'h0f], [5'h12:5'h1e]};
    }
  endgroup

  // ---- dtmcs (REQ_DCT_02, REQ_DCT_42) ----
  covergroup dtmcs_cg with function sample(bit [31:0] wr, bit [31:0] rd);
    option.per_instance = 1;
    cp_dmireset:     coverpoint wr[16];
    cp_dmihardreset: coverpoint wr[17];
    cp_version:      coverpoint rd[3:0]   { bins v0_13 = {4'd1}; }
    cp_abits:        coverpoint rd[9:4]   { bins seven = {6'd7}; }
    cp_dmistat:      coverpoint rd[11:10] {
      bins ok          = {2'd0};
      bins failed      = {2'd2};
      bins in_progress = {2'd3};
    }
    cp_idle:         coverpoint rd[14:12] { bins one = {3'd1}; }
    x_reset_stat: cross cp_dmireset, cp_dmistat;
    x_hard_stat:  cross cp_dmihardreset, cp_dmistat;
  endgroup

  // ---- The mode inputs and the regs interface's LATE_DEBUG_ENABLE (REQ_DCT_23, REQ_DCT_24,
  //      REQ_DCT_28, REQ_DCT_43) ----
  covergroup mode_cg with function sample(mb_e hw, mb_e dft, mb_e pin, mb_e otp, mb_e late,
                                          bit regwen, mb_e cheriot_ena);
    option.per_instance = 1;
    cp_hw:      coverpoint hw;
    cp_dft:     coverpoint dft;
    cp_pin:     coverpoint pin;
    cp_otp:     coverpoint otp;
    cp_late:    coverpoint late;
    cp_regwen:  coverpoint regwen;
    cp_cheriot: coverpoint cheriot_ena;
    // DEV (debug enabled, DFT not): the late-debug sources.
    cp_otp_dev:  coverpoint otp  iff (hw == MbOn && dft != MbOn);
    cp_late_dev: coverpoint late iff (hw == MbOn && dft != MbOn);
    x_hw_dft:      cross cp_hw, cp_dft;
    x_hw_pin:      cross cp_hw, cp_pin;
    x_dev_late:    cross cp_otp_dev, cp_late_dev;
    x_late_regwen: cross cp_late, cp_regwen;
  endgroup

  function new(string name = "rv_dm_reg_cov");
    super.new(name);
    dmi_access_cg     = new();
    dmcontrol_cg      = new();
    dmstatus_cg       = new();
    hartinfo_cg       = new();
    abstractcs_cg     = new();
    command_cg        = new();
    command_result_cg = new();
    abstractauto_cg   = new();
    data_progbuf_cg   = new();
    haltsum_cg        = new();
    sbcs_cg           = new();
    sba_trigger_cg    = new();
    sba_bus_cg        = new();
    mem_cg            = new();
    flags_cg          = new();
    tap_ir_cg         = new();
    dtmcs_cg          = new();
    mode_cg           = new();
    dmi_access_cg.set_inst_name({name, ".dmi_access_cg"});
    dmcontrol_cg.set_inst_name({name, ".dmcontrol_cg"});
    dmstatus_cg.set_inst_name({name, ".dmstatus_cg"});
    hartinfo_cg.set_inst_name({name, ".hartinfo_cg"});
    abstractcs_cg.set_inst_name({name, ".abstractcs_cg"});
    command_cg.set_inst_name({name, ".command_cg"});
    command_result_cg.set_inst_name({name, ".command_result_cg"});
    abstractauto_cg.set_inst_name({name, ".abstractauto_cg"});
    data_progbuf_cg.set_inst_name({name, ".data_progbuf_cg"});
    haltsum_cg.set_inst_name({name, ".haltsum_cg"});
    sbcs_cg.set_inst_name({name, ".sbcs_cg"});
    sba_trigger_cg.set_inst_name({name, ".sba_trigger_cg"});
    sba_bus_cg.set_inst_name({name, ".sba_bus_cg"});
    mem_cg.set_inst_name({name, ".mem_cg"});
    flags_cg.set_inst_name({name, ".flags_cg"});
    tap_ir_cg.set_inst_name({name, ".tap_ir_cg"});
    dtmcs_cg.set_inst_name({name, ".dtmcs_cg"});
    mode_cg.set_inst_name({name, ".mode_cg"});
  endfunction

  // The debug module was reset (dmactive 0 or the module's reset): the context starts again.
  function void reset();
    halted         = 1'b0;
    busy           = 1'b0;
    sbcs_w         = '0;
    abstractauto_w = '0;
    cmd_pending    = 1'b0;
  endfunction

  // A command's kind for command_result_cg (cp_kind's bins).
  protected static function int unsigned cmd_kind(bit [31:0] v, bit cheriot);
    bit [2:0]  aarsize = v[22:20];
    bit [15:0] regno   = v[15:0];
    if (v[31:24] != 8'd0) return 1;
    if (!v[17]) return 4;
    // Sizes up to the register's are supported: 32 bits in RV32I mode, 64 in CHERIoT mode.
    if (aarsize > (cheriot ? 3'd3 : 3'd2)) return 2;
    if (cheriot && regno inside {[16'h1010:16'h101f]}) return 5;
    if (cheriot && aarsize == 3'd3 &&
        !(regno inside {[16'h0000:16'h001f], [16'h1000:16'h100f]})) return 3;
    if (cheriot && aarsize <= 3'd2 && regno inside {16'h0305, 16'h0341}) return 6;
    if (cheriot && aarsize <= 3'd2 && regno inside {[16'h0000:16'h001f]}) return 7;
    return 0;
  endfunction

  // One DMI operation, at its response. `cheriot`: the mode at the request.
  function void sample_dmi(bit [6:0] addr, bit write, bit [1:0] rsp, bit [31:0] wdata,
                           bit [31:0] rdata, bit cheriot);
    dmi_access_cg.sample(addr, write, rsp);
    if (rsp != 2'd0) return;  // only operations the debug module completed
    if (addr inside {[7'h04:7'h0f], [7'h20:7'h2f]}) begin
      bit autoexec = (addr inside {[7'h04:7'h05]}) ? abstractauto_w[addr - 7'h04] :
                     (addr inside {[7'h20:7'h27]}) ? abstractauto_w[16 + addr - 7'h20] : 1'b0;
      data_progbuf_cg.sample(addr, write, busy, autoexec);
    end
    if (addr inside {7'h37, [7'h39:7'h3f]}) begin
      sba_trigger_cg.sample(addr, write, sbcs_w[20], sbcs_w[15], sbcs_w[16], busy);
    end
    if (write) begin
      case (addr)
        7'h10: begin
          dmcontrol_cg.sample(wdata, halted);
          if (!wdata[0]) reset();
        end
        7'h16: abstractcs_cg.sample(wdata, 1'b1, busy);
        7'h17: begin
          command_cg.sample(wdata, cheriot, halted, busy);
          if (!busy) begin
            cmd_pending = 1'b1;
            cmd_w       = wdata;
            cmd_cheriot = cheriot;
            cmd_halted  = halted;
          end
        end
        7'h18: begin
          abstractauto_cg.sample(wdata);
          abstractauto_w = wdata;
        end
        7'h38: begin
          sbcs_cg.sample(wdata, 1'b1);
          sbcs_w = wdata;
        end
        default: ;
      endcase
    end else begin
      case (addr)
        7'h11: begin
          dmstatus_cg.sample(rdata);
          halted = rdata[9];
        end
        7'h12: hartinfo_cg.sample(rdata);
        7'h16: begin
          abstractcs_cg.sample(rdata, 1'b0, busy);
          busy = rdata[12];
          if (cmd_pending && !rdata[12]) begin
            command_result_cg.sample(cmd_kind(cmd_w, cmd_cheriot), cmd_cheriot, cmd_halted,
                                     rdata[10:8]);
            cmd_pending = 1'b0;
          end
        end
        7'h13, 7'h34, 7'h35, 7'h40: haltsum_cg.sample(addr, halted);
        7'h38: sbcs_cg.sample(rdata, 1'b0);
        default: ;
      endcase
    end
  endfunction

  // One access of the core to the debug memory (offset from the window's base), at its response.
  function void sample_mem(bit [11:0] off, bit write, bit err, bit [31:0] rdata);
    mem_cg.sample(off, write, err);
    if (!write && !err && off inside {[12'h400:12'h7ff]}) begin
      flags_cg.sample(rdata[8 * off[1:0] +: 8], halted);
    end
  endfunction

  // One system bus request, at its response.
  function void sample_sba_bus(bit write, bit [3:0] mask, bit err);
    sba_bus_cg.sample(write, mask, err);
  endfunction

  // One JTAG scan: an IR scan selects a register; a DR scan of dtmcs is sampled.
  function void sample_jtag(bit is_ir, bit [4:0] ir, bit [31:0] dr_in, bit [31:0] dr_out);
    if (is_ir) begin
      tap_ir = ir;
      tap_ir_cg.sample(ir);
    end else if (tap_ir == 5'h10) begin
      dtmcs_cg.sample(dr_in, dr_out);
    end
  endfunction

  // The TAP was reset (TRST or the Test-Logic-Reset state): IDCODE is selected again.
  function void tap_reset();
    tap_ir = 5'h01;
  endfunction

  // The mode inputs; sampled when one of them changes.
  function void sample_mode(lc_ctrl_pkg::lc_tx_t hw, lc_ctrl_pkg::lc_tx_t dft,
                            lc_ctrl_pkg::lc_tx_t pin, prim_mubi_pkg::mubi8_t otp,
                            prim_mubi_pkg::mubi32_t late, bit regwen,
                            prim_mubi_pkg::mubi4_t cheriot_ena);
    mb_e c_hw   = lc_cat(hw);
    mb_e c_dft  = lc_cat(dft);
    mb_e c_pin  = lc_cat(pin);
    mb_e c_otp  = otp === prim_mubi_pkg::MuBi8True  ? MbOn  :
                  otp === prim_mubi_pkg::MuBi8False ? MbOff : MbInvalid;
    mb_e c_late = late === prim_mubi_pkg::MuBi32True  ? MbOn  :
                  late === prim_mubi_pkg::MuBi32False ? MbOff : MbInvalid;
    mb_e c_ch   = cheriot_ena === prim_mubi_pkg::MuBi4True  ? MbOn  :
                  cheriot_ena === prim_mubi_pkg::MuBi4False ? MbOff : MbInvalid;
    bit [15:0] now = {2'b0, c_hw, c_dft, c_pin, c_otp, c_late, c_ch, regwen, 1'b0};
    if (lc_seen && now == lc_last) return;
    lc_seen = 1'b1;
    lc_last = now;
    mode_cg.sample(c_hw, c_dft, c_pin, c_otp, c_late, regwen, c_ch);
  endfunction

  protected static function mb_e lc_cat(lc_ctrl_pkg::lc_tx_t v);
    return v === lc_ctrl_pkg::On ? MbOn : v === lc_ctrl_pkg::Off ? MbOff : MbInvalid;
  endfunction
endclass
