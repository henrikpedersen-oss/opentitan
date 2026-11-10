// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_MODEL_EG2_TRANSACTION_H_
#define EG2_MODEL_EG2_TRANSACTION_H_

#include <cstdint>
#include <tlm>

namespace eg2_model {

// Subtraction avoids wraparound in offset + length, including backdoor calls
// whose length is size_t rather than the TLM payload's unsigned length.
inline bool range_fits(uint64_t offset, uint64_t length, uint64_t size) {
  return offset <= size && length <= size - offset;
}

// These functional targets support contiguous reads and writes only. Reject
// unsupported payloads before invoking any device hooks or touching memory.
inline bool valid_transaction(tlm::tlm_generic_payload &transaction) {
  tlm::tlm_response_status status = tlm::TLM_OK_RESPONSE;
  if (!transaction.is_read() && !transaction.is_write())
    status = tlm::TLM_COMMAND_ERROR_RESPONSE;
  else if (!transaction.get_data_ptr() || !transaction.get_data_length())
    status = tlm::TLM_GENERIC_ERROR_RESPONSE;
  else if (transaction.get_byte_enable_ptr())
    status = tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE;
  else if (transaction.get_streaming_width() < transaction.get_data_length())
    status = tlm::TLM_BURST_ERROR_RESPONSE;
  transaction.set_dmi_allowed(false);
  if (status == tlm::TLM_OK_RESPONSE) return true;
  transaction.set_response_status(status);
  return false;
}

}  // namespace eg2_model

#endif  // EG2_MODEL_EG2_TRANSACTION_H_
