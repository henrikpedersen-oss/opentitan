// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// spi_device of the EG2 SystemC model (hw/ip/spi_device: data/spi_device.hjson, README.md,
// doc/theory_of_operation.md, doc/programmers_guide.md).
//
// spi_device is a SPI target: everything it does starts with a SPI host on its pins, and eg2sim
// has none. So the model gives the reply software sees with the bus idle, and does not invent a
// host's commands:
//   STATUS       csb and tpm_csb read 1 (both chip selects de-asserted).
//   Upload       the command and address FIFOs and the payload buffer stay empty: UPLOAD_STATUS and
//                UPLOAD_STATUS2 read 0, a read of UPLOAD_CMDFIFO or UPLOAD_ADDRFIFO reads 0 (an
//                empty FIFO; a debug line says so); upload_cmdfifo_not_empty,
//                upload_payload_not_empty and upload_payload_overflow are never raised.
//   Read buffer  LAST_READ_ADDR keeps its value ("updated by the HW when CSb is de-asserted"),
//                readbuf_watermark and readbuf_flip are never raised; CONTROL.FLASH_READ_BUFFER_CLR
//                and FLASH_STATUS_FIFO_CLR "automatically reset to 0".
//   ADDR_MODE    a software write sets pending, which "will remain set until HW consumes the
//                update": the next host command, so here it stays set.
//   FLASH_STATUS taken as committed when written (the document commits it on the SPI clock, which
//                needs a host); BUSY and WEL only cleared by software (rw0c), never set.
//   TPM          TPM_STATUS and TPM_CMD_ADDR read 0 (no TPM command received);
//                tpm_header_not_empty (status) stays low. A word written to TPM_READ_FIFO is
//                "written while a read command was not active, and it was not accepted":
//                tpm_rdfifo_drop is raised (INTR_STATE description).
//   Waiting      software that polls one of these registers kPollLimit times with no write to the
//                block in between waits for a SPI host: the run stops, saying so.
// The egress and ingress buffers are the window's plain memory (software's writes read back).
// Items: none (the bench's spi_device.chip.log holds each host transaction and the core's reads of
// what the host sent; there is no host here). Debug lines: CONTROL.MODE, the command slots
// (CMD_INFO: opcode, address, dummy cycles, payload, upload and busy), JEDEC, mailbox, read
// threshold, intercepts, ADDR_MODE, FLASH_STATUS, TPM_CFG as written, the first writes into the
// egress buffer by region, reads of empty FIFOs, dropped TPM read-FIFO words, interrupts raised.
// Not modelled: a SPI host (flash, passthrough and TPM transactions, so no upload, read-buffer,
// SFDP, JEDEC, status or TPM traffic), the passthrough to a downstream flash, the CDC delays, the
// SRAM parity errors.

#ifndef EG2_MODEL_IP_EG2_PERIPH_SPI_DEVICE_H_
#define EG2_MODEL_IP_EG2_PERIPH_SPI_DEVICE_H_

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../eg2_ip_model.h"

namespace eg2_model {

class Eg2SpiDevice : public Eg2IpModel {
 public:
  static constexpr unsigned kCmdInfos = 24;

