# Contributing to the EG2 model

Start with [README.md](README.md) and the [architecture guide](docs/architecture.md).
This is a C++/SystemC functional model. The DPI library can supply predictions
to a UVM scoreboard; this directory itself is not a UVM testbench.

## Behavioral changes

Describe a concrete failing case and cite the IP document, HJSON register
description or architectural specification that defines the expected result.
Identify an RTL-derived exception explicitly. A test passing on RTL alone does
not establish a documented requirement.

Keep observations and predictions separate. Images, software stores and external
stimuli may update reference state. A value observed from the DUT must not become
the expected value for a later comparison. If required behavior is unspecified,
stop with an `undocumented:` reason and record the question. If behavior is
specified but not implemented, describe that as a model limitation.

Add a focused regression that fails with the old implementation. Prefer an
independent known-answer vector or observable consequence over reproducing the
implementation in the test. A negative test must fail for its intended reason;
a crash, timeout, missing image or earlier unrelated failure is not sufficient.

## Structure and readability

- Keep a device's behavior in its own file. Separate reusable algorithms from
  the SystemC and DPI bindings. Preserve established include entry points when
  splitting files.
- Use names that describe the operation. Explain ordering, units, masks and
  specification exceptions where they matter; omit development-phase history.
- Keep generated tables generated. Update their source/generator and regenerate
  them rather than editing `eg2_model_regs.h`, `eg2_model_irqs.h` or the RTOS table.
- Preserve the license and provenance of imported reference code. The OTP
  constants in this model are the public simulation constants; they must stay
  consistent with the generated Earl Grey testing configuration.
- Coordinate changes to shared reference cores with their owners. In particular,
  the CMS core is shared with the block verification environment.

## Validation

From this directory, in its pinned development shell:

```sh
nix develop --command make OUT=build check
nix develop --command make OUT=build build/eg2sim lib replay
```

`utility_tests` needs only a C++17 compiler. `transport_test` adds SystemC.
`check` includes both, the device-core tests and their negative variants, shared
runtime tests, public C API tests and replay rejection cases.

For platform, loader or shared IP changes, also compare standalone firmware and
RTOS runs against a control build using the **same firmware artifacts**, and run
a real DPI replay with expected UART output. Record verdict and output changes,
including failures already present in the control. Crypto changes need
independent vectors. Reset or fault changes need the relevant negative tests.
The verification workspace provides `scripts/model_tests.sh`,
`scripts/eg2_model_mutation.py` and the software-suite runners for these checks.

In the change description, distinguish tests that ran from source inspection,
and evaluated behavior from unpredicted accesses. List the remaining limits.
See the [follow-up review](docs/review-followup.md) for an example with control
results, regression evidence and unresolved model-fidelity issues.
