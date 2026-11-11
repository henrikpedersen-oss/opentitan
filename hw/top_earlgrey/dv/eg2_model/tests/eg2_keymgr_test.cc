// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Regressions for behavior shared by the standalone simulator and the DPI model.
// Transactions go through real TLM sockets, including the register access hooks.
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <tlm_utils/simple_initiator_socket.h>

#include "ip/eg2_crypto_aes.h"
#include "ip/eg2_kmac.h"
#include "ip/eg2_keymgr_dpe.h"
namespace {
unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char *description) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", description);
  }
}

const eg2_regs::Block &block(const char *window) {
  for (const auto &description : eg2_regs::kBlocks) {
    if (std::strcmp(description.window, window) == 0) return description;
  }
  throw std::runtime_error(std::string("missing register block: ") + window);
}

class Bus : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Bus> socket;

  Bus(sc_core::sc_module_name name, Eg2RegBlock &target)
      : sc_core::sc_module(name), socket("socket"), target_(target) {
    socket.bind(target.socket);
  }

  uint32_t read(const char *name) { return access(name, false, 0); }
  void write(const char *name, uint32_t value) { access(name, true, value); }

 private:
  uint32_t access(const char *name, bool write, uint32_t value) {
    const int index = target_.reg_index(name);
    if (index < 0) throw std::runtime_error(std::string("missing register: ") + name);
    tlm::tlm_generic_payload transaction;
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    transaction.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
    transaction.set_address(target_.desc().regs[index].offset);
    transaction.set_data_ptr(reinterpret_cast<unsigned char *>(&value));
    transaction.set_data_length(sizeof value);
    transaction.set_streaming_width(sizeof value);
    socket->b_transport(transaction, delay);
    check(transaction.is_response_ok(), name);
    return value;
  }

  Eg2RegBlock &target_;
};


}  // namespace
int sc_main(int, char **) {
  eg2_model::Eg2OtpCtrl otp("otp", block("otp_ctrl.core"), 0);
  eg2_model::Eg2LcCtrl lc("lc", block("lc_ctrl.regs"), 0);
  eg2_model::Eg2KeymgrDpe keymgr("keymgr", block("keymgr_dpe"), 0);
  Bus ob("ob", otp), lb("lb", lc), kb("kb", keymgr);
  otp.backdoor_lc_state("Rma");
  const auto &part = eg2_model::kEg2OtpParts[eg2_model::kEg2OtpSecret2];
  uint32_t digest = eg2_model::eg2_otp_digest_off(part);
  otp.set_dword(digest, eg2_model::OtpCrypto::digest(otp.otp().data() + part.off, digest - part.off));
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  check(kb.read("CFG_REGWEN") == 1, "lifecycle-enabled keymgr configuration is writable");
  kb.write("MAX_KEY_VER_SHADOWED", 99); kb.write("MAX_KEY_VER_SHADOWED", 99);
  kb.write("MAX_KEY_VER_REGWEN", 0);
  kb.write("CONTROL_SHADOWED", 0); kb.write("CONTROL_SHADOWED", 0);
  kb.write("START", 1);
  check(kb.read("WORKING_STATE") == 1 && kb.read("METADATA_LOW_0") == 0,
        "root loading uses fixed max-key-version zero, independent of configuration");
  check(kb.read("MAX_KEY_VER_REGWEN") == 0, "no undocumented MAX_KEY_VER unlock is invented");
  kb.write("OP_STATUS", 3); kb.write("INTR_STATE", 1);
  kb.write("START", 1);
  check(Eg2ModelStop::reason().find("undocumented: keymgr_dpe advance") == 0,
        "valid advance stops on missing padded-input contract");
  check(kb.read("OP_STATUS") == 0 && kb.read("ERR_CODE") == 0 && kb.read("INTR_STATE") == 0,
        "missing specification is not reported as a completed hardware failure");
  check(kb.read("METADATA_HIGH_0") == 17, "stopped derivation leaves parent slot unchanged");
  Eg2ModelStop::reason().clear();
  keymgr.on_reset(Eg2ResetCause::kPor);
  kb.write("SALT_0", 42);
  otp.backdoor_lc_state("Raw"); otp.on_reset(Eg2ResetCause::kPor); lc.on_reset(Eg2ResetCause::kPor);
  kb.write("SALT_0", 99);  // deliberately no CFG_REGWEN read before the locked write
  check(kb.read("SALT_0") == 42 && kb.read("CFG_REGWEN") == 0,
        "lifecycle lock applies to writes without a preceding status read");
  kb.write("CONTROL_SHADOWED", 16); kb.write("CONTROL_SHADOWED", 16); kb.write("START", 1);
  check(kb.read("CONTROL_SHADOWED") == 16 && kb.read("OP_STATUS") == 0,
        "disabled lifecycle neither stages control nor executes START");
  check(!Eg2System::get().stim_handlers.at("keymgr.lifecycle_gating")("0"),
        "stimulus cannot disable lifecycle gating");
  Eg2System::get().close_logs();
  std::printf("eg2_keymgr_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
