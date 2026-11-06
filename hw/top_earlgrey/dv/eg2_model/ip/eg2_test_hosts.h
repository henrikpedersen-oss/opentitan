// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Host programs in the EG2 SystemC model: what the FPGA/silicon host test program does for a test
// whose software waits for a host (Henrik 2026-11-04: the model is self-contained). One program per
// host test, selected with --stim host.program=<name> (bare-metal/bench.txt), each following its
// host program in sw/host/tests/chip step by step. The image is the sim_dv build, so where the
// software's OTTF_WAIT_FOR spins on a variable (ottf_utils.h: kDeviceSimDV, "the environment
// change[s] the value of the variable"), the host's uJSON MemWrite of that variable is a write of
// the symbol, as the chip bench's sw_symbol_backdoor_overwrite does; where the software reads the
// console with uJSON whatever the device type (example_mem_ujcmd.c), the host sends the commands
// over the console UART.
//
// What the host sees and does:
//   log lines      the software's log messages (the DV log), as the host's UartConsole::wait_for
//                  sees the console; status words (InTest for the host's "Running ", Passed for
//                  "PASS!")
//   UART bytes     each byte a UART sends (Eg2System::on_emit) with its time; a host UART at a baud
//                  rate takes a byte only if the sender's bit rate is within the framing tolerance:
//                  an asynchronous receiver samples each bit at its middle from the start bit's
//                  edge, so over a 10-bit character the drift must stay under half a bit at the
//                  middle of the stop bit (9.5 bit times): 0.5 / 9.5 = 5.2 % (no parity; the uart's
//                  documents give the frame: start bit, 8 data bits, optional parity, stop bit)
//   UART bytes in  sent to uart<N>'s RX one character time apart at the host's baud rate
//   resets         a power-on reset (rstmgr.por), as transport.reset() does between the host's
//                  phases; a program that boots the chip several times takes each pass status but
//                  the last (Eg2System::host_takes_status)
//   uJSON          opentitanlib's test_utils/rpc.rs: the command and then {"crc":<n>}, the CRC-32
//                  (ISO-HDLC) of the JSON text; the answer is a console line RESP_OK:<json> CRC:<n>
//                  (or RESP_ERR); console flow control as opentitanlib's (the device's XOFF 0x13
//                  stops the host sending until its XON 0x11)
// Each host check prints `eg2sim: EG2CHK ok|FAIL host <what> <detail>` (Eg2Bench::check); a failed
// check fails the run, and a run that ends before the program's last step fails too, naming the
// step it waited for.
// Not modelled: the pads between the UART and the host (pinmux's routing to the DUT channel is not
// checked: the host is on the UART's own pins); the device UART's receiver takes the host's bytes
// whatever its own bit rate (the host's receiver checks the device's bit rate, so a wrong NCO is
// still caught, on the device's sending).

#ifndef EG2_MODEL_IP_EG2_TEST_HOSTS_H_
#define EG2_MODEL_IP_EG2_TEST_HOSTS_H_

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../eg2_system.h"
#include "eg2_periph_spi_device.h"

namespace eg2_model {

// CRC-32/ISO-HDLC (the crc crate's CRC_32_ISO_HDLC: reflected 0x04c11db7, init and xorout all ones).
inline uint32_t eg2_crc32(const std::string &s) {
  uint32_t c = 0xffffffffu;
  for (unsigned char b : s) {
    c ^= b;
    for (int i = 0; i < 8; ++i) c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
  }
  return c ^ 0xffffffffu;
}

class Eg2HostProgram {
 public:
  Eg2HostProgram(Eg2System &sys, std::string name) : sys_(sys), name_(std::move(name)) {}
  virtual ~Eg2HostProgram() = default;
  virtual void on_status(uint32_t) {}
  virtual void on_log(const std::string &) {}
  virtual void on_uart(const std::string &, uint8_t, uint64_t) {}
  // A pass status that ends one boot, not the run: the program resets the chip and goes on.
  virtual bool takes_pass() { return false; }
  // Empty once the program has done its last step; otherwise the step it waits for.
  std::string pending() const { return done_ ? std::string() : waiting_; }
  const std::string &name() const { return name_; }