  Eg2SpiDevice(sc_core::sc_module_name n, const eg2_regs::Block &d, uint32_t base)
      : Eg2IpModel(n, d, base) {
    control_ = reg("CONTROL");
    cfg_ = reg("CFG");
    addr_mode_ = reg("ADDR_MODE");
    flash_status_ = reg("FLASH_STATUS");
    jedec_cc_ = reg("JEDEC_CC");
    jedec_id_ = reg("JEDEC_ID");
    read_th_ = reg("READ_THRESHOLD");
    mailbox_ = reg("MAILBOX_ADDR");
    intercept_ = reg("INTERCEPT_EN");
    cmdfifo_ = reg("UPLOAD_CMDFIFO");
    addrfifo_ = reg("UPLOAD_ADDRFIFO");
    tpm_cfg_ = reg("TPM_CFG");
    tpm_status_ = reg("TPM_STATUS");
    tpm_cmd_addr_ = reg("TPM_CMD_ADDR");
    tpm_rdfifo_ = reg("TPM_READ_FIFO");
    for (unsigned i = 0; i < kCmdInfos; ++i)
      cmd_info_[i] = reg(("CMD_INFO_" + std::to_string(i)).c_str());
    cmd_info_en4b_ = reg("CMD_INFO_EN4B");
    cmd_info_wrdi_ = reg("CMD_INFO_WRDI");
    egress_ = win_index("egress_buffer");
    // The registers only a host changes: kept as they are (modelled), polled through poll_watch.
    for (const char *r : {"STATUS", "ADDR_MODE", "LAST_READ_ADDR", "FLASH_STATUS", "UPLOAD_STATUS",
                          "UPLOAD_STATUS2", "UPLOAD_CMDFIFO", "UPLOAD_ADDRFIFO", "TPM_STATUS",
                          "TPM_CMD_ADDR"}) {
      set_modelled(reg(r));
      host_regs_.push_back(reg(r));
    }
    host_regs_.push_back(uint16_t(intr_state_));
    debug_irqs();
  }

 protected:
  void reg_write(uint16_t r, uint32_t old, uint32_t wdata, uint32_t wmask) override {
    uint32_t v = value(r), w = wdata & wmask;
    if (r == control_) {
      if (w & 2) dbg(1, "CONTROL.FLASH_READ_BUFFER_CLR: read-buffer state cleared");
      if (w & 1) dbg(1, "CONTROL.FLASH_STATUS_FIFO_CLR: flash status FIFO cleared");
      set_value(control_, v & ~3u);  // "This CSR automatically resets to 0."
      if (((v ^ old) >> 4) & 3) {
        static const char *kMode[4] = {"disabled", "flash emulation", "passthrough", "reserved"};
        dbgf(1, "CONTROL.MODE: %s (no SPI host in eg2sim sends commands)", kMode[(v >> 4) & 3]);
      }
    } else if (r == addr_mode_) {
      set_value(addr_mode_, (v & 1) | (1u << 31));
      dbgf(1, "ADDR_MODE: %u-byte addresses requested; pending stays set until a SPI host's next "
              "command takes it (none in eg2sim)", (v & 1) ? 4 : 3);
    } else if (r == flash_status_ && v != old) {
      dbgf(1, "FLASH_STATUS 0x%06x (status 1-3 = 0x%02x 0x%02x 0x%02x, BUSY %u, WEL %u), taken as "
              "committed", v, v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff, v & 1, (v >> 1) & 1);
    } else if (r == tpm_rdfifo_) {
      if (dbg_count("TPM read FIFO words dropped", 8))
        dbgf(1, "TPM_READ_FIFO 0x%08x dropped: no TPM read command is active (no SPI host): "
                "tpm_rdfifo_drop", w);
      set_intr("tpm_rdfifo_drop");
    } else if (r == tpm_cfg_ && v != old) {
      dbgf(1, "TPM_CFG: TPM %s, %s mode%s%s%s", (v & 1) ? "on" : "off",
           (v & 2) ? "CRB" : "FIFO", (v & 4) ? ", hw_reg_dis" : "",
           (v & 8) ? ", tpm_reg_chk_dis" : "", (v & 16) ? ", invalid_locality" : "");
    } else if ((r == jedec_cc_ || r == jedec_id_) && v != old) {
      uint32_t cc = value(jedec_cc_), id = value(jedec_id_);
      dbgf(1, "JEDEC ID: %u continuation codes 0x%02x, manufacturer 0x%02x, device 0x%04x",
           (cc >> 8) & 0xff, cc & 0xff, (id >> 16) & 0xff, id & 0xffff);
    } else if ((r == cfg_ || r == mailbox_ || r == read_th_ || r == intercept_) && v != old) {
      dbgf(1, "%s = 0x%08x%s", desc().regs[r].name, v,
           r == cfg_ ? ((v >> 24) & 1 ? " (mailbox on)" : " (mailbox off)") : "");
    } else if (v != old && r >= cmd_info_[0] && r <= cmd_info_wrdi_ &&
               dbg_count("command slot writes", 32)) {
      debug_cmd_info(r, v);
    }
  }
  void reg_after_read(uint16_t r) override {
    if ((r == cmdfifo_ || r == addrfifo_ || r == tpm_cmd_addr_) &&
        dbg_count("reads of empty upload or TPM FIFOs", 4)) {
      dbgf(1, "%s read with its FIFO empty (no SPI host has sent anything): reads 0",
           desc().regs[r].name);
    }
    for (uint16_t h : host_regs_) {
      if (h != r) continue;
      poll_watch(r, [] {
        return std::string(
            "what software waits for comes from a SPI host on the bus (an uploaded command, a read "
            "of the read buffer, a TPM transaction), and eg2sim has none");
      });
    }
  }
  void win_write(uint16_t win, uint32_t off, unsigned len) override {
    polls_reset();
    if (int(win) != egress_ || !dbg_on(1)) return;
    // egress_buffer: the read buffer (words 0-511), mailbox (512-767), SFDP (768-831), TPM read
    // buffer (the rest).
    unsigned word = off / 4;
    const char *region = word < 512   ? "read buffer"
                         : word < 768 ? "mailbox"
                         : word < 832 ? "SFDP"
                                      : "TPM read buffer";
    if (dbg_on(2) || dbg_count(std::string("egress buffer writes, ") + region, 1))
      dbgf(1, "egress buffer: %u bytes written at 0x%03x (%s)", len, off, region);
  }

