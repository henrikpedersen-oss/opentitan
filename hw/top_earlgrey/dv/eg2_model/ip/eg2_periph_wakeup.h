// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_IP_EG2_PERIPH_WAKEUP_H_
#define EG2_MODEL_IP_EG2_PERIPH_WAKEUP_H_

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <utility>
#include <vector>
#include "../eg2_ip_model.h"
#include "eg2_aon.h"
#include "eg2_periph_stim.h"

namespace eg2_model {

// The time n cycles of the always-on clock after now (Eg2System's aon clock; a debounce timer
// started before a frequency change keeps the time it was given).
inline uint64_t aon_after(const Eg2System &sys, uint64_t now, uint64_t n) {
  return sys.clock_time_of_cycle(Eg2System::Clk::kAon,
                                 sys.clock_cycles(Eg2System::Clk::kAon, now) + n);
}

inline void eg2_request_wakeup(Eg2System &sys, unsigned src) {
  if (auto *pm = dynamic_cast<Eg2Pwrmgr *>(sys.model("pwrmgr"))) pm->wakeup(src);
}

// The log of these blocks, in the format of the chip bench's system-monitor logs
// (eg2_sysmon_item): "<state name> %08h <fields>" when the block's outputs change, and "< <register>
// %08h" for each status register the core reads that the bench checks (eg2_sysmon_monitors.sv).
class Eg2SysmonLog {
 public:
  struct Read {
    uint16_t reg;
    const char *name;
    uint32_t mask;
  };
  Eg2SysmonLog(Eg2System &sys, std::string inst, std::string state, std::string fields)
      : sys_(sys), inst_(std::move(inst)), state_(std::move(state)), fields_(std::move(fields)) {}
  void reads(std::vector<Read> r) { reads_ = std::move(r); }
  // The block's state now: a line when it differs from the last one.
  void state(uint32_t v) {
    if (have_ && v == last_) return;
    have_ = true;
    last_ = v;
    char b[32];
    std::snprintf(b, sizeof b, "%s %08x", state_.c_str(), v);
    sys_.emit(inst_, fields_.empty() ? std::string(b) : std::string(b) + " " + fields_);
  }
  // A core read of register r, value v.
  void read(uint16_t r, uint32_t v) {
    for (const Read &rd : reads_) {
      if (rd.reg != r) continue;
      char b[64];
      std::snprintf(b, sizeof b, "< %s %08x", rd.name, v & rd.mask);
      sys_.emit_in(inst_, b);
    }
  }

 private:
  Eg2System &sys_;
  std::string inst_, state_, fields_;
  std::vector<Read> reads_;
  bool have_ = false;
  uint32_t last_ = 0;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_WAKEUP_H_
