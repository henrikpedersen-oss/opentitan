// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2Bench: the chip bench's steps, done by the model itself (Henrik 2026-11-04: "we dont need a
// chip testbench for anything and the model should be self contained").
//
// A test that needs the bench prints `EG2 BENCH <n> <op> <a> <b>` on UART0 (n decimal, or hex after
// an `x`; a and b hex) and polls kEg2BenchAck until the bench has written {n, result} into it
// (sw/device/tests/eg2_cheriot/eg2_memsys.h, the protocol chip_sw_eg2_rand_vseq gives the chip
// bench). A step the software marks "skipped (no bench)" is not one. Each op is the handler a model
// registered in Eg2System::bench_ops (the model that owns what the op touches: cheriot_mem_sys's
// bitmap port for revbm_*); the handler answers with bench_ack, at once or when the op is done.
// An op no model takes stops the run as a model stop, naming it.
//
// The ops every EG2 test may use, here: por (a power-on reset; kEg2Param_por_done = 1 is written
// during it, so the next boot knows) and ndm_reset (rv_dm's dmcontrol.ndmreset set and cleared over
// the debugger's DMI).
//
// The bench's checks (bench_check) print `EG2CHK ok|FAIL <what> <detail>` and count; one that fails
// fails the run's verdict, whoever ends it (Eg2System::verdict_checks). bench_end ends the run with
// the bench's own verdict (pass if every check passed and there was one), as the chip bench does
// when the core's fetch is off and the software cannot finish. --stim bench.inject_fail=1@0 makes
// the bench invert the check a must-fail image's build broke (the chip run's +eg2_inject_fail=1).

#ifndef EG2_MODEL_EG2_BENCH_H_
#define EG2_MODEL_EG2_BENCH_H_

#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "eg2_system.h"

class Eg2Bench {
 public:
  explicit Eg2Bench(Eg2System &sys) : sys_(sys) {
    sys_.on_console_line = [this](const std::string &w, const std::string &l) {
      if (w == "uart0") line(l);
    };
    sys_.bench_ack = [this](uint32_t n, uint32_t r) { ack(n, r); };
    sys_.bench_check = [this](const std::string &what, bool ok, const std::string &d) {
      return check(what, ok, d);
    };
    sys_.bench_end = [this](const std::string &why) { end(why); };
    sys_.verdict_checks.push_back([this] {
      if (!fails_) return std::string();
      return "bench: " + std::to_string(fails_) + " of " + std::to_string(checks_) +
             " checks failed (first: EG2CHK FAIL " + first_fail_ + ")";
    });
    sys_.stim_handlers["bench.inject_fail"] = [this](const std::string &v) {
      sys_.bench_inject_fail = std::stoul(v, nullptr, 0) != 0;
      return true;
    };
    sys_.bench_ops["por"] = [this](uint32_t n, uint32_t, uint32_t) {
      sys_.note("bench: power-on reset");
      sys_.set_test_param("por_done", 1);
      sys_.apply_stim("rstmgr.por", "1");
      ack(n, 0);
    };
    sys_.bench_ops["ndm_reset"] = [this](uint32_t n, uint32_t, uint32_t) {
      sys_.note("bench: NDM reset");
      sys_.apply_stim("debugger.dmactive", "1");
      sys_.apply_stim("debugger.ndmreset", "1");
      uint64_t aon = sys_.clock_cycles(Eg2System::Clk::kAon, sys_.now_ps());
      sys_.at(sys_.clock_time_of_cycle(Eg2System::Clk::kAon, aon + 10), [this, n] {
        sys_.apply_stim("debugger.ndmreset", "0");
        ack(n, 0);
      });
    };
  }

 private:
  void line(const std::string &l) {
    if (l.rfind("EG2 BENCH ", 0) != 0) return;
    std::istringstream in(l.substr(10));
    std::vector<std::string> tok;
    for (std::string t; in >> t;) tok.push_back(t);
    if (tok.size() < 4 || (tok.size() > 4 && tok[4] == "skipped")) return;
    if (tok[0].back() == ':') return;  // "EG2 BENCH <n>: no answer from the bench"
    uint32_t n, a, b;
    try {
      n = tok[0][0] == 'x' ? uint32_t(std::stoul(tok[0].substr(1), nullptr, 16))
                           : uint32_t(std::stoul(tok[0]));
      a = uint32_t(std::stoul(tok[2], nullptr, 16));
      b = uint32_t(std::stoul(tok[3], nullptr, 16));
    } catch (...) {
      return;  // "EG2 BENCH <n>: no answer from the bench" and other lines
    }
    std::printf("eg2sim: bench step %u: %s 0x%08x 0x%08x\n", n, tok[1].c_str(), a, b);
    auto it = sys_.bench_ops.find(tok[1]);
    if (it == sys_.bench_ops.end()) {
      Eg2ModelStop::request("bench step " + tok[1] + " is not done by any model");
      return;
    }
    it->second(n, a, b);
  }
  void ack(uint32_t n, uint32_t result) {
    uint8_t b[8];
    for (int i = 0; i < 4; ++i) {
      b[i] = uint8_t(n >> (8 * i));
      b[4 + i] = uint8_t(result >> (8 * i));
    }
    if (!sys_.write_test_symbol || !sys_.write_test_symbol("kEg2BenchAck", b, 8)) {
      Eg2ModelStop::request("bench: the test image has no kEg2BenchAck to answer step " +
                            std::to_string(n));
    }
  }
  bool check(const std::string &what, bool ok, const std::string &detail) {
    ++checks_;
    std::printf("eg2sim: EG2CHK %s %s %s\n", ok ? "ok" : "FAIL", what.c_str(), detail.c_str());
    std::fflush(stdout);
    if (!ok && !fails_++) first_fail_ = what + " " + detail;
    return ok;
  }
  void end(const std::string &why) {
    if (sys_.finished()) return;
    std::printf("eg2sim: bench: %u checks, %u failed\n", checks_, fails_);
    sys_.finish(fails_ == 0 && checks_ > 0,
                "the bench's verdict (" + why + "): " + std::to_string(checks_) + " checks, " +
                    std::to_string(fails_) + " failed");
  }

  Eg2System &sys_;
  unsigned checks_ = 0, fails_ = 0;
  std::string first_fail_;
};

#endif  // EG2_MODEL_EG2_BENCH_H_
