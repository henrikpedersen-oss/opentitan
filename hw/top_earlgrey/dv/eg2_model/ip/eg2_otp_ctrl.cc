// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_otp_ctrl.h"
#include "eg2_lc_ctrl.h"

namespace eg2_model {

bool Eg2OtpCtrl::creator_seed_writable() {
  auto *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  return !lc || lc->creator_seed_sw_rw_en();
}

bool Eg2OtpCtrl::rma() {
  Eg2LcCtrl *lc = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  return lc && lc->state() == kLcRma;
}

}  // namespace eg2_model
