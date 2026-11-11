// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Exercise the public C API without an RTL simulator or prebuilt firmware.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "dpi/eg2_sysmodel.h"
#include "rom_fixture.h"
#include "eg2_model_regs.h"
#include "hw/top_earlgrey/sw/autogen/top_earlgrey.h"

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

struct Read {
  uint32_t data = 0;
  int error = 0;
  int predicted = 0;
};

Read read(void *model, uint32_t address) {
  Read result;
  result.predicted = eg2_sysmodel_read(model, address, 4, &result.data, &result.error);
  return result;
}
}  // namespace

int main() {
  // A valid ELF32 header with no load segments. Tests supply every known byte
  // explicitly, so they cannot accidentally depend on an image's contents.
  std::array<unsigned char, 52> elf{};
  std::memcpy(elf.data(), "\177ELF", 4);
  elf[4] = 1;  // ELFCLASS32
  elf[5] = 1;  // ELFDATA2LSB
  elf[6] = 1;  // EV_CURRENT
  elf[16] = 2;  // ET_EXEC
  elf[18] = 243;  // EM_RISCV
  elf[20] = 1;
  elf[40] = elf.size();
  char path[] = "/tmp/eg2-sysmodel-test-XXXXXX";
  const int file = mkstemp(path);
  if (file < 0) return 1;
  const auto written = write(file, elf.data(), elf.size());
  close(file);
  if (written != static_cast<ssize_t>(elf.size())) {
    unlink(path);
    return 1;
  }
  if (!write_rom_fixture(path)) return 1;
  void *model = eg2_sysmodel_create(path, "", "", ".");
  unlink(path);
  unlink((std::string(path) + ".romcheck").c_str());
  if (!model) {
    std::fprintf(stderr, "create: %s\n", eg2_sysmodel_create_error());
    return 1;
  }
  eg2_sysmodel_reset(model, 0);

  const uint32_t ram = TOP_EARLGREY_SRAM_CTRL_MAIN_RAM_BASE_ADDR;
  check(!read(model, ram).predicted, "unwritten RAM is not a prediction");
  eg2_sysmodel_load_event(model, ram, 4, 0xdeadbeef);
  check(!read(model, ram).predicted, "a DUT observation does not make RAM known");
  check(read(model, ram).data != 0xdeadbeef, "DUT load data is not copied into RAM");
  check(eg2_sysmodel_write(model, ram, 4, 0x12345678) == 0, "an independent store succeeds");
  Read result = read(model, ram);
  check(result.predicted && !result.error && result.data == 0x12345678,
        "an independent store establishes a checkable RAM value");
  eg2_sysmodel_load_event(model, ram, 4, 0xdeadbeef);
  check(read(model, ram).data == 0x12345678, "DUT observations cannot overwrite known RAM");

  // CHERIoT startup closes CTRL_REGWEN. Later INIT commands must not destroy
  // the live SRAM contents even though the bus write itself succeeds.
  const uint32_t main_registers = TOP_EARLGREY_SRAM_CTRL_MAIN_REGS_BASE_ADDR;
  const uint32_t main_lock = main_registers +
      eg2_regs::sram_ctrl_main_regs::kRegs[eg2_regs::sram_ctrl_main_regs::kCtrlRegwen].offset;
  const uint32_t main_control = main_registers +
      eg2_regs::sram_ctrl_main_regs::kRegs[eg2_regs::sram_ctrl_main_regs::kCtrl].offset;
  check(eg2_sysmodel_write(model, main_lock, 4, 0) == 0, "SRAM control lock write succeeds");
  check(eg2_sysmodel_write(model, main_control, 4, 2) == 0,
        "a locked SRAM command is ignored without a bus error");
  check(read(model, ram).data == 0x12345678, "locked SRAM INIT preserves live memory");

  check(eg2_sysmodel_input(model, "gpio0.in", 0x12) == 0, "GPIO0 accepts external inputs");
  check(eg2_sysmodel_input(model, "gpio1.in", 0x34) == 0, "GPIO1 accepts external inputs");
  const uint32_t input0 = TOP_EARLGREY_GPIO0_BASE_ADDR +
      eg2_regs::gpio0::kRegs[eg2_regs::gpio0::kDataIn].offset;
  const uint32_t input1 = TOP_EARLGREY_GPIO1_BASE_ADDR +
      eg2_regs::gpio1::kRegs[eg2_regs::gpio1::kDataIn].offset;
  check(read(model, input0).data == 0x12, "GPIO0 reads its own external input");
  check(read(model, input1).data == 0x34, "GPIO1 reads its own external input");
  check(eg2_sysmodel_input(model, "gpio.in", 0x56) == 0, "legacy GPIO input remains available");
  check(read(model, input0).data == 0x56 && read(model, input1).data == 0x34,
        "legacy GPIO input affects only the first bank");

  const uint32_t bitmap = TOP_EARLGREY_CHERIOT_MEM_SYS_REVBM_BASE_ADDR;
  check(eg2_sysmodel_write(model, bitmap, 4, 0xffffffff) == 0, "bitmap word write succeeds");
  check(read(model, bitmap).data == 0xffffffff, "bitmap holds the written word");
  const uint32_t meta_control = TOP_EARLGREY_SRAM_CTRL_META_REGS_BASE_ADDR +
      eg2_regs::sram_ctrl_meta_regs::kRegs[eg2_regs::sram_ctrl_meta_regs::kCtrl].offset;
  check(eg2_sysmodel_write(model, meta_control, 4, 2) == 0, "meta SRAM INIT succeeds");
  check(read(model, bitmap).data == 0, "meta SRAM initialization clears its bitmap alias");
  eg2_sysmodel_forget(model, bitmap, 4);
  check(!read(model, bitmap).predicted, "forget invalidates knowledge of the bitmap bytes");

  eg2_sysmodel_release(model);
  std::printf("eg2_sysmodel_test: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