 protected:
  static constexpr uint32_t kInTest = 0x4354, kPassed = 0x900d;
  static constexpr uint64_t kUs = 1000000;  // ps

  bool check(const std::string &what, bool ok, const std::string &detail = "") {
    if (!sys_.bench_check) return ok;
    return sys_.bench_check("host " + name_ + ": " + what, ok, detail);
  }
  void wait_for(const std::string &what) { waiting_ = what; }
  void finished() { done_ = true; waiting_.clear(); }
  // The software's copy of a symbol (its VMA: initialised data has been copied to SRAM), as the
  // host's MemRead/MemWrite at the ELF's symbol address reach it.
  bool sym_rw(bool write, const std::string &s, uint8_t *b, size_t n) {
    uint32_t a = 0;
    bool ok = sys_.test_symbol_addr && sys_.mem_rw && sys_.test_symbol_addr(s, &a) &&
              sys_.mem_rw(write, a, b, n);
    if (!ok) check("symbol " + s + (write ? " writable" : " readable"), false,
                   "not in the image or in no memory");
    return ok;
  }
  bool read_sym(const std::string &s, uint8_t *b, size_t n) { return sym_rw(false, s, b, n); }
  bool read_u32(const std::string &s, uint32_t *v) {
    uint8_t b[4];
    if (!read_sym(s, b, 4)) return false;
    *v = uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    return true;
  }
  bool write_sym(const std::string &s, std::vector<uint8_t> b) {
    return sym_rw(true, s, b.data(), b.size());
  }
  bool write_u8(const std::string &s, uint8_t v) { return write_sym(s, {v}); }
  bool write_u32(const std::string &s, uint32_t v) {
    return write_sym(s, {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)});
  }
  // Bytes into uart<N>'s RX, one character (10 bit times) apart at baud, starting one character
  // from now.
  void uart_send(const std::string &win, const std::vector<uint8_t> &bytes, uint32_t baud) {
    uint64_t ch = 10ull * 1000000000000ull / baud, t = sys_.now_ps();
    for (uint8_t b : bytes) {
      t += ch;
      sys_.at(t, [this, win, b] {
        auto it = sys_.inputs.find(win + ".rx");
        if (it == sys_.inputs.end()) {
          check(win + " exists", false, "no such UART input");
          return;
        }
        it->second(b);
      });
    }
  }
  void reset_chip(const std::string &why) {
    sys_.note("host " + name_ + ": power-on reset (" + why + ")");
    sys_.apply_stim("rstmgr.por", "1");
  }
  // Calls f every period_us until it returns true, at most tries times; then gives_up().
  void poll(uint64_t period_us, unsigned tries, std::function<bool()> f,
            std::function<void()> gives_up) {
    sys_.at(sys_.now_ps() + period_us * kUs, [this, period_us, tries, f, gives_up] {
      if (f()) return;
      if (tries <= 1) {
        gives_up();
        return;
      }
      poll(period_us, tries - 1, f, gives_up);
    });
  }
  // A host UART receiving at baud: the bytes a UART sent, and whether each came at the host's bit
  // rate (the spacing of back-to-back characters is one character time of the sender).
  struct Capture {
    std::string win;
    uint32_t baud = 0;
    bool on = false;
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> times;
    void start(const std::string &w, uint32_t b) {
      win = w;
      baud = b;
      on = true;
      bytes.clear();
      times.clear();
    }
    bool take(const std::string &w, uint8_t b, uint64_t t) {
      if (!on || w != win) return false;
      bytes.push_back(b);
      times.push_back(t);
      return true;
    }
    // The sender's bit rate from the shortest spacing (back-to-back characters), against the
    // host's: within 5.2 % (header comment), or no two characters to measure.
    bool rate_ok(std::string *detail) const {
      uint64_t best = 0;
      for (size_t i = 1; i < times.size(); ++i) {
        uint64_t d = times[i] - times[i - 1];
        if (d && (!best || d < best)) best = d;
      }
      if (!best) return *detail = "fewer than two characters", true;
      double sender = 10.0 * 1e12 / double(best), err = (sender - baud) / double(baud);
      char b[96];
      std::snprintf(b, sizeof b, "sender %.0f Bd, host %u Bd (%+.2f %%)", sender, baud,
                    err * 100);
      *detail = b;
      return err < 0.052 && err > -0.052;
    }
  };

