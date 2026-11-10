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

#include "eg2_ip_model.h"
#include "ip/eg2_core_system.h"
#include "ip/eg2_mem.h"
#include "ip/eg2_periph_gpio.h"

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

// A locked command must not reach either device hook. The hardware-updated
// status register also lets us exercise the unmodelled-register polling limit.
constexpr eg2_regs::Field kFields[] = {
    {0, 1, eg2_regs::kAccRw0c, 0},
    {0, 32, eg2_regs::kAccRw, 0},
    {0, 32, eg2_regs::kAccRo, 1},
};
constexpr eg2_regs::Reg kRegisters[] = {
    {"REGWEN", 0, 1, 0, 1, -1, 0, 0, 0},
    {"COMMAND", 4, 0, 1, 1, 0, 1, 1, 0},
    {"STATUS", 8, 0, 2, 1, -1, 0, 0, 4},
};
constexpr eg2_regs::Block kBlock = {
    "test", "test", kRegisters, 3, kFields, nullptr, 0};

class HookedRegisters : public Eg2RegBlock {
 public:
  explicit HookedRegisters(sc_core::sc_module_name name) : Eg2RegBlock(name, kBlock, 0) {}
  unsigned before_commands = 0;
  unsigned after_commands = 0;

 protected:
  bool before_write(uint16_t index, uint32_t, uint32_t) override {
    if (index == 1) ++before_commands;
    return true;
  }
  void on_write(uint16_t index, uint32_t, uint32_t, uint32_t) override {
    if (index == 1) ++after_commands;
  }
};

void check_register_locks(HookedRegisters &registers, Bus &bus) {
  bus.write("COMMAND", 0x12345678);
  check(registers.before_commands == 1 && registers.after_commands == 1,
        "an unlocked command reaches both hooks");
  bus.write("REGWEN", 0);
  bus.write("COMMAND", 0x87654321);
  check(bus.read("COMMAND") == 0x12345678, "REGWEN protects the stored command");
  check(registers.before_commands == 1 && registers.after_commands == 1,
        "REGWEN also prevents command side effects");
  registers.reset();
  bus.write("COMMAND", 42);
  check(registers.after_commands == 2, "reset unlocks commands again");
}

void check_alert_test(Eg2IpModel &uart, Bus &bus) {
  unsigned alerts = 0;
  Eg2System::get().on_alert = [&alerts](uint16_t) { ++alerts; };
  bus.write("ALERT_TEST", 0x80000001);
  check(alerts == 1, "unlocked ALERT_TEST raises an alert");
  bus.write("ALERT_TEST", 1);
  check(alerts == 2, "the write clearing REGWEN can raise its final alert");
  check(bus.read("ALERT_TEST") == 0, "ALERT_TEST.REGWEN reads locked");
  bus.write("ALERT_TEST", 0x80000001);
  check(alerts == 2, "locked ALERT_TEST neither raises an alert nor unlocks");
  uart.on_reset(Eg2ResetCause::kPor);
  bus.write("ALERT_TEST", 0x80000001);
  check(alerts == 3, "reset re-enables alert testing");
  Eg2System::get().on_alert = {};
}

void check_gpio(Bus &bus) {
  auto &system = Eg2System::get();
  std::vector<std::string> output;
  system.on_emit = [&output](const std::string &instance, const std::string &line) {
    if (instance == "gpio0") output.push_back(line);
  };
  bus.write("DIRECT_OE", 0xffffffff);
  bus.write("DIRECT_OUT", 0xffffffff);
  const size_t before = output.size();
  bus.write("DIRECT_OUT", 0);
  check(output.size() == before + 1, "GPIO logs the transition from all-high to all-low");
  check(!output.empty() && output.back() == "00000000 ffffffff",
        "GPIO emits the new driven value and output enables");

  bus.write("DIRECT_OE", 0);
  bus.write("INTR_CTRL_EN_RISING", 1);
  const auto input = system.inputs.find("gpio.in");
  check(input != system.inputs.end(), "GPIO registers a chip-bench input handler");
  if (input != system.inputs.end()) {
    input->second(1);
    check(bus.read("DATA_IN") == 1, "chip-bench GPIO input reaches DATA_IN immediately");
    check(bus.read("INTR_STATE") & 1, "chip-bench GPIO edges raise configured interrupts");
  }
  system.on_emit = {};
}

