// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_RAND_ENVIRONMENT_H_
#define EG2_RAND_ENVIRONMENT_H_

#include "eg2_rand.h"

#define EG2_RAND_CORE_SMOKE_CONSTRAINTS                                      \
  {                                                                          \
    kEg2RandModeRiscv | kEg2RandModeCheriot, kEg2RandPrivilegeM,             \
        kEg2RandActionAlu | kEg2RandActionIllegal, 1000                      \
  }

// These adapters belong in the EG2 fork and RTOS firmware respectively. Keeping them out of the
// common library ensures that it never learns an expected value from a DIF, CSR or RTOS service.
const eg2_rand_params_t *eg2_rand_environment_params(void);
const eg2_rand_platform_t *eg2_rand_bare_metal_platform(void);
const eg2_rand_platform_t *eg2_rand_rtos_platform(void);

#endif  // EG2_RAND_ENVIRONMENT_H_
