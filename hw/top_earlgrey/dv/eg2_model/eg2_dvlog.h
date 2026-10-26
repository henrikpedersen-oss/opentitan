// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// Eg2DvLog: the software's DV log, decoded, as the chip bench's sw_logger does.
//
// In sim_dv builds LOG_* writes to the DV window's log word (device_sim_dv.c,
// device_log_bypass_uart_address: DV_SIM_WINDOW + 4) a pointer to the line's log_fields_t,
// then its nargs argument words (log.c, base_log_internal_dv). The log_fields_t and the strings it
// points to are in the image's .logs.fields section, which is not loaded: info_sections.ld places
// it at address 0, its first word is the image's _dv_log_offset, and a pointer into it is
// _dv_log_offset plus the offset in the section (_dv_log_offset is 0x0 for the ROMs, 0x10000 for
// tests: test_rom.ld, rom.ld, ottf_common.ld). A %s argument points into the loaded image.
//
// Each image's sections are kept (add_image); words arrive through feed(); each complete line is
// printed on stdout ("sw: ...") and written to <log-dir>/sw.model.log.

#ifndef EG2_MODEL_EG2_DVLOG_H_
#define EG2_MODEL_EG2_DVLOG_H_

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

class Eg2DvLog {
 public:
  // Keep an ELF image's .logs.fields and its loaded sections; false if it cannot be read.
  bool add_image(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> e((std::istreambuf_iterator<char>(f)), {});
    if (e.size() < 52 || std::memcmp(e.data(), "\x7f" "ELF", 4) != 0) return false;
    uint32_t shoff = u32(e, 32);
    uint16_t shentsize = u16(e, 46), shnum = u16(e, 48), shstrndx = u16(e, 50);
    if (shstrndx >= shnum) return false;
    uint32_t stroff = u32(e, shoff + shstrndx * shentsize + 16);
    Image img;
    for (uint16_t i = 0; i < shnum; ++i) {
      uint32_t sh = shoff + i * shentsize;
      if (sh + 40 > e.size()) break;
      uint32_t name = u32(e, sh), type = u32(e, sh + 4), flags = u32(e, sh + 8);
      uint32_t addr = u32(e, sh + 12), off = u32(e, sh + 16), size = u32(e, sh + 20);
      if (off + uint64_t(size) > e.size() || type == 8 /*NOBITS*/) continue;
      std::string n = cstr(e, stroff + name);
      std::vector<uint8_t> bytes(e.begin() + off, e.begin() + off + size);
      if (n == ".logs.fields" && size >= 4) {
        img.log_base = u32(bytes, 0);
        img.logs = bytes;
      } else if ((flags & 2) /*ALLOC*/ && size) {
        img.secs.push_back({addr, bytes});
      }
    }
    images_.push_back(img);
    return true;
  }

  std::string log_path;  // sw.model.log; empty: stdout only

  void feed(uint32_t word) {
    if (!pending_) {
      header_ = word;
      args_.clear();
      uint8_t rec[20];
      if (!read(word, rec, sizeof rec)) {
        line("?? log entry 0x" + hex(word));
        return;
      }
      std::memcpy(&sev_, rec, 4);
      std::memcpy(&file_, rec + 4, 4);
      std::memcpy(&lineno_, rec + 8, 4);
      std::memcpy(&nargs_, rec + 12, 4);
      std::memcpy(&fmt_, rec + 16, 4);
      if (nargs_ > 32) nargs_ = 0;
      pending_ = true;
    } else {
      args_.push_back(word);
    }
    if (pending_ && args_.size() == nargs_) {
      pending_ = false;
      std::string f = str(file_);
      size_t sl = f.rfind('/');
      if (sl != std::string::npos) f = f.substr(sl + 1);
      const char sev = sev_ < 4 ? "IWEF"[sev_] : '?';
      line(std::string(1, sev) + " " + f + ":" + std::to_string(lineno_) + "] " + format());
    }
  }

  void close() {
    if (out_) std::fclose(out_);
    out_ = nullptr;
  }

 private:
  struct Image {
    uint32_t log_base = 0;
    std::vector<uint8_t> logs;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> secs;
  };

