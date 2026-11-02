// Copyright lowRISC contributors.
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0

#include "eg2_rand.h"

enum {
  kMstatusMie = 1u << 3,
  kMstatusMpie = 1u << 7,
  kMstatusMpp = 3u << 11,
  kMstatusTrapMask = kMstatusMie | kMstatusMpie | kMstatusMpp,
  kMstatusTrapFromM = kMstatusMpp,
  kIllegalInstructionCause = 2,
};

typedef struct eg2_rand_model {
  uint32_t random;
  uint32_t failures;
  uint32_t mutation_seen;
} eg2_rand_model_t;

// Xorshift32 has a fully specified sequence and needs no runtime support.
static uint32_t next(eg2_rand_model_t *model) {
  uint32_t value = model->random;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  model->random = value;
  return value;
}

static uint32_t pick_bit(eg2_rand_model_t *model, uint32_t mask) {
  uint32_t count = 0;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    count += (mask >> bit) & 1u;
  }
  if (count == 0) {
    return 0;
  }
  uint32_t selected = next(model) % count;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    if (((mask >> bit) & 1u) != 0 && selected-- == 0) {
      return 1u << bit;
    }
  }
  return 0;
}

static void check(eg2_rand_model_t *model, const eg2_rand_platform_t *platform,
                  void *context, uint32_t step, const char *field, uint32_t got,
                  uint32_t expected) {
  if (got == expected) {
    return;
  }
  ++model->failures;
  if (platform->report != 0) {
    platform->report(context, step, field, got, expected);
  }
}

static int valid(const eg2_rand_constraints_t *constraints, const eg2_rand_params_t *params,
                 const eg2_rand_platform_t *platform) {
  if (constraints == 0 || params == 0 || platform == 0 || platform->setup == 0 ||
      platform->execute == 0 || params->seed == 0 || params->n_steps == 0 ||
      params->n_steps > constraints->max_steps || constraints->modes == 0 ||
      constraints->privileges == 0 || (constraints->actions & kEg2RandActionAlu) == 0) {
    return 0;
  }
  if (params->mode > 2 || params->mutate > 2) {
    return 0;
  }
  uint32_t requested_mode = params->mode == 0 ? 0 : 1u << (params->mode - 1);
  if (requested_mode != 0 && (requested_mode & constraints->modes) == 0) {
    return 0;
  }
  return 1;
}

int eg2_rand_run(const eg2_rand_constraints_t *constraints, const eg2_rand_params_t *params,
                 const eg2_rand_platform_t *platform, void *context) {
  if (!valid(constraints, params, platform)) {
    return -1;
  }
  eg2_rand_model_t model = {.random = params->seed};
  eg2_rand_setup_t setup = {
      .mode = params->mode == 0 ? pick_bit(&model, constraints->modes)
                                : 1u << (params->mode - 1),
      .privilege = pick_bit(&model, constraints->privileges),
      .interrupt_enable = 0,
  };
  uint32_t mutate = params->mutate == 2 ? next(&model) & 1u : params->mutate;
  if ((constraints->actions & kEg2RandActionIllegal) == 0) {
    mutate = 0;
  }

  eg2_rand_observation_t observation = {0};
  if (platform->setup(context, &setup, &observation) != 0) {
    return -1;
  }
  check(&model, platform, context, 0, "setup trap", observation.trap_taken, 0);
  check(&model, platform, context, 0, "mstatus.MIE", observation.mstatus & kMstatusMie, 0);

  for (uint32_t step = 0; step < params->n_steps; ++step) {
    eg2_rand_action_t action = {
        .kind = kEg2RandAluAdd,
        .lhs = next(&model),
        .rhs = next(&model),
    };
    // An enabled mutation run always contains a negative check, even for a one-step smoke test.
    if (mutate && ((next(&model) & 3u) == 0 ||
                   (!model.mutation_seen && step + 1 == params->n_steps))) {
      action.kind = kEg2RandIllegal;
      action.instruction = 0xffffffffu;
      action.address = platform->illegal_address;
      model.mutation_seen = 1;
    }
    observation = (eg2_rand_observation_t){0};
    if (platform->execute(context, &action, &observation) != 0) {
      return -1;
    }
    if (action.kind == kEg2RandAluAdd) {
      check(&model, platform, context, step + 1, "legal trap", observation.trap_taken, 0);
      check(&model, platform, context, step + 1, "add result", observation.result,
            action.lhs + action.rhs);
      continue;
    }

    // Ibex documentation, Exceptions and Interrupts, says trap entry saves the current PC,
    // copies MIE to MPIE and enters M-mode with MIE clear. Its CSR documentation says an illegal
    // instruction has cause 2 and mtval contains the faulting instruction.
    check(&model, platform, context, step + 1, "illegal trap", observation.trap_taken, 1);
    check(&model, platform, context, step + 1, "mcause", observation.mcause,
          kIllegalInstructionCause);
    check(&model, platform, context, step + 1, "mtval", observation.mtval,
          action.instruction);
    check(&model, platform, context, step + 1, "mepc", observation.mepc, action.address);
    check(&model, platform, context, step + 1, "mstatus trap fields",
          observation.mstatus & kMstatusTrapMask, kMstatusTrapFromM);
  }
  return (int)model.failures;
}
