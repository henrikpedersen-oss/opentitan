// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_rand.h"
#include "eg2_rand_environment.h"

int eg2_rand_rtos_compartment_test(void) {
  const eg2_rand_constraints_t constraints = EG2_RAND_CORE_SMOKE_CONSTRAINTS;
  return eg2_rand_run(&constraints, eg2_rand_environment_params(), eg2_rand_rtos_platform(), 0);
}
