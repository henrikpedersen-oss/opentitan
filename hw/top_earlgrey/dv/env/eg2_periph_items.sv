// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Items of the EG2 per-peripheral scoreboards (compiled with EG2_PERIPH_SB only).
//
// The extended monitors (eg2_periph_monitors.sv) send the peripheral's own sequence items
// (uart_item, spi_item, i2c_item, eg2_gpio_item): the pin items as the actual side, the items
// made from the peripheral's TL-UL register traffic as the expected side. A control event of the
// expected stream (a FIFO reset, a reset of the block, a transfer the conversion cannot predict)
// travels in the same stream as an item of the same type, marked in eg2_periph_marker, so it
// keeps its place among the items.

class eg2_periph_marker;
  typedef enum int {
    MarkFlush,        // drop the pending expected items but the first `keep` (they may still be in
                      // flight: those become optional)
    MarkOptional,     // the item may be missing on the actual side (a byte the bench drove with
                      // a parity or framing error; an aborted transfer)
    MarkUnpredicted,  // from here on the stream cannot be predicted (e.g. a dual or quad SPI
                      // segment): the scoreboard counts and logs only, and says so
    MarkNack          // an I2C transfer was NACKed: the pending expected items become optional
  } kind_e;

  typedef struct {
    kind_e kind;
    int    keep;
    string why;
  } mark_t;

  protected static mark_t marks[uvm_object];

  static function void mark(uvm_object o, kind_e kind, int keep = 0, string why = "");
    mark_t m;
    m.kind = kind;
    m.keep = keep;
    m.why  = why;
    marks[o] = m;
  endfunction

  static function bit is_marked(uvm_object o, output mark_t m);
    if (!marks.exists(o)) return 1'b0;
    m = marks[o];
    return 1'b1;
  endfunction

  // A marker item carries no data: it is a control event only.
  static function bit is_control(uvm_object o);
    return marks.exists(o) && marks[o].kind != MarkOptional;
  endfunction

  static function void forget(uvm_object o);
    if (marks.exists(o)) marks.delete(o);
  endfunction
endclass

// GPIO as seen on the pads, in GPIO index space: for each GPIO routed to a pad of the bench's
// GPIO pad set (chip_if gpios_if), whether the chip drives that pad and the level it drives.
class eg2_gpio_item extends uvm_sequence_item;
  bit [31:0] routed;  // GPIOs that reach a pad of the bench's GPIO set (pinmux MIO_OUTSEL)
  bit [31:0] oe;      // the chip drives it
  bit [31:0] out;     // the level it drives (where oe)
  bit [31:0] known;   // the level was 0 or 1 (an X or Z level on a driven pad is an error)

  `uvm_object_utils_begin(eg2_gpio_item)
    `uvm_field_int(routed, UVM_DEFAULT)
    `uvm_field_int(oe,     UVM_DEFAULT)
    `uvm_field_int(out,    UVM_DEFAULT)
  `uvm_object_utils_end
  `uvm_object_new

  function string line();
    return $sformatf("%08h %08h routed %08h", out & oe & routed, oe & routed, routed);
  endfunction
endclass

