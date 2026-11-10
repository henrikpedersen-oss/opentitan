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
* [TileLink host interface](../../../dv/sv/tl_agent/README.md) on the `regs` port
* `cheriot_mem_sys_core_if` on the core data port: TL-UL, the tag sideband (`cored_tag_h2d_i`, `cored_tag_d2h_o`) and `cheriot_ena_i`
* TileLink host interfaces on the revocation bitmap ports (`corerevbm_tl`, `revbm_tl_d`)
* TileLink device interfaces on the host ports towards the main crossbar (`cored_tl_h`, `tbre_tl_h`) and the meta SRAM (`meta_sram_tl`)
* Interrupts ([`pins_if`](../../../dv/sv/common_ifs/README.md))
* [Alerts interface](../../../dv/sv/alert_agent/README.md)

The DUT is built with Earl Grey 2's address map, or with a small one (16 KiB SRAM, 8 KiB NVM; build mode `cms_map`) for the feature tests, which fill and sweep whole regions.

### Common DV utility components
The following utilities provide generic helper tasks and functions to perform activities that are common across the project:
* [dv_utils_pkg](../../../dv/sv/dv_utils/README.md)
* [csr_utils_pkg](../../../dv/sv/csr_utils/README.md)

### Global types & methods
All common types and methods defined at the package level can be found in `env/cheriot_mem_sys_env_pkg`: the address map, the meta SRAM map, the TBRE_STATUS fields, capability permissions, the fault kinds the memory devices inject, and the capability encoding helpers (`cap_base`, `cap_meta`, `revbm_index`, `tag_word_addr`).

### Agents
* **Core agent** (`core_agent/`, `cheriot_mem_sys_core_agent`): the core on the core data port.
  Its item (`cheriot_mem_sys_core_item`) is one access with its tag (store tag, or capability-load hint) and its response with the response tag.
  The driver presents one request at a time, with its tag, holds it until it is taken, and keeps up to two in flight, as Ibex does.
  The monitor publishes each completed access with both tags and the mode at the request's handshake.
* **TL-UL host agents** ([tl_agent](../../../dv/sv/tl_agent/README.md)) on `regs` (from the CIP environment), `revbm_tl_d` and `corerevbm_tl`.
* **TL-UL device agents** on `cored_tl_h`, `tbre_tl_h` and `meta_sram_tl`, answered by `cheriot_mem_sys_mem_device_seq`: a memory with the crossbar's error range and read-only NVM, and fault injection (an error response, a corrupted response or data integrity) by address range, direction and count.
* **Alert agent** on `fatal_fault` (from the CIP environment).

### UVM RAL Model
The CHERIoT memory subsystem RAL model is created with the [`ralgen`](../../../dv/tools/ralgen/README.md) FuseSoC generator script automatically when the simulation is at the build stage.

It can be created manually by invoking [`regtool`](../../../../util/reggen/doc/setup_and_use.md).

### Stimulus strategy
#### Tests and test sequences
Every test is in its own file in `hw/ip/cheriot_mem_sys/dv/tests` (package `cheriot_mem_sys_test_pkg`) and names the virtual sequence it runs.
The sequences reside in `hw/ip/cheriot_mem_sys/dv/env/seq_lib`, one per test, each describing what it checks.
They extend `cheriot_mem_sys_base_vseq`, which brings the environment up (reset, the memories preloaded, the memory devices started) and provides the operations the sequences are written in:
* core accesses (`core_store`, `core_load`, `core_store_cap`, `core_load_cap`, `core_issue`), each a `cheriot_mem_sys_core_access_seq` on the core agent
* bitmap accesses on the two bitmap agents, register accesses through the RAL, and malformed register accesses on the `regs` agent
* sweeps (`tbre_sweep`, `tbre_wait_idle`, `tbre_wait_intr`), resets, fault injection, and the checks (`check_tags`, `expect_alert`, `csr_expect`)

`cheriot_mem_sys_common_vseq` runs the common CSR, alert, interrupt and TL-UL tests.

#### Functional coverage
`cheriot_mem_sys_env_cov` samples, from the scoreboard:
* `core_access_cg`: every core access by opcode, size, tag, mode, region, error and response tag, crossed
* `bitmap_access_cg`: the bitmap ports by opcode and error

The CIP covergroups (CSR, interrupt and alert) are collected as well.

### Self-checking strategy
#### Scoreboard
`cheriot_mem_sys_scoreboard` keeps the RAL up to date from the `regs` port.
In the feature tests the C reference model of the subsystem (`dv/cms/cmodel`, written from the design specification, through DPI) is the checker.
Every completed access on the core data port, both bitmap ports and the `regs` port goes to the model, which says whether the response (data, tag, error) is allowed; the crossbar's error answers come from the `cored_tl_h` device agent.
At the end of the test the meta SRAM is compared with the model's image, word by word.
In the common tests the RAL checks the register reads; the registers the revocation engine changes (`intr_state`, `tbre_regwen`, `tbre_status`, `tbre_epoch`) are not compared there.
The tests judge `fatal_fault` themselves (`expect_alert`, `expect_no_alert`) from the alerts the scoreboard counts.

#### Assertions
* TLUL assertions: The `sva/cheriot_mem_sys_bind.sv` binds the `tlul_assert` [assertions](../../tlul/doc/TlulProtocolChecker.md) to the IP to ensure TileLink interface protocol compliance on every port: the `regs`, `cored_tl_d`, `corerevbm_tl` and `revbm_tl_d` device ports and the `cored`, `tbre` and `meta_sram` host ports
* CSR assertions: The `sva/cheriot_mem_sys_bind.sv` binds the generated `cheriot_mem_sys_regs_csr_assert_fpv` checks
* Unknown checks on DUT outputs: The RTL has assertions to ensure all outputs are initialized to known values after coming out of reset

## Second testbench: EG2 `cms`
The CHERIoT memory subsystem has a second testbench, built for Earl Grey 2: `cms`, in [`dv/cms`](cms/README.md). It is a class-based SystemVerilog testbench (its own TL-UL drivers, monitors and scoreboard; no UVM library, no RAL), so it also runs on Verilator.
The feature tests of this testbench were ported from it.
Its scoreboard predicts tags, data, the WTRC, the revocation sweep (including its races with core stores), the registers and the meta SRAM from the design specification.
The C reference model of the subsystem (`dv/cms/cmodel`) is shared by both testbenches.
Its testplan is [`cheriot_mem_sys_cms_testplan.hjson`](../data/cheriot_mem_sys_cms_testplan.hjson) (39 testpoints, 14 covergroups).

## Building and running tests
The [dvsim](https://github.com/lowRISC/dvsim) tool is used for building and running our tests and regressions.
Please take a look at the link for detailed information on the usage, capabilities, features and known issues.
Here's how to run the smoke test, and all the feature tests:
```console
$ dvsim $REPO_TOP/hw/ip/cheriot_mem_sys/dv/cheriot_mem_sys_sim_cfg.hjson -i cheriot_mem_sys_tag_smoke
$ dvsim $REPO_TOP/hw/ip/cheriot_mem_sys/dv/cheriot_mem_sys_sim_cfg.hjson -i feature
```

## Testplan
[Testplan](../data/cheriot_mem_sys_testplan.hjson)

The `cms` testbench: [testplan](../data/cheriot_mem_sys_cms_testplan.hjson)
