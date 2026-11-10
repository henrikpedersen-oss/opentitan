# CHERIoT Memory Subsystem DV document

## Goals
* **DV**
  * Verify all CHERIoT memory subsystem IP features by running dynamic simulations with a SV/UVM based testbench
  * Develop and run all tests based on the [testplan](#testplan) below towards closing code and functional coverage on the IP and all of its sub-modules
* **FPV**
  * Verify TileLink device protocol compliance with an SVA based testbench
  * Verify the main functional path (tag filter, WTRC, RMW filter, revocation engine) against its specification (planned)

## Current status
* [Design & verification stage](../../../README.md)
  * [HW development stages](../../../../doc/project_governance/development_stages.md)
* [Simulation results](https://reports.opentitan.org/hw/ip/cheriot_mem_sys/dv/latest/report.html)

## Design features
For detailed information on the CHERIoT memory subsystem design features, please see the [CHERIoT memory subsystem design specification](../README.md).

## Testbench architecture
The CHERIoT memory subsystem testbench has been constructed based on the [CIP testbench architecture](../../../dv/sv/cip_lib/README.md).

### Block diagram
![Block diagram](./doc/tb.svg)

### Top level testbench
Top level testbench is located at `hw/ip/cheriot_mem_sys/dv/tb.sv`. It instantiates the CHERIoT memory subsystem DUT module `hw/ip/cheriot_mem_sys/rtl/cheriot_mem_sys.sv`.
In addition, it instantiates the following interfaces, connects them to the DUT and sets their handle into `uvm_config_db`:
* [Clock and reset interface](../../../dv/sv/common_ifs/README.md)
* [TileLink host interface](../../../dv/sv/tl_agent/README.md) (`regs` CSR port)
* Interrupts ([`pins_if`](../../../dv/sv/common_ifs/README.md))
* [Alerts interface](../../../dv/sv/alert_agent/README.md)

The core data port with its tag sideband (`cored_tl_d`, `cored_tag`), the revocation bitmap ports (`corerevbm_tl`, `revbm_tl_d`), the host ports towards the main crossbar (`cored_tl_h`, `tbre_tl_h`) and the meta SRAM (`meta_sram_tl_h`) are tied to their idle values, and `cheriot_ena_i` to `MuBi4False`.
These ports are not exercised by this testbench yet.

### Common DV utility components
The following utilities provide generic helper tasks and functions to perform activities that are common across the project:
* [dv_utils_pkg](../../../dv/sv/dv_utils/README.md)
* [csr_utils_pkg](../../../dv/sv/csr_utils/README.md)

### Global types & methods
All common types and methods defined at the package level can be found in `env/cheriot_mem_sys_env_pkg`.
Some of them in use are:
```systemverilog
parameter uint   NUM_ALERTS = 1;
parameter string LIST_OF_ALERTS[NUM_ALERTS] = {"fatal_fault"};
```

### TL_agent
The CHERIoT memory subsystem testbench instantiates (already handled in CIP base env) [tl_agent](../../../dv/sv/tl_agent/README.md)
which provides the ability to drive and independently monitor random traffic via
TL host interface into the `regs` port of the CHERIoT memory subsystem device.

### UVM RAL Model
The CHERIoT memory subsystem RAL model is created with the [`ralgen`](../../../dv/tools/ralgen/README.md) FuseSoC generator script automatically when the simulation is at the build stage.

It can be created manually by invoking [`regtool`](../../../../util/reggen/doc/setup_and_use.md).

### Stimulus strategy
#### Test sequences
All test sequences reside in `hw/ip/cheriot_mem_sys/dv/env/seq_lib`.
The `cheriot_mem_sys_base_vseq` virtual sequence is extended from `cip_base_vseq` and serves as a starting point.
All test sequences are extended from `cheriot_mem_sys_base_vseq`.
It provides commonly used handles, variables, functions and tasks that the test sequences can simply use / call.
The following test sequences exist:
* `cheriot_mem_sys_common_vseq` : runs the common CSR, alert, interrupt and TL-UL tests
* `cheriot_mem_sys_smoke_vseq`  : smoke test; its body is still empty

#### Functional coverage
To ensure high quality constrained random stimulus, it is necessary to develop a functional coverage model.
The CIP covergroups (CSR, interrupt and alert) are collected; `cheriot_mem_sys_env_cov` does not define any covergroups of its own yet.

### Self-checking strategy
#### Scoreboard
The `cheriot_mem_sys_scoreboard` is primarily used for end to end checking.
It retrieves the data monitored by the TL agent on the `regs` port (TL address and data channels).

For a write transaction, during the address channel, CSR values are updated in RAL.
For a read transaction, during the data channel, the scoreboard compares the read data with the mirrored value in RAL.
`intr_state`, `tbre_regwen` and `tbre_status` are updated by the hardware and are not compared.
Alerts are not checked by the scoreboard yet (`do_alert_check` is 0).

#### Assertions
* TLUL assertions: The `sva/cheriot_mem_sys_bind.sv` binds the `tlul_assert` [assertions](../../tlul/doc/TlulProtocolChecker.md) to the IP to ensure TileLink interface protocol compliance on the `regs` device port and on the `cored`, `tbre` and `meta_sram` host ports
* CSR assertions: The `sva/cheriot_mem_sys_bind.sv` binds the generated `cheriot_mem_sys_regs_csr_assert_fpv` checks
* Unknown checks on DUT outputs: The RTL has assertions to ensure all outputs are initialized to known values after coming out of reset

## Second testbench: EG2 `cms`
The CHERIoT memory subsystem has a second testbench, built for Earl Grey 2: `cms`, in [`dv/cms`](cms/README.md). It is a class-based SystemVerilog testbench (its own TL-UL drivers, monitors and scoreboard; no UVM library, no RAL), so it also runs on Verilator.
Where this testbench covers the `regs` port, `cms` drives every port of the subsystem:
* the core data port with its tag sideband, both revocation bitmap ports and the `regs` port, through TL-UL host agents
* the host ports towards the main crossbar and the meta SRAM, through TL-UL memory models that also inject errors and integrity faults
* `cheriot_ena_i` with every MuBi4 encoding, the alert and the interrupt

Its scoreboard predicts tags, data, the WTRC, the revocation sweep (including its races with core stores), the registers and the meta SRAM from the design specification.
The C reference model of the subsystem (`dv/cms/cmodel`, also written from the specification) checks every transaction as a second checker through DPI.
Its testplan is [`cheriot_mem_sys_cms_testplan.hjson`](../data/cheriot_mem_sys_cms_testplan.hjson) (39 testpoints, 14 covergroups).

## Building and running tests
The [dvsim](https://github.com/lowRISC/dvsim) tool is used for building and running our tests and regressions.
Please take a look at the link for detailed information on the usage, capabilities, features and known issues.
Here's how to run a smoke test:
```console
$ dvsim $REPO_TOP/hw/ip/cheriot_mem_sys/dv/cheriot_mem_sys_sim_cfg.hjson -i cheriot_mem_sys_smoke
```

## Testplan
[Testplan](../data/cheriot_mem_sys_testplan.hjson)

The `cms` testbench: [testplan](../data/cheriot_mem_sys_cms_testplan.hjson)
