// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// DPI imports of the alert handler reference model (eg2_alert_dpi.cc, ip/eg2_alert_core.h).
// A read returns an expectation, the bits to compare and, for the escalation counters, a tolerance
// in cycles.
package eg2_alert_cmodel_pkg;
  import "DPI-C" function chandle eg2_alert_create(int unsigned nalerts);
  import "DPI-C" function void eg2_alert_release(chandle h);
  import "DPI-C" function void eg2_alert_reset(chandle h, longint unsigned cyc);
  import "DPI-C" function void eg2_alert_time(chandle h, longint unsigned cyc);
  import "DPI-C" function void eg2_alert_csr_write(chandle h, int unsigned addr, int unsigned data,
                                                   int unsigned be_mask);
  import "DPI-C" function void eg2_alert_csr_read(chandle h, int unsigned addr, int quiet,
                                                  output int unsigned exp,
                                                  output int unsigned mask,
                                                  output int unsigned tol);
  import "DPI-C" function int unsigned eg2_alert_csr_value(chandle h, int unsigned addr);
  import "DPI-C" function void eg2_alert_alert(chandle h, int unsigned id);
  import "DPI-C" function void eg2_alert_local(chandle h, int unsigned idx);
  import "DPI-C" function void eg2_alert_lpg(chandle h, int unsigned g, int low_power);
  import "DPI-C" function void eg2_alert_lpg_map(chandle h, int unsigned alert, int g);
  import "DPI-C" function int eg2_alert_esc(chandle h, int unsigned e);
  import "DPI-C" function int unsigned eg2_alert_intr_state(chandle h);
  import "DPI-C" function int unsigned eg2_alert_intr_enable(chandle h);
  import "DPI-C" function int unsigned eg2_alert_class_state(chandle h, int unsigned k);
  import "DPI-C" function int unsigned eg2_alert_dump_words(chandle h);
  import "DPI-C" function int unsigned eg2_alert_dump_word(chandle h, int unsigned i);
  import "DPI-C" function int eg2_alert_dump_latched(chandle h);
  import "DPI-C" function int eg2_alert_near_transition(chandle h);
  import "DPI-C" function void eg2_alert_set_tol(chandle h, int unsigned t);
  import "DPI-C" function void eg2_alert_verbose(chandle h, int v);
  import "DPI-C" function string eg2_alert_last_note(chandle h);
endpackage
