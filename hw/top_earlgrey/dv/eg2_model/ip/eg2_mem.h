// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Compatibility entry point. Device implementations live in the headers below.

#ifndef EG2_MODEL_IP_EG2_MEM_H_
#define EG2_MODEL_IP_EG2_MEM_H_

#include "eg2_rram_ctrl.h"
#include "eg2_sram_ctrl.h"
#include "eg2_rom_ctrl.h"
#include "eg2_cheriot_mem_sys.h"

// This group's entries of kModels (eg2_ip_models.cc): {window name or IP type, factory}.
#define EG2_MODELS_MEM \
  {"otp_ctrl", make<eg2_model::Eg2OtpCtrl>}, \
  {"lc_ctrl", make<eg2_model::Eg2LcCtrl>}, \
  {"rram_ctrl.core", make<eg2_model::Eg2RramCtrl>}, \
  {"sram_ctrl", make<eg2_model::Eg2SramCtrl>}, \
  {"rom_ctrl", make<eg2_model::Eg2RomCtrl>}, \
  {"cheriot_mem_sys", make<eg2_model::Eg2CheriotMemSys>},


#endif  // EG2_MODEL_IP_EG2_MEM_H_
