// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_nist/eg2_nist_sha2_engine.h"

#include "hw/top/dt/api.h"     // Generated.
#include "hw/top/dt/hmac.h"    // Generated.
#include "hw/top/hmac_regs.h"  // Generated.
#include "sw/device/lib/base/mmio.h"
#include "sw/device/lib/testing/eg2/eg2_test.h"

enum {
  kPolls = 200000,
  kKeyWords = 32,
};

static mmio_region_t hmac;

void eg2_nist_sha2_init(void) {
  hmac = mmio_region_from_addr(dt_hmac_primary_reg_block(kDtHmac));
}

static bool fail(const char *what) {
  eg2_puts("hmac: ");
  eg2_puts(what);
  eg2_puts("\r\n");
  return false;
}

static uint32_t digest_size(uint32_t bits) {
  return bits == 256   ? HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_256
         : bits == 384 ? HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_384
                       : HMAC_CFG_DIGEST_SIZE_VALUE_SHA2_512;
}

static bool push(const uint8_t *msg, uint32_t len, uint32_t max_burst) {
  uint32_t i = 0;
  while (i < len) {
    uint32_t polls = 0;
    while (mmio_region_get_bit32(hmac, HMAC_STATUS_REG_OFFSET, HMAC_STATUS_FIFO_FULL_BIT)) {
      if (++polls == kPolls) {
        return fail("message FIFO stays full");
      }
    }
    uint32_t burst = eg2_rand_range(1, max_burst);
    for (uint32_t w = 0; w < burst && i < len; ++w) {
      if (len - i >= 4) {
        uint32_t word = (uint32_t)msg[i] | ((uint32_t)msg[i + 1] << 8) |
                        ((uint32_t)msg[i + 2] << 16) | ((uint32_t)msg[i + 3] << 24);
        mmio_region_write32(hmac, HMAC_MSG_FIFO_REG_OFFSET, word);
        i += 4;
      } else {
        mmio_region_write8(hmac, HMAC_MSG_FIFO_REG_OFFSET, msg[i]);
        i += 1;
      }
    }
  }
  return true;
}

// One hash or HMAC with a key already in the engine's supported form (`key_words` words).
static bool run_once(uint32_t bits, const uint32_t *key, uint32_t key_length_value,
                     uint32_t key_words, const uint8_t *msg, uint32_t len, uint32_t max_burst,
                     uint8_t *out) {
  bool keyed = key != NULL;
  uint32_t cfg = 0;
  cfg = bitfield_field32_write(cfg, HMAC_CFG_DIGEST_SIZE_FIELD, digest_size(bits));
  cfg = bitfield_field32_write(cfg, HMAC_CFG_KEY_LENGTH_FIELD,
                               keyed ? key_length_value : HMAC_CFG_KEY_LENGTH_VALUE_KEY_NONE);
  cfg = bitfield_bit32_write(cfg, HMAC_CFG_HMAC_EN_BIT, keyed);
  // Configure with the engine off, load the key, then enable it (programmer's guide).
  mmio_region_write32(hmac, HMAC_CFG_REG_OFFSET, cfg);
  for (uint32_t i = 0; i < kKeyWords; ++i) {
    mmio_region_write32(hmac, HMAC_KEY_0_REG_OFFSET + (ptrdiff_t)(4 * i), keyed && i < key_words ? key[i] : 0);
  }
  mmio_region_write32(hmac, HMAC_INTR_STATE_REG_OFFSET,
                      (1u << HMAC_INTR_STATE_HMAC_DONE_BIT) | (1u << HMAC_INTR_STATE_HMAC_ERR_BIT));
  mmio_region_write32(hmac, HMAC_CFG_REG_OFFSET, bitfield_bit32_write(cfg, HMAC_CFG_SHA_EN_BIT, true));
  mmio_region_write32(hmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_START_BIT);
  if (!push(msg, len, max_burst)) {
    return false;
  }
  mmio_region_write32(hmac, HMAC_CMD_REG_OFFSET, 1u << HMAC_CMD_HASH_PROCESS_BIT);
  uint32_t polls = 0;
  for (;;) {
    uint32_t intr = mmio_region_read32(hmac, HMAC_INTR_STATE_REG_OFFSET);
    if (intr & (1u << HMAC_INTR_STATE_HMAC_ERR_BIT)) {
      eg2_puts("hmac: ERR_CODE 0x");
      eg2_puthex(mmio_region_read32(hmac, HMAC_ERR_CODE_REG_OFFSET));
      return fail("error");
    }
    if (intr & (1u << HMAC_INTR_STATE_HMAC_DONE_BIT)) {
      break;
    }
    if (++polls == kPolls) {
      return fail("no hmac_done");
    }
  }
  mmio_region_write32(hmac, HMAC_INTR_STATE_REG_OFFSET, 1u << HMAC_INTR_STATE_HMAC_DONE_BIT);
  for (uint32_t i = 0; i < bits / 32; ++i) {
    uint32_t d = mmio_region_read32(hmac, HMAC_DIGEST_0_REG_OFFSET + (ptrdiff_t)(4 * i));
    out[4 * i] = (uint8_t)(d >> 24);
    out[4 * i + 1] = (uint8_t)(d >> 16);
    out[4 * i + 2] = (uint8_t)(d >> 8);
    out[4 * i + 3] = (uint8_t)d;
  }
  // Disable: clears the digest and the key from the engine's use.
  mmio_region_write32(hmac, HMAC_CFG_REG_OFFSET, cfg);
  return true;
}

bool eg2_nist_sha2_run(uint32_t bits, const uint8_t *key, uint32_t key_len, const uint8_t *msg,
                       uint32_t msg_len, uint32_t max_burst, uint8_t *out) {
  if (key == NULL) {
    return run_once(bits, NULL, 0, 0, msg, msg_len, max_burst, out);
  }
  uint32_t block = bits == 256 ? 64 : 128;
  uint8_t hashed[64];
  if (key_len > block) {
    // HMAC: a key longer than the block is replaced by its hash.
    if (!run_once(bits, NULL, 0, 0, key, key_len, max_burst, hashed)) {
      return false;
    }
    key = hashed;
    key_len = bits / 8;
  }
  static const struct {
    uint32_t bytes, value;
  } kLengths[] = {
      {16, HMAC_CFG_KEY_LENGTH_VALUE_KEY_128}, {32, HMAC_CFG_KEY_LENGTH_VALUE_KEY_256},
      {48, HMAC_CFG_KEY_LENGTH_VALUE_KEY_384}, {64, HMAC_CFG_KEY_LENGTH_VALUE_KEY_512},
      {128, HMAC_CFG_KEY_LENGTH_VALUE_KEY_1024},
  };
  uint32_t value = 0, bytes = 0;
  for (uint32_t i = 0; i < sizeof(kLengths) / sizeof(kLengths[0]); ++i) {
    if (key_len <= kLengths[i].bytes && kLengths[i].bytes <= block) {
      value = kLengths[i].value;
      bytes = kLengths[i].bytes;
      break;
    }
  }
  if (value == 0) {
    return fail("no supported key length");
  }
  // KEY_i big-endian, the key zero-padded at its end to the configured length.
  uint32_t words[kKeyWords];
  for (uint32_t i = 0; i < bytes / 4; ++i) {
    uint32_t w = 0;
    for (uint32_t b = 0; b < 4; ++b) {
      uint32_t at = 4 * i + b;
      w = (w << 8) | (at < key_len ? key[at] : 0);
    }
    words[i] = w;
  }
  return run_once(bits, words, value, bytes / 4, msg, msg_len, max_burst, out);
}
