// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "sw/device/tests/eg2_boot/eg2_boot_svc.h"

#include "sw/device/silicon_creator/lib/boot_svc/boot_svc_header.h"
#include "sw/device/silicon_creator/lib/error.h"

enum { kWords = sizeof(boot_svc_msg_t) / sizeof(uint32_t) };

void eg2_boot_svc_put(const boot_svc_msg_t *msg) {
  volatile uint32_t *dst = (volatile uint32_t *)&eg2_boot_retram()->creator.boot_svc_msg;
  const uint32_t *src = (const uint32_t *)msg;
  for (uint32_t i = 0; i < kWords; ++i) {
    dst[i] = src[i];
  }
}

void eg2_boot_svc_get(boot_svc_msg_t *msg) {
  const volatile uint32_t *src =
      (const volatile uint32_t *)&eg2_boot_retram()->creator.boot_svc_msg;
  uint32_t *dst = (uint32_t *)msg;
  for (uint32_t i = 0; i < kWords; ++i) {
    dst[i] = src[i];
  }
}

bool eg2_boot_svc_check_response(uint32_t type, boot_svc_msg_t *msg) {
  eg2_boot_svc_get(msg);
  bool ok = eg2_boot_check("boot_svc response header (digest, length)", kErrorOk,
                           boot_svc_header_check(&msg->header));
  return eg2_boot_check("boot_svc response type", type, msg->header.type) && ok;
}
