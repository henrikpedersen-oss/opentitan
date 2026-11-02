// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#ifndef EG2_RAND_H_
#define EG2_RAND_H_

#include <stdint.h>

enum {
  kEg2RandModeRiscv = 1u << 0,
  kEg2RandModeCheriot = 1u << 1,
  kEg2RandPrivilegeM = 1u << 0,
  kEg2RandPrivilegeU = 1u << 1,
  kEg2RandActionAlu = 1u << 0,
  kEg2RandActionIllegal = 1u << 1,
};

typedef struct eg2_rand_constraints {
  uint32_t modes;
  uint32_t privileges;
  uint32_t actions;
  uint32_t max_steps;
} eg2_rand_constraints_t;

typedef struct eg2_rand_params {
  uint32_t seed;
  uint32_t n_steps;
  // Zero draws from the constraints; one and two select RISC-V and CHERIoT respectively.
  uint32_t mode;
  // Zero disables mutation, one enables it, and two draws the setting from the seed.
  uint32_t mutate;
} eg2_rand_params_t;

typedef struct eg2_rand_setup {
  uint32_t mode;
  uint32_t privilege;
  uint32_t interrupt_enable;
} eg2_rand_setup_t;

typedef enum eg2_rand_action_kind {
  kEg2RandAluAdd,
  kEg2RandIllegal,
} eg2_rand_action_kind_t;

typedef struct eg2_rand_action {
  eg2_rand_action_kind_t kind;
  uint32_t lhs;
  uint32_t rhs;
  uint32_t instruction;
  uint32_t address;
} eg2_rand_action_t;

typedef struct eg2_rand_observation {
  uint32_t trap_taken;
  uint32_t result;
  uint32_t mcause;
  uint32_t mtval;
  uint32_t mepc;
  uint32_t mstatus;
} eg2_rand_observation_t;

typedef struct eg2_rand_platform {
  // The address of the adapter's fixed illegal-instruction stub, not a hardware observation.
  uint32_t illegal_address;
  int (*setup)(void *context, const eg2_rand_setup_t *setup,
               eg2_rand_observation_t *observation);
  int (*execute)(void *context, const eg2_rand_action_t *action,
                 eg2_rand_observation_t *observation);
  void (*report)(void *context, uint32_t step, const char *field, uint32_t got,
                 uint32_t expected);
} eg2_rand_platform_t;

// Returns the number of failed checks, or -1 when constraints, parameters or the adapter are bad.
int eg2_rand_run(const eg2_rand_constraints_t *constraints, const eg2_rand_params_t *params,
                 const eg2_rand_platform_t *platform, void *context);

#endif  // EG2_RAND_H_