  Eg2System &sys_;
  std::string name_;
  bool done_ = false;

 public:
  // --stim host.red=1: the host breaks its own expectation (a wrong phase, a changed byte, a baud
  // rate 10 % off), so that each kind of host check is seen to fail.
  bool red_ = false;

 protected:
  std::string waiting_ = "the test to start";
};

// sw/host/tests/chip/example_sival/main.rs: check_and_set_phase twice, then PASS.
class Eg2HostExampleSival : public Eg2HostProgram {
 public:
  explicit Eg2HostExampleSival(Eg2System &s) : Eg2HostProgram(s, "example_sival") {
    wait_for("\"Waiting for host\"");
  }
  void on_log(const std::string &m) override {
    if (step_ == 0 && m == "Waiting for host") {
      step_ = 1;
      handshake(0, 1, 2, "\"Change phase to kWaitHost2\"");
    } else if (step_ == 2 && m == "Change phase to kWaitHost2") {
      step_ = 3;
      handshake(2, 3, 4, "the PASS");
    }
  }
  void on_status(uint32_t w) override {
    if (w == kPassed && step_ == 4) finished();
  }

 private:
  // Phase as the host's MemRead32 sees it once the device is waiting (it reads until the device
  // has set the value, at most 10 ms), checked, then the next phase written.
  void handshake(uint32_t expect, uint32_t next, unsigned then, const std::string &wait) {
    wait_for("kPhase == " + std::to_string(expect));
    poll(10, 1000,
         [this, expect, next, then, wait] {
           uint32_t v = 0;
           if (!read_u32("kPhase", &v) || v != expect + (red_ ? 1u : 0u)) return false;
           check("kPhase", true, "reads " + std::to_string(v));
           write_u32("kPhase", next);
           step_ = then;
           wait_for(wait);
           return true;
         },
         [this, expect] {
           uint32_t v = 0;
           read_u32("kPhase", &v);
           check("kPhase", false,
                 "reads " + std::to_string(v) + " after 10 ms, not " +
                     std::to_string(expect + (red_ ? 1u : 0u)));
         });
  }
  unsigned step_ = 0;
};

// sw/host/tests/chip/uart/src/uart_loopback.rs: for each UART 0-3, after a reset: uart_idx, the
// line loopback (the host's "Loopback test!" must come back on TX) and the system loopback (the
// software checks it).
class Eg2HostUartLoopback : public Eg2HostProgram {
 public:
  explicit Eg2HostUartLoopback(Eg2System &s) : Eg2HostProgram(s, "uart_loopback") {
    wait_for("UART0: the test to start");
  }
  void on_status(uint32_t w) override {
    if (w == kInTest && step_ == 0) {
      step_ = 1;
      write_u8("uart_idx", uint8_t(idx_));
      wait_for("UART" + std::to_string(idx_) + " configured");
    }
  }
  void on_log(const std::string &m) override {
    std::string u = "UART" + std::to_string(idx_);
    if (step_ == 1 && m == u + " configured") {
      step_ = 2;
      write_u8("test_phase", 1);  // kTestPhaseLine
      wait_for("\"Testing the line loopback\"");
    } else if (step_ == 2 && m == "Testing the line loopback") {
      step_ = 3;
      cap_.start("uart" + std::to_string(idx_), kBaud);
      uart_send("uart" + std::to_string(idx_), kData, kBaud);
      wait_for(u + ": the loopback data on TX");
      poll(100, 100, [this] { return cap_.bytes.size() >= kData.size(); }, [this] {
        check("UART" + std::to_string(idx_) + " line loopback", false,
              std::to_string(cap_.bytes.size()) + " of 14 bytes came back after 10 ms");
      });
    }
  }
  void on_uart(const std::string &w, uint8_t b, uint64_t t) override {
    if (step_ != 3 || !cap_.take(w, b, t) || cap_.bytes.size() < kData.size()) return;
    cap_.on = false;
    std::string u = "UART" + std::to_string(idx_), d;
    std::vector<uint8_t> want = kData;
    if (red_) want[0] ^= 1;
    check(u + " line loopback data", cap_.bytes == want,
          "\"" + std::string(cap_.bytes.begin(), cap_.bytes.end()) + "\"");
    check(u + " line loopback bit rate", cap_.rate_ok(&d), d);
    step_ = 4;
    write_u8("test_phase", 2);  // kTestPhaseSystem
    wait_for(u + ": the PASS");
  }
  bool takes_pass() override {
    if (step_ != 4) return false;
    check("UART" + std::to_string(idx_) + " loopback: the device passed", true);
    if (idx_ == 3) {
      finished();
      return false;
    }
    ++idx_;
    step_ = 0;
    wait_for("UART" + std::to_string(idx_) + ": the test to start");
    reset_chip("next UART");
    return true;
  }

