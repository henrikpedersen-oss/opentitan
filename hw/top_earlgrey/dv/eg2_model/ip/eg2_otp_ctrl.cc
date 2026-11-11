// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
#include "eg2_otp_ctrl.h"
#include "eg2_lc_ctrl.h"
namespace eg2_model {
bool Eg2OtpCtrl::lc(Eg2OtpLc signal) {
  auto *controller = dynamic_cast<Eg2LcCtrl *>(sys_.model("lc_ctrl.regs"));
  if (!controller) return false;  // no connected enable signal
  switch (signal) {
    case Eg2OtpLc::kCreatorSeedSwRwEn: return controller->creator_seed_sw_rw_en();
    case Eg2OtpLc::kSeedHwRdEn: return controller->seed_hw_rd_en();
    case Eg2OtpLc::kRmaState: return controller->state() == kLcRma;
    case Eg2OtpLc::kCheckBypEn:
      if (controller->state() == kLcPostTransition)
        Eg2ModelStop::request("undocumented: OTP lifecycle consistency-check bypass timing (OTP-1)");
      return false;
  }
  return false;
}
}  // namespace eg2_model
