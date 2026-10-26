// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The registry of IP models: which model each register window of the EG2 SystemC model gets.
// A window takes the first entry whose key is its window name (for example "rv_core_ibex.cfg") or,
// failing that, its IP type (for example "uart", which every UART instance gets). A window with no
// entry is a generic Eg2IpModel: its registers, interrupts and alerts, no behaviour.
//
// To add an IP model (Phase B): write the class under ip/ (derive from Eg2IpModel, see
// eg2_ip_model.h), add it to its group's EG2_MODELS_<GROUP> list (ip/eg2_mem.h, ip/eg2_periph.h,
// ip/eg2_crypto.h).

#include <cstring>

#include "eg2_ip_model.h"
#include "ip/eg2_aon.h"
#include "ip/eg2_clkmgr.h"
#include "ip/eg2_core_system.h"
#include "ip/eg2_crypto.h"
#include "ip/eg2_mem.h"
#include "ip/eg2_periph.h"

namespace {

using Factory = Eg2IpModel *(*)(const char *inst, const eg2_regs::Block &desc, uint32_t base);

template <typename T>
Eg2IpModel *make(const char *inst, const eg2_regs::Block &desc, uint32_t base) {
  return new T(inst, desc, base);
}

struct Entry {
  const char *key;  // window name or IP type
  Factory make;
};

const Entry kModels[] = {
    // Core system (M3).
    {"rv_plic", make<eg2_model::Eg2RvPlic>},
    {"alert_handler", make<eg2_model::Eg2AlertHandler>},
    {"rv_core_ibex.cfg", make<eg2_model::Eg2RvCoreIbexCfg>},
    {"rv_timer", make<eg2_model::Eg2RvTimer>},
    // Always-on (M3).
    {"rstmgr", make<eg2_model::Eg2Rstmgr>},
    {"pwrmgr", make<eg2_model::Eg2Pwrmgr>},
    {"aon_timer", make<eg2_model::Eg2AonTimer>},
    {"sensor_ctrl", make<eg2_model::Eg2SensorCtrl>},
    {"clkmgr", make<eg2_model::Eg2Clkmgr>},
    // Phase B: each group lists its models in its own header.
    EG2_MODELS_MEM
    EG2_MODELS_PERIPH
    EG2_MODELS_CRYPTO
};

}  // namespace

Eg2IpModel *make_ip_model(const char *inst, const eg2_regs::Block &desc, uint32_t base) {
  for (const Entry &e : kModels) {
    if (std::strcmp(e.key, desc.window) == 0) return e.make(inst, desc, base);
  }
  for (const Entry &e : kModels) {
    if (std::strcmp(e.key, desc.ip) == 0) return e.make(inst, desc, base);
  }
  return new Eg2IpModel(inst, desc, base);
}
