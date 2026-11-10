// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

// Standalone image loading, OTP persistence and ELF symbol access.
#ifndef EG2_MODEL_EG2_IMAGE_H_
#define EG2_MODEL_EG2_IMAGE_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Eg2Memory;
class Eg2RegBlock;

namespace eg2_sim {

bool load_elf(const std::string &path, std::vector<Eg2Memory *> &mems,
              std::map<std::string, uint32_t> *symbols = nullptr, bool segments = true);
bool load_rram_image(const std::string &path, Eg2Memory *rram);
bool load_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks);
bool save_otp(const std::string &path, std::vector<Eg2RegBlock *> &blocks);
uint32_t elf_symbol_size(const std::string &path, const std::string &symbol);
bool elf_symbol_bytes(const std::string &path, const std::string &symbol, size_t len,
                      std::vector<uint8_t> *out);
uint32_t elf_load_address(const std::string &path, uint32_t vaddr);
bool write_symbol(const std::map<std::string, uint32_t> &syms, std::vector<Eg2Memory *> &mems,
                  const std::string &name, uint32_t value);

}  // namespace eg2_sim

#endif  // EG2_MODEL_EG2_IMAGE_H_