 private:
  static constexpr uint32_t kBaud = 1000000;  // device_sim_dv.c kUartBaudrate
  const std::vector<uint8_t> kData{'L', 'o', 'o', 'p', 'b', 'a', 'c', 'k', ' ', 't', 'e', 's', 't', '!'};
  unsigned idx_ = 0, step_ = 0;
  Capture cap_;
};

// sw/host/tests/chip/uart/src/uart_baud_rate.rs: for each UART 0-3, after a reset, every baud rate
// the software picks: the host reads baud_rate, receives kSendData at that rate, and sends it back.
class Eg2HostUartBaudRate : public Eg2HostProgram {
 public:
  explicit Eg2HostUartBaudRate(Eg2System &s) : Eg2HostProgram(s, "uart_baud_rate") {
    wait_for("UART0: the test to start");
  }
  void on_status(uint32_t w) override {
    if (w == kInTest && step_ == 0) {
      step_ = 1;
      uint8_t d[16];
      if (read_sym("kSendData", d, sizeof d)) data_.assign(d, d + sizeof d);
      write_u8("uart_idx", uint8_t(idx_));
      wait_for("\"Configured UART" + std::to_string(idx_) + " with Baud rate ...\"");
    }
  }
  void on_log(const std::string &m) override {
    std::string u = "UART" + std::to_string(idx_);
    if (step_ == 1 && m.rfind("Configured " + u + " with Baud rate ", 0) == 0) {
      uint32_t baud = 0;
      if (!read_u32("baud_rate", &baud) || baud == 0) return;
      baud_ = red_ ? baud + baud / 10 : baud;
      step_ = 2;
      cap_.start("uart" + std::to_string(idx_), baud_);
      write_u8("test_phase", 2);  // kTestPhaseSend
      wait_for(u + " at " + std::to_string(baud_) + " Bd: kSendData on TX");
      uint64_t limit_us = 2 * 10ull * 1000000 * data_.size() / baud_ + 1000;
      unsigned iter = ++iter_;
      poll(limit_us, 1, [] { return false; }, [this, u, iter] {
        if (step_ == 2 && iter == iter_) {
          check(u + " at " + std::to_string(baud_) + " Bd: data received", false,
                std::to_string(cap_.bytes.size()) + " of " + std::to_string(data_.size()) +
                    " bytes in time");
        }
      });
    }
  }
  void on_uart(const std::string &w, uint8_t b, uint64_t t) override {
    if (step_ != 2 || !cap_.take(w, b, t) || cap_.bytes.size() < data_.size()) return;
    cap_.on = false;
    std::string what = "UART" + std::to_string(idx_) + " at " + std::to_string(baud_) + " Bd", d;
    bool rate = cap_.rate_ok(&d);
    check(what + ": bit rate", rate, d);
    check(what + ": data", rate && cap_.bytes == data_, "16 bytes");
    step_ = 1;  // the next baud rate, or the PASS
    write_u8("test_phase", 3);  // kTestPhaseRecv
    uart_send("uart" + std::to_string(idx_), data_, baud_);
    wait_for("the next baud rate on UART" + std::to_string(idx_) + ", or the PASS");
  }
  bool takes_pass() override {
    if (step_ != 1) return false;
    check("UART" + std::to_string(idx_) + " baud rates: the device passed", true);
    if (idx_ == 3) {
      finished();
      return false;
    }
    ++idx_;
    step_ = 0;
    wait_for("UART" + std::to_string(idx_) + ": the test to start");
    reset_chip("next UART");
    return true;
  }

