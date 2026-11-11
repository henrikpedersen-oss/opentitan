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
  using namespace eg2_model;
  Eg2OtpCtrl otp("otp", block("otp_ctrl.core"), 0);
  Eg2LcCtrl lc("lc", block("lc_ctrl.regs"), 0);
  Bus ob("ob", otp), lb("lb", lc);
  otp.backdoor_lc_state("Rma");
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  auto dai = [&](uint32_t cmd, uint32_t addr, uint32_t data = 0) {
    ob.write("DIRECT_ACCESS_ADDRESS", addr); ob.write("DIRECT_ACCESS_WDATA_0", data);
    ob.write("DIRECT_ACCESS_CMD", cmd); return ob.read("ERR_CODE_11");
  };
  check(dai(2, kEg2OtpDeviceId, 0x12345678) == 0, "adapter routes DAI programming to controller");
  check(otp.device_id()[0] == 0, "programming does not change reset-sampled hardware output");
  check(dai(4, kEg2OtpDeviceId) == 0, "adapter routes hardware digest command");
  otp.on_reset(Eg2ResetCause::kPor);
  check(otp.device_id()[0] == 0x78 && ob.read("ERR_CODE_5") == 0,
        "reset validates independently stored digest and releases buffer");
  otp.otp()[kEg2OtpDeviceId] ^= 1;
  check(otp.device_id()[0] == 0x78, "raw backdoor cannot change a live hardware buffer");
  otp.on_reset(Eg2ResetCause::kPor);
  check(ob.read("ERR_CODE_5") == 6 && otp.device_id()[0] == 0,
        "corruption present at reset is rejected and hardware output clamped");
  otp.otp()[kEg2OtpDeviceId] ^= 1; otp.on_reset(Eg2ResetCause::kPor);
  check(Eg2System::get().stim_handlers.at("otp.buffer_fault")("5:0:1"), "inject buffer fault through full-chip binding");
  ob.write("CHECK_TRIGGER", 1);
  check(ob.read("ERR_CODE_5") == 6 && otp.device_id()[0] == 0 && (ob.read("INTR_STATE") & 2),
        "software integrity trigger detects live buffer fault and raises otp_error");
  otp.on_reset(Eg2ResetCause::kPor);
  auto data = otp.lc_data(); uint16_t words[44];
  for (unsigned i = 0; i < 24; ++i) words[i] = data.count[i];
  for (unsigned i = 0; i < 20; ++i) words[24 + i] = data.state[i];
  words[0] = 1;
  check(otp.lci_program(words) && otp.lc_data().count[0] == 1,
        "runtime lifecycle programming updates raw OTP and its checked buffer");
  ob.write("CHECK_TRIGGER", 2);
  check(ob.read("ERR_CODE_10") == 0, "successful LCI update does not cause a consistency error");
  const auto &secret = kEg2OtpParts[kEg2OtpSecret1];
  check(dai(8, kEg2OtpSramDataKeySeed) == 0 && otp.dword(kEg2OtpSramDataKeySeed) == UINT64_MAX,
        "ZEROIZE is available through the full-chip DAI");
  ob.read("DIRECT_ACCESS_RDATA_0");
  check(Eg2ModelStop::reason().find("undocumented: OTP ZEROIZE RDATA") == 0,
        "ZEROIZE readback stops on conflicting documents");
  Eg2ModelStop::reason().clear();
  check(dai(8, eg2_otp_zer_off(secret)) == 0, "ZEROIZE can program the marker");
  otp.on_reset(Eg2ResetCause::kPor);
  otp.otp()[eg2_otp_digest_off(secret)] ^= 1;
  ob.write("CHECK_TRIGGER", 3);
  check(ob.read("ERR_CODE_8") == 0, "fully zeroized partition skips integrity and consistency checks");
  otp.set_dword(eg2_otp_zer_off(secret), 1); otp.on_reset(Eg2ResetCause::kPor);
  check(Eg2ModelStop::reason().find("undocumented: OTP ZeroizationValidBound") == 0,
        "partial marker cannot use an invented threshold");
  Eg2System::get().close_logs();
  std::printf("eg2_otp_adapter_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
