// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2BenchSeq: what the chip bench's sequences of OpenTitan's chip tests do to the chip (fault
// injection into a countermeasure, a broken memory word), as stimulus keys. The suite's bench.txt
// gives them per test, timed on the software's log messages (--stim <key>=<value>@log:<message>),
// together with the symbols the sequence writes (--sym). The model then does what the IPs'
// documentation says follows; the test's own checks decide. (Henrik 2026-11-04: the model is
// self-contained, no test needs the chip bench.)
//
//   alert.fatal_fault=<window>.<alert>   a fault in a countermeasure of the IP at <window>: it
//                                        raises its fatal <alert> and, where the IP's
//                                        documentation gives the fault a status field, sets it
//                                        (Eg2IpModel::fatal_fault). chip_sw_all_escalation_resets_vseq.

#ifndef EG2_MODEL_EG2_BENCH_SEQ_H_
#define EG2_MODEL_EG2_BENCH_SEQ_H_

#include <cstdio>
#include <string>

#include "eg2_ip_model.h"
#include "eg2_system.h"

class Eg2BenchSeq {
 public:
  explicit Eg2BenchSeq(Eg2System &sys) : sys_(sys) {
    sys_.stim_handlers["alert.fatal_fault"] = [this](const std::string &v) {
      size_t dot = v.find('.');
      if (dot == std::string::npos) return false;
      std::string window = v.substr(0, dot), alert = v.substr(dot + 1);
      Eg2IpModel *m = sys_.model(window);
      if (!m || !m->fatal_fault(alert)) {
        std::printf("eg2sim: alert.fatal_fault: no fatal alert %s at %s\n", alert.c_str(),
                    window.c_str());
        return false;
      }
      sys_.note("bench: countermeasure fault in " + window + ", fatal alert " + alert);
      return true;
    };
  }

 private:
  Eg2System &sys_;
};

#endif  // EG2_MODEL_EG2_BENCH_SEQ_H_