 private:
  unsigned idx_ = 0, step_ = 0, iter_ = 0;  // iter_: the baud rate being tested, for its timeout
  uint32_t baud_ = 0;
  std::vector<uint8_t> data_;
  Capture cap_;
};

// sw/host/tests/chip/mem/src/main.rs with opentitanlib's test_utils/mem.rs: the memory commands
// over the console in uJSON, with CRCs and flow control.
class Eg2HostMemUjcmd : public Eg2HostProgram {
 public:
  explicit Eg2HostMemUjcmd(Eg2System &s) : Eg2HostProgram(s, "example_mem_ujcmd") {}
  void on_status(uint32_t w) override {
    if (w != kInTest || started_) return;
    started_ = true;
    if (!sys_.test_symbol_addr || !sys_.test_symbol_addr("kTestWord", &word_) ||
        !sys_.test_symbol_addr("kTestBytes", &bytes_) ||
        !sys_.test_symbol_addr("kEndTest", &end_)) {
      check("symbols kTestWord, kTestBytes, kEndTest", false, "missing in the image");
      return;
    }
    run();
  }
  void on_uart(const std::string &w, uint8_t b, uint64_t) override {
    if (w != kConsole) return;
    if (b == 0x13) { xoff_ = true; return; }  // XOFF
    if (b == 0x11) {                           // XON
      xoff_ = false;
      pump();
      return;
    }
    if (b == '\n' || b == '\r') {
      if (!line_.empty()) console_line(line_);
      line_.clear();
    } else {
      line_ += char(b);
    }
  }

 private:
  using Resp = std::function<void(const std::string &)>;
  static constexpr uint32_t kBaud = 1000000;  // device_sim_dv.c kUartBaudrate (the console)
  const std::string kConsole = "uart0";

  // The host's steps, in main.rs's order.
  void run() {
    read32(word_, [this](uint32_t v) {
      check("MemRead32 kTestWord", v == 0xface1234u, hex(v));
      write32(word_, 0x12345678u, [this] {
        read32(word_, [this](uint32_t v) {
          check("MemWrite32 then MemRead32 kTestWord", v == 0x12345678u, hex(v));
          read(bytes_, 256, [this](const std::vector<uint8_t> &d) {
            std::vector<uint8_t> up(256);
            for (unsigned i = 0; i < 256; ++i) up[i] = uint8_t(i);
            check("MemRead kTestBytes", d == up, std::to_string(d.size()) + " bytes");
            std::vector<uint8_t> down(up.rbegin(), up.rend());
            write(bytes_, down, [this, down] {
              read(bytes_, 256, [this, down](const std::vector<uint8_t> &d) {
                check("MemWrite then MemRead kTestBytes", d == down,
                      std::to_string(d.size()) + " bytes");
                read32(end_, [this](uint32_t v) {
                  check("MemRead32 kEndTest", v == 0, hex(v));
                  write32(end_, 1, [this] {
                    sent_all_ = true;
                    wait_for("the PASS");
                  });
                });
              });
            });
          });
        });
      });
    });
  }
  void on_pass() { if (sent_all_) finished(); }

