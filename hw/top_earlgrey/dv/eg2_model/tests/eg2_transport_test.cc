// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include "eg2_regblock.h"
#include "eg2_router.h"

namespace {
unsigned checks = 0, failures = 0;
void check(bool ok, const char *what) {
  ++checks;
  if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", what); }
}
class Bus : public sc_core::sc_module {
 public:
  tlm_utils::simple_initiator_socket<Bus> socket;
  explicit Bus(sc_core::sc_module_name name) : sc_module(name), socket("socket") {}
  tlm::tlm_generic_payload transaction;
  uint32_t data = 0;
  void setup(tlm::tlm_command command, uint64_t address, unsigned length = 4) {
    transaction.set_command(command);
    transaction.set_address(address);
    transaction.set_data_ptr(reinterpret_cast<uint8_t *>(&data));
    transaction.set_data_length(length);
    transaction.set_streaming_width(length);
    transaction.set_byte_enable_ptr(nullptr);
    transaction.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
  }
  bool access() {
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    socket->b_transport(transaction, delay);
    return transaction.is_response_ok();
  }
  bool dmi(tlm::tlm_dmi &result) { return socket->get_direct_mem_ptr(transaction, result); }
};
constexpr eg2_regs::Field fields[] = {{0, 32, eg2_regs::kAccRw, 0}};
constexpr eg2_regs::Reg registers[] = {{"VALUE", 0, 42, 0, 1, -1, 0, 0, 0}};
constexpr eg2_regs::Block description = {"test", "test", registers, 1, fields, nullptr, 0};
class Registers : public Eg2RegBlock {
 public:
  explicit Registers(sc_core::sc_module_name name) : Eg2RegBlock(name, description, 0) {}
  unsigned writes = 0, reads = 0;
  bool reject = false;
 protected:
  void before_read(uint16_t) override { if (reject) fail_access(); }
  void after_read(uint16_t) override { ++reads; }
  bool before_write(uint16_t, uint32_t, uint32_t) override {
    if (reject) fail_access();
    return true;
  }
  void on_write(uint16_t, uint32_t, uint32_t, uint32_t) override { ++writes; }
};
}  // namespace

int sc_main(int, char **) {
  Registers regs("regs");
  Eg2Memory memory("memory", 0x100, 64), alias("alias", 0x100, 16);
  Eg2Memory large("large", 0x200, 64), empty("empty", 0x300, 0);
  alias.share_start_of(memory);
  alias.set_word_only();
  Eg2Router router("router", {{"alias", 0x100, 16}, {"backing", 0x100, 64}, {"small", 0x200, 16}});
  router.to(0).bind(alias.socket); router.to(1).bind(memory.socket); router.to(2).bind(large.socket);
  Bus bus("bus"), regbus("regbus"), emptybus("emptybus");
  bus.socket.bind(router.socket); regbus.socket.bind(regs.socket); emptybus.socket.bind(empty.socket);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);

  regbus.setup(tlm::TLM_IGNORE_COMMAND, 0); regbus.data = 7;
  check(!regbus.access() && regs.value(0) == 42 && regs.writes == 0, "IGNORE cannot write a register");
  regbus.setup(tlm::TLM_WRITE_COMMAND, uint64_t(1) << 32);
  check(!regbus.access() && regs.value(0) == 42, "64-bit address cannot alias register zero");
  regbus.setup(tlm::TLM_WRITE_COMMAND, 0);
  uint8_t enables[4] = {0xff, 0, 0, 0};
  regbus.transaction.set_byte_enable_ptr(enables);
  check(!regbus.access() && regs.value(0) == 42, "unsupported byte enables have no side effects");
  regbus.setup(tlm::TLM_WRITE_COMMAND, 0);
  regbus.transaction.set_streaming_width(1);
  check(!regbus.access() && regs.value(0) == 42, "streaming burst cannot become a contiguous write");
  regbus.setup(tlm::TLM_READ_COMMAND, 0, 0);
  check(!regbus.access() && regs.reads == 0, "empty access does not invoke read hook");
  regs.reject = true;
  regbus.setup(tlm::TLM_WRITE_COMMAND, 0);
  check(!regbus.access() && regs.value(0) == 42 && regs.writes == 0, "failed before-write hook prevents storage and after hook");
  regbus.setup(tlm::TLM_READ_COMMAND, 0);
  check(!regbus.access() && regs.reads == 0, "failed before-read hook prevents consuming read");

  bus.setup(tlm::TLM_WRITE_COMMAND, 0x100, 1);
  check(!bus.access(), "bitmap alias rejects partial store");
  bus.setup(tlm::TLM_WRITE_COMMAND, 0x100); bus.data = 0x87654321;
  check(bus.access(), "full-word alias store succeeds");
  check(bus.transaction.get_address() == 0x100, "router restores absolute address");
  check(memory.data()[0] == 0x21, "alias store updates backing memory");
  bus.setup(tlm::TLM_READ_COMMAND, 0x10e);
  check(!bus.access(), "transaction cannot cross first matching window boundary");
  tlm::tlm_dmi dmi;
  bus.setup(tlm::TLM_READ_COMMAND, 0x110);
  check(!bus.dmi(dmi), "backing DMI cannot bypass the restricted alias");
  bus.setup(tlm::TLM_READ_COMMAND, 0x200);
  check(bus.dmi(dmi) && dmi.get_start_address() == 0x200 && dmi.get_end_address() == 0x20f,
        "DMI is clipped to decoded window, not backing allocation");
  emptybus.setup(tlm::TLM_READ_COMMAND, 0);
  check(!emptybus.dmi(dmi), "empty memory cannot grant a wrapping DMI range");
  check(!memory.write(0x101, reinterpret_cast<uint8_t *>(&bus.data), SIZE_MAX),
        "backdoor length cannot wrap around");
  std::printf("eg2_transport_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
