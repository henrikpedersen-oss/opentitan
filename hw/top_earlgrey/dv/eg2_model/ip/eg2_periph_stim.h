// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// External stimulus for the peripheral models of the EG2 SystemC model: what an agent or the
// board would drive into the chip, given on eg2sim's command line as --stim <key>=<value>.
//
//   gpio.in=<hex>          the value outside drives on the GPIOs whose output is disabled (default
//                          0: no pull-up modelled)
//   uart<N>.rx=<text>      bytes the peer sends to uart<N> (C escapes \n \r \t \\ \xNN), received
//                          as soon as CTRL.RX is set
//   adc_ctrl.chan<N>=<v>   the value channel N (0, 1) samples (default 0)
//   sysrst_ctrl.in=<hex>   the levels of sysrst_ctrl's inputs, in PIN_IN_VALUE's bit order (default
//                          0xcf: buttons released, lid closed, no AC, EC reset and write-protect
//                          not asserted)
//   pinmux.mio=<hex>       the levels of the MIO pads the wake-up detectors watch (default 0)
//   pinmux.dio=<hex>       the levels of the DIO pads the wake-up detectors watch (default 0)
//   usbdev.vbus_us=<us>    when VBUS rises after the start, in microseconds (default 100)
//   i2c<N>.rx=<hex bytes>  bytes the I2C peer answers reads with (default: the bytes it was written,
//                          echoed back)
//   i2c<N>.nack=<addr>     the 7-bit address the I2C peer does not acknowledge
//   spi_host<N>.rx=<hex>   bytes the SPI peer returns on read segments (default 0xff, an idle bus)
//   spi_host<N>.peer=echo  the SPI peer answers with the bytes it was sent, in order
//
// Byte values may take bytes from the test image, as the chip bench's sequences send data they
// wrote into it: {sym:<symbol>:<length>} is the symbol's initial bytes, {rep:<hex byte>:<count>} a
// byte repeated (eg2sim.cc expand_stim; for example uart0.rx={sym:kExpUartRxData:64}{rep:55:65}).

#ifndef EG2_MODEL_IP_EG2_PERIPH_STIM_H_
#define EG2_MODEL_IP_EG2_PERIPH_STIM_H_

#include <cctype>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace eg2_model {

class Eg2PeriphStim {
 public:
  static std::map<std::string, std::string> &all() {
    static std::map<std::string, std::string> m;
    return m;
  }
  // "key=value" from the command line; false if there is no '='.
  static bool add(const std::string &kv) {
    size_t eq = kv.find('=');
    if (eq == std::string::npos) return false;
    all()[kv.substr(0, eq)] = kv.substr(eq + 1);
    return true;
  }
  static bool has(const std::string &key) { return all().count(key) != 0; }
  static uint32_t u32(const std::string &key, uint32_t dflt) {
    auto it = all().find(key);
    return it == all().end() ? dflt : uint32_t(std::stoul(it->second, nullptr, 0));
  }
  // Text with C escapes, as bytes.
  static std::vector<uint8_t> text(const std::string &key) {
    std::vector<uint8_t> out;
    auto it = all().find(key);
    if (it == all().end()) return out;
    const std::string &s = it->second;
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] != '\\' || i + 1 == s.size()) {
        out.push_back(uint8_t(s[i]));
        continue;
      }
      char c = s[++i];
      if (c == 'n') out.push_back('\n');
      else if (c == 'r') out.push_back('\r');
      else if (c == 't') out.push_back('\t');
      else if (c == 'x' && i + 2 < s.size() + 1) {
        out.push_back(uint8_t(std::stoul(s.substr(i + 1, 2), nullptr, 16)));
        i += 2;
      } else out.push_back(uint8_t(c));
    }
    return out;
  }
  // Hex bytes ("01 02 ff" or "0102ff").
  static std::vector<uint8_t> hex_bytes(const std::string &key) {
    std::vector<uint8_t> out;
    auto it = all().find(key);
    if (it == all().end()) return out;
    std::string h;
    for (char c : it->second) {
      if (std::isxdigit(static_cast<unsigned char>(c))) h += c;
    }
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(uint8_t(std::stoul(h.substr(i, 2), nullptr, 16)));
    return out;
  }
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_STIM_H_