  static std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%08x", v);
    return b;
  }
  // opentitanlib's send_with_crc: the JSON, then {"crc":<n>}.
  void send_json(const std::string &j) {
    std::string s = j + "{\"crc\":" + std::to_string(eg2_crc32(j)) + "}";
    for (char c : s) out_.push_back(uint8_t(c));
    pump();
  }
  void command(const std::string &cmd, const std::string &req, const std::string &what, Resp r) {
    wait_for(what + ": its RESP_OK");
    resp_ = std::move(r);
    resp_what_ = what;
    send_json("\"" + cmd + "\"");
    send_json(req);
  }
  void read32(uint32_t a, std::function<void(uint32_t)> f) {
    command("MemRead32", "{\"address\":" + std::to_string(a) + "}", "MemRead32",
            [f](const std::string &j) { f(uint32_t(num(j, "\"value\":"))); });
  }
  void write32(uint32_t a, uint32_t v, std::function<void()> f) {
    command("MemWrite32",
            "{\"address\":" + std::to_string(a) + ",\"value\":" + std::to_string(v) + "}",
            "MemWrite32", [f](const std::string &) { f(); });
  }
  void read(uint32_t a, unsigned n, std::function<void(const std::vector<uint8_t> &)> f) {
    command("MemRead",
            "{\"address\":" + std::to_string(a) + ",\"data_len\":" + std::to_string(n) + "}",
            "MemRead", [f, n](const std::string &j) {
              std::vector<uint8_t> d = arr(j, "\"data\":[");
              long len = num(j, "\"data_len\":");
              if (len >= 0 && size_t(len) <= d.size()) d.resize(size_t(len));
              (void)n;
              f(d);
            });
  }
  void write(uint32_t a, const std::vector<uint8_t> &d, std::function<void()> f) {
    std::string j = "{\"address\":" + std::to_string(a) + ",\"data\":[";
    for (size_t i = 0; i < d.size(); ++i) j += (i ? "," : "") + std::to_string(d[i]);
    j += "],\"data_len\":" + std::to_string(d.size()) + "}";
    command("MemWrite", j, "MemWrite", [f](const std::string &) { f(); });
  }
  static long num(const std::string &j, const std::string &key) {
    size_t p = j.find(key);
    if (p == std::string::npos) return -1;
    return std::strtol(j.c_str() + p + key.size(), nullptr, 0);
  }
  static std::vector<uint8_t> arr(const std::string &j, const std::string &key) {
    std::vector<uint8_t> d;
    size_t p = j.find(key);
    if (p == std::string::npos) return d;
    const char *c = j.c_str() + p + key.size();
    while (*c && *c != ']') {
      char *e;
      long v = std::strtol(c, &e, 0);
      if (e == c) break;
      d.push_back(uint8_t(v));
      c = e;
      while (*c == ',' || *c == ' ') ++c;
    }
    return d;
  }
  // rpc.rs recv: RESP_OK:<json> CRC:<n>, the CRC checked.
  void console_line(const std::string &l) {
    size_t ok = l.find("RESP_OK:"), err = l.find("RESP_ERR:");
    if (ok == std::string::npos && err == std::string::npos) return;
    bool good = ok != std::string::npos;
    size_t b = good ? ok + 8 : err + 9, c = l.rfind(" CRC:");
    if (c == std::string::npos || c < b) return;
    std::string j = l.substr(b, c - b);
    uint32_t crc = uint32_t(std::strtoul(l.c_str() + c + 5, nullptr, 10));
    check(resp_what_ + ": response CRC", crc == eg2_crc32(j), j.substr(0, 60));
    check(resp_what_ + ": RESP_OK", good, j.substr(0, 60));
    Resp r;
    r.swap(resp_);
    if (r && good) r(j);
  }
  // One byte at a time at the console's rate, stopped while the device has sent XOFF.
  void pump() {
    if (pumping_ || xoff_ || out_.empty()) return;
    pumping_ = true;
    sys_.at(sys_.now_ps() + 10ull * 1000000000000ull / kBaud, [this] {
      pumping_ = false;
      if (xoff_ || out_.empty()) return;
      uint8_t b = out_.front();
      out_.pop_front();
      auto it = sys_.inputs.find(kConsole + ".rx");
      if (it != sys_.inputs.end()) it->second(b);
      pump();
    });
  }

 public:
  void status_after(uint32_t w) {
    if (w == kPassed) on_pass();
  }

 private:
  bool started_ = false, xoff_ = false, pumping_ = false, sent_all_ = false;
  uint32_t word_ = 0, bytes_ = 0, end_ = 0;
  std::deque<uint8_t> out_;
  std::string line_, resp_what_;
  Resp resp_;
};

