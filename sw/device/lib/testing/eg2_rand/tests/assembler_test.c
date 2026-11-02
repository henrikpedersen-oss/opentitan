// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_rand.h"

#define EG2_RAND_PARAM(name, value)                                                   \
  const uint32_t kEg2Param_##name __attribute__((section(".rodata"), used)) = value
EG2_RAND_PARAM(seed, 1);
EG2_RAND_PARAM(n_steps, 8);
EG2_RAND_PARAM(mode, 1);
EG2_RAND_PARAM(mutate, 1);

extern void eg2_rand_illegal_instruction(void);
extern volatile eg2_rand_observation_t eg2_rand_trap_observation;

static int setup(void *context, const eg2_rand_setup_t *setup,
                 eg2_rand_observation_t *observation) {
  (void)context;
  if (setup->mode != kEg2RandModeRiscv || setup->privilege != kEg2RandPrivilegeM ||
      setup->interrupt_enable != 0) {
    return -1;
  }
  uint32_t mstatus;
  __asm__ volatile("csrw mie, zero\n"
                   "csrci mstatus, 8\n"
                   "csrr %0, mstatus"
                   : "=r"(mstatus)
                   :
                   : "memory");
  observation->mstatus = mstatus;
  return 0;
}

static int execute(void *context, const eg2_rand_action_t *action,
                   eg2_rand_observation_t *observation) {
  (void)context;
  eg2_rand_trap_observation.trap_taken = 0;
  if (action->kind == kEg2RandAluAdd) {
    observation->result = action->lhs + action->rhs;
    return 0;
  }
  eg2_rand_illegal_instruction();
  *observation = eg2_rand_trap_observation;
  return 0;
}

int main(void) {
  const eg2_rand_constraints_t constraints = {
      .modes = kEg2RandModeRiscv,
      .privileges = kEg2RandPrivilegeM,
      .actions = kEg2RandActionAlu | kEg2RandActionIllegal,
      .max_steps = 1000,
  };
  const eg2_rand_params_t params = {
      *(volatile const uint32_t *)&kEg2Param_seed,
      *(volatile const uint32_t *)&kEg2Param_n_steps,
      *(volatile const uint32_t *)&kEg2Param_mode,
      *(volatile const uint32_t *)&kEg2Param_mutate,
  };
  const eg2_rand_platform_t platform = {
      .illegal_address = (uint32_t)(uintptr_t)eg2_rand_illegal_instruction,
      .setup = setup,
      .execute = execute,
  };
  return eg2_rand_run(&constraints, &params, &platform, 0);
}