 private:
  void debug_cmd_info(uint16_t r, uint32_t v) {
    if (r >= cmd_info_en4b_) {
      dbgf(1, "%s: opcode 0x%02x, %s", desc().regs[r].name, v & 0xff,
           (v >> 31) ? "valid (handled by hardware)" : "not valid");
      return;
    }
    // The slots' assignment (theory_of_operation.md "Command Information List").
    unsigned slot = r - cmd_info_[0];
    const char *role = slot <= 2    ? "read status"
                       : slot == 3  ? "read JEDEC ID"
                       : slot == 4  ? "read SFDP"
                       : slot <= 10 ? "read command"
                                    : "software command";
    static const char *kAddr[4] = {"no address", "address size from ADDR_MODE", "3-byte address",
                                   "4-byte address"};
    std::string dummy = ((v >> 15) & 1) ? std::to_string(((v >> 12) & 7) + 1) + " cycles" : "none";
    std::string lanes;
    for (unsigned l = 0; l < 4; ++l) {
      if ((v >> (16 + l)) & 1) lanes += (lanes.empty() ? "SD" : "+SD") + std::to_string(l);
    }
    dbgf(1, "CMD_INFO_%u (%s slot): opcode 0x%02x, %s, %s, dummy %s, payload %s%s%s%s", slot, role,
         v & 0xff, (v >> 31) ? "valid" : "not valid", kAddr[(v >> 8) & 3], dummy.c_str(),
         lanes.empty() ? "none" : ((v >> 20) & 1) ? "to the host on " : "from the host on ",
         lanes.c_str(), ((v >> 24) & 1) ? ", upload" : "", ((v >> 25) & 1) ? ", sets BUSY" : "");
  }

  uint16_t control_, cfg_, addr_mode_, flash_status_, jedec_cc_, jedec_id_, read_th_, mailbox_,
      intercept_, cmdfifo_, addrfifo_, tpm_cfg_, tpm_status_, tpm_cmd_addr_, tpm_rdfifo_;
  uint16_t cmd_info_[kCmdInfos], cmd_info_en4b_, cmd_info_wrdi_;
  int egress_ = -1;
  std::vector<uint16_t> host_regs_;
};

}  // namespace eg2_model

#endif  // EG2_MODEL_IP_EG2_PERIPH_SPI_DEVICE_H_
