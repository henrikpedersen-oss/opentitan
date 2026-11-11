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


// Isolate the shadow protocol from EDN startup. The real idle-gating hook still
// runs; this test controls its input explicitly instead of synthesizing entropy.
class Aes : public eg2_model::Eg2Aes {
 public:
  Aes() : Eg2Aes("aes", block("aes"), 0) {}
  bool available = true;
  bool idle() const override { return available; }
};
uint32_t stored(Eg2RegBlock &ip, const char *name) { return ip.value(ip.reg_index(name)); }
void pair(Bus &bus, const char *name, uint32_t value) {
  bus.write(name, value); bus.write(name, value);
}
void protocol(Eg2IpModel &ip, Bus &bus, const char *name, uint32_t first, uint32_t next) {
  uint32_t old = stored(ip, name);
  bus.write(name, first);
  check(stored(ip, name) == old, "first write leaves committed storage unchanged");
  bus.write(name, first);
  check(bus.read(name) == first, "second equal write commits");
  bus.write(name, next); bus.read(name); bus.write(name, next);
  check(stored(ip, name) == first, "read cancels the pending first write");
  bus.write(name, next);
  check(stored(ip, name) == next, "new matching pair after read commits");
  bus.write(name, first); ip.on_reset(Eg2ResetCause::kPor);
  old = stored(ip, name); bus.write(name, first);
  check(stored(ip, name) == old, "reset cancels pending write");
  bus.write(name, first);
  check(stored(ip, name) == first, "new matching pair after reset commits");
}
}  // namespace
int sc_main(int, char **) {
  Aes aes;
  eg2_model::Eg2Kmac kmac("kmac", block("kmac"), 0);
  eg2_model::Eg2KeymgrDpe keymgr("keymgr_dpe", block("keymgr_dpe"), 0);
  Bus ab("ab", aes), kb("kb", kmac), db("db", keymgr);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);
  protocol(aes, ab, "CTRL_AUX_SHADOWED", 2, 3);
  protocol(kmac, kb, "ENTROPY_REFRESH_THRESHOLD_SHADOWED", 7, 9);
  protocol(keymgr, db, "MAX_KEY_VER_SHADOWED", 7, 9);
  protocol(keymgr, db, "RESEED_INTERVAL_SHADOWED", 7, 9);
  protocol(keymgr, db, "KDF_ENGINE_SHADOWED", 6, 9);
  unsigned alerts = 0;
  Eg2System::get().on_alert = [&](uint16_t) { ++alerts; };
  ab.write("CTRL_AUX_SHADOWED", 0); ab.write("CTRL_AUX_SHADOWED", 1);
  check(stored(aes, "CTRL_AUX_SHADOWED") == 0,
        "matching AES field commits while mismatched field retains its previous value");
  check((ab.read("STATUS") & 32) && alerts == 1, "AES mismatch reports recoverable alert and status");
  uint32_t ctrl = stored(aes, "CTRL_SHADOWED");
  pair(ab, "CTRL_SHADOWED", ctrl);
  check(!(ab.read("STATUS") & 32), "successful AES CTRL rewrite clears update error");
  aes.available = false; ab.write("CTRL_SHADOWED", ctrl ^ (1u << 15)); aes.available = true;
  ab.write("CTRL_SHADOWED", ctrl ^ (1u << 15));
  check(stored(aes, "CTRL_SHADOWED") == ctrl, "busy AES write does not stage a value");
  ab.write("CTRL_SHADOWED", ctrl ^ (1u << 15));
  check(stored(aes, "CTRL_SHADOWED") == (ctrl ^ (1u << 15)), "pair after busy write commits");
  kb.write("CFG_SHADOWED", 0); kb.write("CFG_SHADOWED", 1);
  check((kb.read("STATUS") & (1u << 29)) && alerts == 2, "KMAC mismatch reports alert and status");
  check(kb.read("ERR_CODE") == 0 && kb.read("INTR_STATE") == 0,
        "KMAC shadow error is separate from command errors and interrupts");
  pair(kb, "CFG_SHADOWED", 0);
  check(!(kb.read("STATUS") & (1u << 29)), "successful KMAC configuration clears update error");
  db.write("MAX_KEY_VER_SHADOWED", 10); db.write("MAX_KEY_VER_SHADOWED", 11);
  check(stored(keymgr, "MAX_KEY_VER_SHADOWED") == 0 && alerts == 3,
        "keymgr mismatch preserves committed data and raises alert");
  check(db.read("ERR_CODE") == 4 && db.read("OP_STATUS") == 0 && db.read("INTR_STATE") == 0,
        "keymgr asynchronous shadow error does not complete a command");
  db.write("ERR_CODE", 4); check(db.read("ERR_CODE") == 0, "software explicitly clears keymgr error");
  db.write("MAX_KEY_VER_SHADOWED", 20); db.write("MAX_KEY_VER_REGWEN", 0);
  db.write("MAX_KEY_VER_SHADOWED", 20);
  check(stored(keymgr, "MAX_KEY_VER_SHADOWED") == 0, "REGWEN blocks second write and commit");
  keymgr.on_reset(Eg2ResetCause::kPor);
  db.write("MAX_KEY_VER_SHADOWED", 20);
  check(stored(keymgr, "MAX_KEY_VER_SHADOWED") == 0, "reset also discards a locked pending update");
  Eg2System::get().on_alert = {};
  Eg2System::get().close_logs();
  std::printf("eg2_shadow_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
