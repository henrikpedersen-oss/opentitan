// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_stimulus.h"

#include <cctype>
#include <cstdio>
#include <map>

#include "eg2_image.h"
#include "ip/eg2_periph_stim.h"

namespace eg2_sim {

// Stimulus values may name bytes the bench takes from the test image, as the chip bench's
// sequences do (they write data into the image with sw_symbol_backdoor_overwrite and then send the
// same bytes): {sym:<name>:<length>} is the symbol's initial bytes, {rep:<hex byte>:<count>} a byte
// repeated. They become C escapes in a UART's text (uart<N>.*) and hex bytes everywhere else
// (ip/eg2_periph_stim.h). False, with a message, if a symbol is missing.
// A stimulus applied while the model runs, as a chip-bench sequence drives it at a point of the
// test: --stim <key>=<value>@<point>[@<point>...][+<delay>], or --stim <key>=<value>@<delay> once,
// <delay> after the start (the header comment gives the points). The points are what the sequence
// waits for (sw_logger_if printed_log, sw_test_status_if, the core's sleep, pwrmgr_low_power_if),
// in order: the stimulus applies when the last one happens, after <delay> (<n>us, <n>ns, or
// <n>cyc: main-clock cycles, the counter that times it against the chip), and the points start
// again. The keys are the models' (Eg2System::stim_handlers), each documented where its model
// registers it.

// The test status words (sw/device/lib/testing/test_framework/status.h), by the name the bench's
// sw_test_status_e gives them without its SwTestStatus prefix.
bool status_word(const std::string &name, uint32_t *w) {
  static const std::map<std::string, uint32_t> kWords = {
      {"InBootRom", 0xb090}, {"InBootRomHalt", 0xb057}, {"InTest", 0x4354},
      {"InWfi", 0x1d1e},     {"Passed", 0x900d},        {"Failed", 0xbaad}};
  auto it = kWords.find(name);
  if (it != kWords.end()) return *w = it->second, true;
  if (name.empty() || !std::isdigit(static_cast<unsigned char>(name[0]))) return false;
  *w = uint32_t(std::stoul(name, nullptr, 0));
  return true;
}

// Parses "<key>=<value>@<when>"; false if the argument has no trigger (a plain --stim).
bool parse_trig_stim(const std::string &arg, TrigStim *t, std::string *err) {
  size_t eq = arg.find('=');
  if (eq == std::string::npos) return false;
  size_t at = arg.find('@', eq);
  if (at == std::string::npos || at + 1 >= arg.size()) return false;
  std::string when = arg.substr(at + 1);
  bool from_start = std::isdigit(static_cast<unsigned char>(when[0]));
  static const char *kPoints[] = {"log:", "status:", "event:", "wfi", "lowpower"};
  bool point = false;
  for (const char *p : kPoints) point |= when.rfind(p, 0) == 0;
  if (!from_start && !point) return false;
  t->key = arg.substr(0, eq);
  t->arg = arg;
  std::string v = arg.substr(eq + 1, at - eq - 1);
  for (size_t s = 0;;) {
    size_t c = v.find(',', s);
    t->values.push_back(v.substr(s, c == std::string::npos ? std::string::npos : c - s));
    if (c == std::string::npos) break;
    s = c + 1;
  }
  // The delay: the last "+<n><unit>" of the last point, or the whole of a start trigger.
  std::string delay;
  if (from_start) {
    delay = when;
  } else {
    size_t plus = when.rfind('+');
    if (plus != std::string::npos && plus + 1 < when.size() &&
        std::isdigit(static_cast<unsigned char>(when[plus + 1]))) {
      delay = when.substr(plus + 1);
      when = when.substr(0, plus);
    }
    for (size_t s = 0; s <= when.size();) {
      size_t a = when.find('@', s);
      std::string p = when.substr(s, a == std::string::npos ? std::string::npos : a - s);
      s = a == std::string::npos ? when.size() + 1 : a + 1;
      // %20 is a space (bench.txt splits its lines at spaces).
      for (size_t q; (q = p.find("%20")) != std::string::npos;) p.replace(q, 3, " ");
      TrigStim::Point pt;
      if (p.rfind("log:", 0) == 0) {
        pt.text = p.substr(4);
        pt.kind = TrigStim::Point::kLog;
        if (!pt.text.empty() && pt.text.back() == '*') {
          pt.kind = TrigStim::Point::kLogPrefix;
          pt.text.pop_back();
        }
        if (pt.text.empty()) return *err = "an empty log message", true;
      } else if (p.rfind("status:", 0) == 0) {
        pt.kind = TrigStim::Point::kStatus;
        pt.text = p.substr(7);
        if (!status_word(pt.text, &pt.status)) return *err = "test status '" + pt.text + "'", true;
      } else if (p.rfind("event:", 0) == 0) {
        pt.kind = TrigStim::Point::kEvent;
        pt.text = p.substr(6);
        if (pt.text.empty()) return *err = "an empty event name", true;
      } else if (p == "wfi" || p == "lowpower") {
        pt.kind = TrigStim::Point::kEvent;
        pt.text = p;
      } else {
        return *err = "point '" + p + "' (log:, status:, event:, wfi or lowpower)", true;
      }
      t->points.push_back(pt);
    }
  }
  if (!delay.empty()) {
    size_t n = 0;
    uint64_t v = std::stoull(delay, &n);
    std::string unit = delay.substr(n);
    if (unit == "us") t->ps = v * 1000000ull;
    else if (unit == "ns") t->ps = v * 1000ull;
    else if (unit == "cyc") t->cycles = v;
    else return *err = "delay unit '" + unit + "' (us, ns or cyc)", true;
  }
  return true;
}

bool expand_stim(const std::string &test) {
  for (auto &kv : eg2_model::Eg2PeriphStim::all()) {
    std::string &v = kv.second;
    std::string out;
    bool text = kv.first.rfind("uart", 0) == 0;
    size_t pos = 0;
    while (pos < v.size()) {
      size_t open = v.find('{', pos), close = v.find('}', open);
      if (open == std::string::npos || close == std::string::npos) break;
      out += v.substr(pos, open - pos);
      std::string tok = v.substr(open + 1, close - open - 1);
      size_t c1 = tok.find(':'), c2 = tok.rfind(':');
      std::vector<uint8_t> bytes;
      if (c1 != std::string::npos && c2 > c1) {
        std::string kind = tok.substr(0, c1), arg = tok.substr(c1 + 1, c2 - c1 - 1);
        size_t n = std::stoul(tok.substr(c2 + 1), nullptr, 0);
        if (kind == "rep") bytes.assign(n, uint8_t(std::stoul(arg, nullptr, 16)));
        if (kind == "sym" && !elf_symbol_bytes(test, arg, n, &bytes)) {
          std::fprintf(stderr, "eg2sim: --stim %s: the test image has no %zu bytes at %s\n",
                       kv.first.c_str(), n, arg.c_str());
          return false;
        }
      }
      char hex[8];
      for (uint8_t b : bytes) {
        std::snprintf(hex, sizeof hex, text ? "\\x%02x" : "%02x", b);
        out += hex;
      }
      pos = close + 1;
    }
    v = out + v.substr(pos);
  }
  return true;
}

}  // namespace eg2_sim