// hw/top_earlgrey/dv/env/seq_lib/chip_sw_spi_device_tpm_vseq.sv (the SPI host agent's
// spi_host_tpm_seq on tpm_csb): after "SYNC: Begin TPM Test", 10 times a TPM write of 1-64 random
// bytes at a random 24-bit address, and after "SYNC: Waiting Read" a TPM read of as many bytes at
// that address, which must return the bytes written (the software loops them back). The host
// clocks each byte at the SPI agent's 20 MHz (header, one WAIT byte at a time until the device sends
// START, the TCG flow control, at most 10 ms, then the data). host.program=spi_tpm[:<seed>] (default seed 1).
class Eg2HostSpiTpm : public Eg2HostProgram {
 public:
  Eg2HostSpiTpm(Eg2System &s, uint32_t seed) : Eg2HostProgram(s, "spi_tpm"), rng_(seed) {
    wait_for("\"SYNC: Begin TPM Test\"");
  }
  void on_log(const std::string &m) override {
    if (step_ == 0 && m == "SYNC: Begin TPM Test") {
      for (Eg2IpModel *mod : sys_.models()) {
        if ((dev_ = dynamic_cast<Eg2SpiDevice *>(mod))) break;
      }
      if (!dev_) {
        check("spi_device present", false, "no spi_device model");
        return;
      }
      write();
    } else if (step_ == 2 && m == "SYNC: Waiting Read") {
      read();
    }
  }

 private:
  std::string it() const { return "iteration " + std::to_string(i_); }
  // One SPI byte at the agent's SCK (spi_agent_cfg.sv sck_period_ps 50 ns: 20 MHz, 8 bits).
  static constexpr uint64_t kBytePs = 8 * 50000;
  // The header (4 bytes), then a WAIT byte at a time until START (at most 10 ms), then n data
  // bytes; done() at the data's last bit.
  void transaction(uint8_t cmd, unsigned n, std::function<void()> start,
                   std::function<void()> done) {
    sys_.at(sys_.now_ps() + 4 * kBytePs, [this, cmd, n, start, done] {
      dev_->tpm_header(cmd, addr_);
      wait_byte(25000, n, start, done);
    });
  }
  void wait_byte(unsigned left, unsigned n, std::function<void()> start,
                 std::function<void()> done) {
    sys_.at(sys_.now_ps() + kBytePs, [this, left, n, start, done] {
      if (!dev_->tpm_start()) {
        if (left <= 1) {
          check(it() + ": START", false, "WAIT for 10 ms");
          return;
        }
        wait_byte(left - 1, n, start, done);
        return;
      }
      start();
      sys_.at(sys_.now_ps() + n * kBytePs, done);
    });
  }
  void write() {
    step_ = 1;
    unsigned size = 1 + rng_() % 64;
    addr_ = rng_() & 0xffffff;
    data_.resize(size);
    for (auto &b : data_) b = uint8_t(rng_());
    if (!check(it() + ": TPM enabled", dev_->tpm_enabled(), "TPM_CFG.en")) return;
    wait_for(it() + ": START for the write");
    transaction(uint8_t(size - 1), size, [] {}, [this] {
      dev_->tpm_write(data_);
      step_ = 2;
      wait_for(it() + ": \"SYNC: Waiting Read\"");
    });
  }
  void read() {
    step_ = 3;
    wait_for(it() + ": START for the read");
    auto got = std::make_shared<std::vector<uint8_t>>();
    transaction(uint8_t(0x80 | (data_.size() - 1)), unsigned(data_.size()),
                [this, got] { *got = dev_->tpm_read(); },
                [this, got] {
                  char a[16];
                  std::snprintf(a, sizeof a, "0x%06x", addr_);
                  std::vector<uint8_t> want = data_;
                  if (red_) want[0] ^= 1;
                  check(it() + ": read returns the bytes written", *got == want,
                        std::to_string(data_.size()) + " bytes at " + a);
                  if (++i_ == 10) {
                    step_ = 4;
                    wait_for("the PASS");
                    if (pass_held_) {  // the software passed while the last read was clocked
                      finished();
                      sys_.finish(true, "test passed (DV status 0x900d), after the host's last "
                                        "TPM read");
                    }
                  } else {
                    write();
                  }
                });
  }