// USB on the pads (usbdev's D+ and D-, VBUS): one item per bus event; and usbdev's receive side
// as the core reads it (eg2_usbdev_sb's RX direction).
class eg2_usb_item extends uvm_sequence_item;
  typedef enum int {
    UsbConnect, UsbDisconnect, UsbReset, UsbPacket, UsbSuspend,
    UsbRxEntry,   // an RXFIFO entry: endpoint, SETUP or OUT, size, buffer
    UsbBufRead    // a word of the packet buffer the core read
  } kind_e;
  kind_e   kind;
  bit      from_chip;   // a packet the chip sent (the DPI host was not driving)
  int      transitions; // J/K transitions in the packet
  realtime duration;
  bit [3:0] pid;        // the packet's PID (its low nibble), if its check nibble matched
  bit       pid_ok;
  bit [6:0] address;    // a token's device address and endpoint
  bit [3:0] endpoint;
  bit       token;
  bit [7:0] data[$];    // a data packet's payload (CRC16 removed)
  // Expected items only: the payload bytes the prediction fixes; `loose`: a register write raced
  // the decision, so a NAK (or, for a predicted NAK, any data packet) is accepted too; and what the
  // prediction rests on.
  bit       data_care[$];
  bit       loose;
  string    why;
  // UsbRxEntry and UsbBufRead.
  bit        setup;
  int        size;
  bit [4:0]  buffer;
  bit [10:0] offset;    // UsbBufRead: byte offset in the 2 kB packet buffer
  bit [31:0] word;
  bit [31:0] care = '1; // bits of `word` the prediction fixes

  `uvm_object_utils(eg2_usb_item)
  `uvm_object_new

  static function string pid_name(bit [3:0] p);
    case (p)
      4'h1: return "OUT";
      4'h9: return "IN";
      4'h5: return "SOF";
      4'hd: return "SETUP";
      4'h3: return "DATA0";
      4'hb: return "DATA1";
      4'h2: return "ACK";
      4'ha: return "NAK";
      4'he: return "STALL";
      default: return $sformatf("pid %h", p);
    endcase
  endfunction

  // A data packet (DATA0 or DATA1: the data toggle is not predicted).
  function bit is_data();
    return pid_ok && pid inside {4'h3, 4'hb};
  endfunction

  function string line();
    case (kind)
      UsbPacket: begin
        string s = $sformatf("packet %s %s%s", from_chip ? "chip->host" : "host->chip",
                             pid_ok ? pid_name(pid) : "?",
                             token ? $sformatf(" addr %0d ep %0d", address, endpoint) : "");
        if (is_data()) begin
          s = {s, $sformatf(" %0d bytes", data.size())};
          foreach (data[i]) begin
            s = {s, (i < data_care.size() && !data_care[i]) ? " --" : $sformatf(" %02h", data[i])};
          end
        end
        if (loose) s = {s, " (raced: or NAK)"};
        if (why != "") s = {s, " [", why, "]"};
        if (duration > 0) s = {s, $sformatf(" ; %0d transitions %0.3f us", transitions,
                                            duration / 1us)};
        return s;
      end
      UsbRxEntry: return $sformatf("< RXFIFO ep %0d %s size %0d buffer %0d", endpoint,
                                   setup ? "SETUP" : "OUT", size, buffer);
      UsbBufRead: return $sformatf("< buffer[%03h] %08h%s", offset, word,
                                   care == '1 ? "" : $sformatf(" (bits %08h predicted)", care));
      default:   return kind.name();
    endcase
  endfunction
endclass

// spi_device as its SPI host sees it (the spi_agent in host mode): one item per transaction (CSB
// low to high) with what the chip sent on MISO, as the per-byte expected or actual values; and
// spi_device's receive side as the core reads it (RX).
class eg2_spi_device_item extends uvm_sequence_item;
  typedef enum int {
    SpidFlash,    // a flash or passthrough command (opcode, address, payload)
    SpidTpm,      // a TPM transaction (command byte, address, data)
    SpidGeneric,  // bytes of a generic-mode transfer
    SpidRx        // a value the core read from the upload or TPM write side
  } kind_e;
  kind_e      kind;
  bit [7:0]   opcode;
  bit [31:0]  addr;
  int         addr_bytes;
  bit         write;        // the payload went from the host to the chip
  logic [7:0] data[$];      // the chip's bytes on MISO (a read), or the host's (a write)
  bit         care[$];      // expected items: the bytes the prediction fixes
  bit         pred = 1'b1;  // expected items: 0 = this transaction is NOT PREDICTED
  string      what;         // what serves it: "read buffer", "status", "jedec", ...
  // SpidRx.
  bit [31:0]  word;
  bit [31:0]  word_care = '1;

  `uvm_object_utils(eg2_spi_device_item)
  `uvm_object_new

  function string line();
    string s;
    if (kind == SpidRx) begin
      return $sformatf("< %s %08h%s", what, word,
                       word_care == '1 ? "" : $sformatf(" (bits %08h predicted)", word_care));
    end
    case (kind)
      SpidFlash:   s = $sformatf("flash %02h", opcode);
      SpidTpm:     s = $sformatf("tpm %s", write ? "write" : "read");
      SpidGeneric: s = "generic";
      default:     s = "?";
    endcase
    if (addr_bytes > 0) s = {s, $sformatf(" addr %0h", addr)};
    if (what != "") s = {s, " (", what, ")"};
    s = {s, $sformatf(" %s %0d bytes", write ? "in" : "out", data.size())};
    if (!pred) return {s, " NOT PREDICTED"};
    for (int i = 0; i < data.size() && i < 64; i++) begin
      s = {s, (i < care.size() && !care[i]) ? " --" : $sformatf(" %02h", data[i])};
    end
    if (data.size() > 64) s = {s, " ..."};
    return s;
  endfunction
endclass

// The system monitor's blocks (sysrst_ctrl, adc_ctrl, sensor_ctrl, the wake-up detectors): a
// block's outputs as a state (a new item when they change and have settled), an event the block
// sent (an alert), or a register the core read (RX), with the bits the prediction fixes.
class eg2_sysmon_item extends uvm_sequence_item;
  typedef enum int {
    SysState,   // the block's outputs, `value` packed as the block's item definition says
    SysEvent,   // one event (an alert sent): `name`
    SysRead     // a register the core read: `name`, `value`, `care`
  } kind_e;
  kind_e     kind;
  string     name;
  bit [31:0] value;
  bit [31:0] care = '1;
  string     fields;   // SysState: the bit names, for the log
  string     why;      // expected items: what the prediction rests on

  `uvm_object_utils(eg2_sysmon_item)
  `uvm_object_new

  function string line();
    string s;
    case (kind)
      SysState: s = $sformatf("%s %08h", name, value);
      SysEvent: s = $sformatf("event %s", name);
      SysRead:  s = $sformatf("< %s %08h", name, value);
      default:  s = "?";
    endcase
    if (care != '1) s = {s, $sformatf(" (bits %08h predicted)", care)};
    if (fields != "") s = {s, " ", fields};
    if (why != "") s = {s, " [", why, "]"};
    return s;
  endfunction
endclass

// ---------------------------------------------------------------------------------------------
// TL-UL error responses at a peripheral port: predicted from the documents, classified against
// what the port answered.

typedef enum int {
  Eg2TlOk,            // no error predicted, none returned
  Eg2TlExpectedErr,   // an error predicted (or injected on purpose and announced), and returned
  Eg2TlUnexpectedErr, // d_error with no prediction: an error
  Eg2TlMissingErr     // an error predicted, none returned: an error
} eg2_tl_err_e;

// One access at a peripheral's TL-UL port with its classification (the per-peripheral
// scoreboards' tl_fifo; the path check uses the same classification).
class eg2_tl_access_item extends uvm_sequence_item;
  string        inst;
  bit           write;
  bit [2:0]     opcode;
  bit [31:0]    addr;
  bit [3:0]     mask;
  bit [31:0]    data;        // write data or read data
  bit           tl_err;      // d_error as the port answered
  bit           tl_err_exp;  // an error was predicted for this access
  eg2_tl_err_e  cls;
  string        why;         // the prediction's reasons, or the injection
  realtime      t;

  `uvm_object_utils(eg2_tl_access_item)
  `uvm_object_new

  function string line();
    string c = cls == Eg2TlOk ? "OK" : cls == Eg2TlExpectedErr ? "EXPECTED_ERR" :
               cls == Eg2TlUnexpectedErr ? "UNEXPECTED_ERR" : "MISSING_ERR";
    return $sformatf("tl %s %s [%08h/%h]%s d_error %0d%s", c, write ? "write" : "read", addr, mask,
                     write ? $sformatf("=%08h", data) : "", tl_err,
                     why == "" ? "" : {" (", why, ")"});
  endfunction
endclass

// The prediction (a pure function of the access and the peripheral's register model) and the
// classification. The register model is the chip RAL's block of the peripheral, generated by
// reggen from the IP's hjson: the documents, not the RTL. The rules are those of OpenTitan's
// cip_base_scoreboard::predict_tl_err (hw/dv/sv/cip_lib), with the TL-UL specification's
// (doc/contributing/hw/... and hw/ip/tlul/doc) protocol and integrity rules:
//   - an address no register and no memory window of the block decodes (unless the block takes
//     unmapped accesses);
//   - a write that is not full-word to a block or window without byte enables, or to a register
//     of a block without sub-word writes that leaves one of the register's bytes out (reggen's
//     PERMIT check);
//   - a write to a read-only window, a read of a write-only window;
//   - an instruction fetch (a_user.instr_type) from a register of a block that does not allow it;
//   - a malformed request (opcode, size, mask, alignment; tl_seq_item::get_exp_d_error), a
//     malformed a_user.instr_type, or command or data integrity (a_user) that does not match.
// Not predicted: errors of a block's own logic that its register model does not describe (a
// locked or gated memory, a peripheral's busy state); they show as UNEXPECTED_ERR, to be resolved
// against the block's documentation.
class eg2_tl_err;
  protected static dv_base_reg_block blocks[string];
  protected static int unsigned      n_seen[string];

  static function void set_block(string inst, dv_base_reg_block blk);
    blocks[inst] = blk;
  endfunction

  static function bit has_block(string inst);
    return blocks.exists(inst);
  endfunction

  protected static function bit is_mem(dv_base_reg_block blk, uvm_reg_addr_t norm);
    addr_range_t r[$];
    blk.get_mem_ranges(r);
    foreach (r[i]) if (norm inside {[r[i].start_addr : r[i].end_addr]}) return 1'b1;
    return 1'b0;
  endfunction

  // Whether the documents predict an error for this access, and why.
  static function bit predict(string inst, tl_seq_item t, output string why);
    dv_base_reg_block   blk;
    uvm_reg_map         map;
    uvm_reg_addr_t      norm;
    uvm_reg             rg;
    bit                 mem;
    bit                 write = t.is_write();
    tlul_pkg::tl_a_user_t user = tlul_pkg::tl_a_user_t'(t.a_user);
    tlul_pkg::tl_h2d_t  h2d;
    why = "";
    // The TL-UL protocol and integrity, whatever the block.
    if (t.get_exp_d_error()) why = {why, "malformed request (opcode, size, mask or alignment); "};
    if (tlul_pkg::tl_a_user_chk(user)) why = {why, "malformed a_user.instr_type; "};
    h2d = '0;
    h2d.a_address = t.a_addr;
    h2d.a_opcode  = tlul_pkg::tl_a_op_e'(t.a_opcode);
    h2d.a_mask    = t.a_mask;
    h2d.a_user    = user;
    if (tlul_pkg::get_cmd_intg(h2d) != user.cmd_intg) why = {why, "command integrity; "};
    // a_user.data_intg covers a_data on every request (reads too).
    if (tlul_pkg::get_data_intg(t.a_data) != user.data_intg) begin
      why = {why, "data integrity; "};
    end
    if (!blocks.exists(inst)) return why != "";
    blk  = blocks[inst];
    map  = blk.get_default_map().get_root_map();
    norm = blk.get_normalized_addr(t.a_addr);
    mem  = is_mem(blk, norm);
    rg   = map.get_reg_by_offset(norm, 1'b0);
    if (!mem && rg == null && !blk.get_unmapped_access_ok()) begin
      why = {why, "no register or window at this address; "};
    end
    if (!blk.get_supports_byte_enable() && write && (t.a_size != 2 || t.a_mask != '1)) begin
      why = {why, "sub-word write to a block without byte enables; "};
    end
    if (mem) begin
      dv_base_mem dm;
      if ($cast(dm, map.get_mem_by_offset(norm))) begin
        if (!dm.get_mem_partial_write_support() && write && (t.a_size != 2 || t.a_mask != '1)) begin
          why = {why, "sub-word write to a window without byte enables; "};
        end
        if (dm.get_access() == "RO" && write) why = {why, "write to a read-only window; "};
        if (dm.get_access() == "WO" && !write) why = {why, "read of a write-only window; "};
      end
    end else if (rg != null) begin
      dv_base_reg       csr;
      dv_base_reg_block sub;
      if (write && $cast(csr, map.get_reg_by_offset(norm)) && $cast(sub, csr.get_parent()) &&
          !sub.get_supports_sub_word_csr_writes()) begin
        int unsigned lanes = (1 << (1 + csr.get_msb_pos() / 8)) - 1;
        if (lanes & ~t.a_mask) why = {why, "sub-word write to a register (reggen PERMIT); "};
      end
      if (!write && user.instr_type == prim_mubi_pkg::MuBi4True &&
          !dv_base_reg_block::reg_allows_fetch(rg)) begin
        why = {why, "instruction fetch from a register; "};
      end
    end
    return why != "";
  endfunction

  // Whether the documents refuse the access (the conversion applies no state for it).
  static function bit refused(string inst, tl_seq_item t);
    string why;
    return predict(inst, t, why);
  endfunction

  // The classification without side effects (the path check).
  static function eg2_tl_err_e pure_class(string inst, tl_seq_item t);
    string why;
    bit    exp = predict(inst, t, why);
    if (t.d_error) return exp ? Eg2TlExpectedErr : Eg2TlUnexpectedErr;
    return exp ? Eg2TlMissingErr : Eg2TlOk;
  endfunction

  // The classification of each access, once (eg2_periph_env's port thread). Checker fault
  // injection: +sb_corrupt=<inst>.tl_err flips the prediction of the n-th access at the port
  // (+sb_corrupt_at, default 1) and announces it, so its scoreboard must report it as EXPECTED;
  // +eg2_tl_err_flip=<inst> flips it without the announcement, so the run must fail.
  static function eg2_tl_access_item classify(string inst, tl_seq_item t);
    eg2_tl_access_item it = eg2_tl_access_item::type_id::create("tl");
    string             sb_corrupt, flip;
    int unsigned       at = 1;
    it.inst   = inst;
    it.write  = t.is_write();
    it.opcode = t.a_opcode;
    it.addr   = t.a_addr;
    it.mask   = t.a_mask;
    it.data   = it.write ? t.a_data : t.d_data;
    it.tl_err = t.d_error;
    it.t      = $realtime;
    it.tl_err_exp = predict(inst, t, it.why);
    if (!n_seen.exists(inst)) n_seen[inst] = 0;
    n_seen[inst]++;
    void'($value$plusargs("sb_corrupt=%s", sb_corrupt));
    void'($value$plusargs("eg2_tl_err_flip=%s", flip));
    void'($value$plusargs("sb_corrupt_at=%d", at));
    if (at == 0) at = 1;
    if (n_seen[inst] == at && (sb_corrupt == {inst, ".tl_err"} || flip == inst)) begin
      it.tl_err_exp = !it.tl_err_exp;
      it.why = {it.why, "prediction flipped by ",
                flip == inst ? "+eg2_tl_err_flip" : "+sb_corrupt", "; "};
      if (sb_corrupt == {inst, ".tl_err"}) begin
        void'(eg2_inject_registry::announce({"sb_corrupt ", inst, ".tl_err"}, inst,
                                            $sformatf("TL-UL error prediction of access %0d flipped",
                                                      n_seen[inst]), $realtime, $realtime));
      end
    end
    if (it.tl_err && it.tl_err_exp) it.cls = Eg2TlExpectedErr;
    else if (!it.tl_err && !it.tl_err_exp) it.cls = Eg2TlOk;
    else if (!it.tl_err) it.cls = Eg2TlMissingErr;
    else begin
      // An error response nobody predicted: expected only inside an announced injection.
      string src = eg2_inject_registry::consult(inst, it.t);
      if (src != "") begin
        it.cls = Eg2TlExpectedErr;
        it.why = {it.why, "injected by ", src, "; "};
      end else begin
        it.cls = Eg2TlUnexpectedErr;
      end
    end
    return it;
  endfunction
endclass
