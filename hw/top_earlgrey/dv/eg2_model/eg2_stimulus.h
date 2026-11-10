// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Triggered stimulus description and parsing for the standalone simulator.
#ifndef EG2_MODEL_EG2_STIMULUS_H_
#define EG2_MODEL_EG2_STIMULUS_H_

#include <cstdint>
#include <string>
#include <vector>

namespace eg2_sim {

struct TrigStim {
  struct Point {
    enum Kind { kLog, kLogPrefix, kStatus, kEvent } kind;
    std::string text;  // the message, its prefix, or the event's name
    uint32_t status = 0;
  };
  std::string key, arg;
  std::vector<std::string> values;  // one: every time; more: the n-th time the n-th
  std::vector<Point> points;        // none: once, from the start
  uint64_t ps = 0, cycles = 0;
  size_t next = 0;   // the point waited for
  size_t times = 0;  // how often the points have all happened
  std::vector<std::string> nums;  // the numbers of the last log message a point matched
};


bool parse_trig_stim(const std::string &arg, TrigStim *t, std::string *err);
bool expand_stim(const std::string &test);

}  // namespace eg2_sim

#endif  // EG2_MODEL_EG2_STIMULUS_H_