void check_plic_claim(eg2_model::Eg2RvPlic &plic, Bus &bus) {
  auto &system = Eg2System::get();
  bus.write("PRIO_1", 2);
  bus.write("PRIO_2", 1);
  bus.write("IE0_0", (1u << 1) | (1u << 2));
  system.set_plic_source(1, true);
  system.set_plic_source(1, false);
  system.set_plic_source(2, true);
  check(bus.read("CC0") == 1, "PLIC retains a pending request after its source falls");
  plic.load_event(plic.desc().regs[plic.reg_index("CC0")].offset, 2);
  check(bus.read("IP_0") & (1u << 2),
        "an observed RTL claim cannot consume another model request");
  check(bus.read("CC0") == 2, "the model independently claims its next pending request");
  system.set_plic_source(2, false);
  bus.write("CC0", 1);
  bus.write("CC0", 2);
  bus.write("THRESHOLD0", 2);
  system.set_plic_source(1, true);
  system.set_plic_source(1, false);
  check(!system.meip, "the threshold suppresses external interrupt notification");
  check(bus.read("CC0") == 1, "software can claim a pending interrupt below the threshold");
  check(!(bus.read("IP_0") & (1u << 1)), "a polled claim consumes the pending request");
}

void check_tbre_error_race(eg2_model::Eg2CheriotMemSys &tbre, Bus &bus) {
  auto &system = Eg2System::get();
  std::vector<uint8_t> memory(256, 0);
  std::vector<uint8_t> meta(8, 0);
  eg2_model::Eg2TbreLayout layout;
  layout.heap_base = 0x10000000;
  layout.revbm_bytes = 4;
  layout.meta = &meta;
  layout.regions = {{layout.heap_base, layout.heap_base + 256, 4, &memory}};
  layout.cheriot = [] { return true; };
  tbre.configure(layout);

  bus.write("TBRE_BASE_ADDR", layout.heap_base + 7);
  check(bus.read("TBRE_BASE_ADDR") == layout.heap_base, "TBRE base reads back aligned to eight bytes");
  bus.write("TBRE_NUM_CAPS", 32);
  system.bench_ops.at("tbre_fault_arm")(0, 0, layout.heap_base);
  bus.write("TBRE_START", 1);
  tbre.on_time(Eg2System::time_of_cycle(32, system.clock_hz(layout.clk)));
  check(bus.read("TBRE_STATUS") & (1u << 9), "a failed TBRE read sets sweep_err");
  bus.write("TBRE_STATUS", 1u << 9);
  check(!(bus.read("TBRE_STATUS") & (1u << 9)), "software can clear sweep_err during the sweep");
  tbre.on_time(Eg2System::time_of_cycle(64, system.clock_hz(layout.clk)));
  check(Eg2ModelStop::reason().find("undocumented: TBRE_STATUS.sweep_err") == 0,
        "an unspecified completion value stops instead of selecting a passing result");
  check(bus.read("TBRE_EPOCH") == 0, "a faulty sweep does not advance the epoch");
  system.unschedule(&tbre);
}
}  // namespace

int sc_main(int, char **) {
  HookedRegisters registers("registers");
  Eg2IpModel uart("uart", block("uart0"), 0);
  eg2_model::Eg2Gpio gpio("gpio", block("gpio0"), 0);
  eg2_model::Eg2RvPlic plic("plic", block("rv_plic"), 0);
  eg2_model::Eg2CheriotMemSys tbre("tbre", block("cheriot_mem_sys.regs"), 0);
  Bus register_bus("register_bus", registers);
  Bus uart_bus("uart_bus", uart);
  Bus gpio_bus("gpio_bus", gpio);
  Bus plic_bus("plic_bus", plic);
  Bus tbre_bus("tbre_bus", tbre);
  sc_core::sc_start(sc_core::SC_ZERO_TIME);

  check_register_locks(registers, register_bus);
  check_alert_test(uart, uart_bus);
  check_gpio(gpio_bus);
  check_plic_claim(plic, plic_bus);
  check_tbre_error_race(tbre, tbre_bus);
  Eg2System::get().close_logs();
  std::printf("eg2_model_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