  static uint32_t u32(const std::vector<uint8_t> &b, size_t o) {
    uint32_t v = 0;
    if (o + 4 <= b.size()) std::memcpy(&v, &b[o], 4);
    return v;
  }
  static uint16_t u16(const std::vector<uint8_t> &b, size_t o) {
    uint16_t v = 0;
    if (o + 2 <= b.size()) std::memcpy(&v, &b[o], 2);
    return v;
  }
  static std::string cstr(const std::vector<uint8_t> &b, size_t o) {
    std::string s;
    while (o < b.size() && b[o]) s += char(b[o++]);
    return s;
  }
  static std::string hex(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%x", v);
    return b;
  }

  // n bytes at a, from the .logs.fields of the image whose range holds a (the larger base wins:
  // the ROM's range starts at 0), else from a loaded section.
  bool read(uint32_t a, uint8_t *dst, size_t n) const {
    const Image *best = nullptr;
    for (const Image &img : images_) {
      if (a >= img.log_base && a - img.log_base + n <= img.logs.size() &&
          (!best || img.log_base > best->log_base)) {
        best = &img;
      }
    }
    if (best) {
      std::memcpy(dst, &best->logs[a - best->log_base], n);
      return true;
    }
    for (const Image &img : images_) {
      for (auto &s : img.secs) {
        if (a >= s.first && a - s.first + n <= s.second.size()) {
          std::memcpy(dst, &s.second[a - s.first], n);
          return true;
        }
      }
    }
    return false;
  }
  std::string str(uint32_t a) const {
    std::string s;
    uint8_t c;
    while (s.size() < 512 && read(a + uint32_t(s.size()), &c, 1) && c) s += char(c);
    return s;
  }

  // The format string with the arguments: %d %u %x %X %c %s %p %b and OpenTitan's %! (status_t),
  // with flags and widths; anything else shows the argument in hex.
  std::string format() const {
    std::string f = str(fmt_), out;
    size_t a = 0;
    for (size_t i = 0; i < f.size(); ++i) {
      if (f[i] != '%') {
        out += f[i];
        continue;
      }
      if (i + 1 < f.size() && f[i + 1] == '%') {
        out += '%';
        ++i;
        continue;
      }
      std::string spec = "%";
      size_t j = i + 1;
      while (j < f.size() && std::strchr("-+ #0123456789!", f[j])) spec += f[j++];
      if (j >= f.size()) break;
      char conv = f[j];
      i = j;
      uint32_t v = a < args_.size() ? args_[a++] : 0;
      bool bang = spec.find('!') != std::string::npos;
      std::string flags;
      for (char c : spec) {
        if (c != '!') flags += c;
      }
      char b[64];
      if (bang) {
        std::snprintf(b, sizeof b, "status 0x%x", v);
      } else if (conv == 'd' || conv == 'i') {
        std::snprintf(b, sizeof b, (flags + "d").c_str(), int32_t(v));
      } else if (conv == 'u') {
        std::snprintf(b, sizeof b, (flags + "u").c_str(), v);
      } else if (conv == 'x' || conv == 'X') {
        std::snprintf(b, sizeof b, (flags + conv).c_str(), v);
      } else if (conv == 'c') {
        std::snprintf(b, sizeof b, "%c", char(v));
      } else if (conv == 's') {
        out += str(v);
        continue;
      } else if (conv == 'b') {
        std::string s;
        for (int k = 31; k >= 0; --k) {
          if (!s.empty() || ((v >> k) & 1) || k == 0) s += ((v >> k) & 1) ? '1' : '0';
        }
        out += s;
        continue;
      } else {
        std::snprintf(b, sizeof b, "0x%x", v);
      }
      out += b;
    }
    return out;
  }

  void line(const std::string &s) {
    std::printf("sw: %s\n", s.c_str());
    if (!out_ && !log_path.empty()) out_ = std::fopen(log_path.c_str(), "w");
    if (out_) std::fprintf(out_, "%s\n", s.c_str());
  }

  std::vector<Image> images_;
  bool pending_ = false;
  uint32_t header_ = 0, sev_ = 0, file_ = 0, lineno_ = 0, nargs_ = 0, fmt_ = 0;
  std::vector<uint32_t> args_;
  FILE *out_ = nullptr;
};

#endif  // EG2_MODEL_EG2_DVLOG_H_