 public:
  void on_status(uint32_t w) override {
    if (w == kPassed && step_ == 4) finished();
  }
  // The software passes once it has filled the read FIFO for the last read; the host is still
  // clocking that read then, so the run ends when the host has checked it.
  bool takes_pass() override {
    if (step_ != 3) return false;
    pass_held_ = true;
    return true;
  }

 private:
  bool pass_held_ = false;
  std::mt19937 rng_;
  Eg2SpiDevice *dev_ = nullptr;
  unsigned step_ = 0, i_ = 0;
  uint32_t addr_ = 0;
  std::vector<uint8_t> data_;
};

// The dispatcher: --stim host.program=<name> starts a program; the system's events go to it.
class Eg2TestHosts {
 public:
  explicit Eg2TestHosts(Eg2System &sys) : sys_(sys) {
    sys_.stim_handlers["host.program"] = [this](const std::string &v) {
      if (prog_) return false;
      if (v == "example_sival") prog_.reset(new Eg2HostExampleSival(sys_));
      else if (v == "uart_loopback") prog_.reset(new Eg2HostUartLoopback(sys_));
      else if (v == "uart_baud_rate") prog_.reset(new Eg2HostUartBaudRate(sys_));
      else if (v == "example_mem_ujcmd") prog_.reset(mem_ = new Eg2HostMemUjcmd(sys_));
      else if (v.rfind("spi_tpm", 0) == 0)
        prog_.reset(new Eg2HostSpiTpm(
            sys_, v.size() > 8 ? uint32_t(std::stoul(v.substr(8), nullptr, 0)) : 1u));
      else return false;
      std::printf("eg2sim: host program %s\n", v.c_str());
      return true;
    };
    sys_.stim_handlers["host.red"] = [this](const std::string &v) {
      if (!prog_) return false;
      prog_->red_ = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
    sys_.sw_log_watchers.push_back([this](const std::string &m) {
      if (prog_) prog_->on_log(m);
    });
    sys_.sw_status_watchers.push_back([this](uint32_t w) {
      if (!prog_) return;
      prog_->on_status(w);
      if (mem_) mem_->status_after(w);
    });
    auto prev = sys_.on_emit;
    sys_.on_emit = [this, prev](const std::string &inst, const std::string &item) {
      if (prev) prev(inst, item);
      if (!prog_ || inst.rfind("uart", 0) != 0 || item.size() < 2) return;
      uint8_t b = uint8_t(std::strtoul(item.substr(0, 2).c_str(), nullptr, 16));
      prog_->on_uart(inst, b, sys_.now_ps());
    };
    sys_.host_takes_status = [this](uint32_t w) {
      return prog_ && w == 0x900d && prog_->takes_pass();
    };
    sys_.verdict_checks.push_back([this]() -> std::string {
      if (!prog_) return std::string();
      std::string p = prog_->pending();
      if (p.empty()) return std::string();
      return "host " + prog_->name() + ": the run ended while the host waited for " + p;
    });
  }

 private:
  Eg2System &sys_;
  std::unique_ptr<Eg2HostProgram> prog_;
  Eg2HostMemUjcmd *mem_ = nullptr;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_TEST_HOSTS_H_
