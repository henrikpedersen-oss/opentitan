# CHERIoT memory subsystem: block-level testbench

A SystemVerilog testbench for the CHERIoT memory subsystem on its own: the top module
`cheriot_mem_sys` (`hw/ip/cheriot_mem_sys/rtl/cheriot_mem_sys.sv` in this tree), unmodified. It is the
second testbench of the subsystem, next to the CIP testbench in `../` (`../README.md`). The testplan
is [`cheriot_mem_sys_cms_testplan.hjson`](../../data/cheriot_mem_sys_cms_testplan.hjson); it also says
which obligations are left to the RTOS firmware and to formal.

The C reference model of the subsystem, written from the design specification, is in `cmodel/`
(`cms_core.h`, its DPI layer `cms_dpi.cc` and package `cms_cmodel_pkg.sv`, and its unit test); with
`+cms_cmodel=1` it checks every transaction beside the scoreboard and reports, with `=2` a mismatch
fails the run (`cms_regress.sh -m 1|2`; `cms_random_cmodel` runs it by default).

The scripts run in the EDA environment (xrun; verilator and python3 with hjson for lint and the
file list), from the repository root. The RTL is this tree's (`cms_rtl.f`). The two RTL covergroup
files the bench binds come from the Ibex and CHERIoT RTOS benches (`cov/`).

Until now the subsystem was exercised only indirectly, by CHERIoT RTOS firmware on the RTOS test SoC
(the CHERIoT RTOS suite), and by OpenTitan's CSR-only smoke environment. Firmware cannot
inject faults, race the revocation engine against a store on a chosen cycle, drive invalid
`cheriot_ena_i` encodings, or compare the whole tag store after every sweep; this bench does.

## Structure

```
cms_tl_host x4 ──> cored_tl_d (+ tag sideband) ┐
                   revbm_tl_d                  │            ┌──> cored_tl_h ─┐
                   corerevbm_tl                ├─ cheriot ──┼──> tbre_tl_h  ─┴─ cms_tl_mem x2 (one data store)
                   regs_tl_d                   ┘            └──> meta_sram_tl ── cms_tl_mem (meta SRAM)
                                                  alert_tx ──> prim_alert_receiver
                                                  intr_tbre_done_o ──> checked against the model
```

| File | What it is |
|---|---|
| `tb/cms_tb.sv` | Top: DUT, hosts, memories, alert receiver, `tlul_assert` on all seven ports, always-on checks, helper tasks, main |
| `tb/cms_tests.svh` | The tests (`make tests` lists them) |
| `tb/cms_tests_eg2.svh` | The tests of the Earl Grey 2 specification's REQ_CMS_01, 02 and 07 (see "Tests from the Earl Grey 2 specification") |
| `tb/cms_pkg.sv` | Address map, transactions, TB storage, the reference model / scoreboard (`cms_sb`), TB covergroups (`cms_cov`) |
| `tb/cms_tl_host.sv` | TL-UL host driver: queued requests, up to N outstanding, random gaps and `d_ready`, integrity generation, response checks (integrity included); holds back after a capability store's second word, as Ibex does |
| `tb/cms_tl_mem.sv` | TL-UL memory model: random `a_ready`/latency, response integrity, request checks, fault injection; the NVM is read-only on the bus (tests program it through the backdoor) |
| `tb/cms_fcov_bind.sv` | Binds the RTOS SoC's RTL covergroups (`../cheriot-rtos-test-suite/dv/cov/`) into this bench |
| `cms_dv.f` | File list (Verilator): `cms_rtl.f` (this tree's RTL) + the testbench |
| `cms_xlm.f` | Explicit file list for Xcelium, **generated** from `cms_dv.f` by `make filelist` (`make lint` checks it is current) |
| `cms_lint.vlt` | Lint waivers (combinational-loop notices and vendored-code notes only) |
| `cms_xlm_build.sh`, `cms_xlm_run.sh`, `cms_regress.sh` | Xcelium build, one run, the regression |
| `cms_mutation.sh` | The mutation check: one build, every mutation run, the grade (see below) |
| `cms_regress.list` | Every run the testplan needs: name, test, seeds, plusargs |
| `cms_cover.ccf` | Code coverage of `cheriot_mem_sys` and below, on top of the shared `../coverage/ibex_cover.ccf` |

The RTL is built with `cheriot_rtl.f`'s library set (ibex's `vendor` prim and pulp cells,
this tree's TL-UL), which elaborates standalone. The default address map is small (16 KiB SRAM, 8 KiB NVM) so whole-SRAM sweeps and
full tag-store compares are cheap; `MAP=earlgrey` builds with the RTL's defaults.

## The reference model

`cms_sb` is written from the design documents (`hw/ip/cheriot_mem_sys/doc/
theory_of_operation.md`, `registers.md`, `programmers_guide.md`), `cheriot_mem_sys.hjson` and the CHERIoT capability encoding. It never reads a DUT
signal, never calls `ibex_cheriot_pkg` (the RTL's own bounds functions), and never copies DUT state
into itself. It keeps:

- a **shadow tag map**, one predicted tag per 8-byte granule of SRAM and NVM. A prediction is a
  set: exact normally, `{0,1}` where the document allows either (a load of a revocable capability
  while a sweep runs, a store in flight at reset); the next observation resolves it;
- the **revocation bitmap**, from writes through the software window;
- **data**, from core writes and NVM programming (the engine must never change it; a capability
  store to the NVM never writes it);
- the **WTRC sequence** of capability stores to the NVM: which word answers d_error, when the tag
  is set;
- the **CSRs**: INTR_STATE/INTR_ENABLE (and the interrupt pin), TBRE_STATUS's sticky start_err
  and sweep_err, TBRE_EPOCH, and the sweep in progress: the exact sequence of words the engine
  must read and, when the engine is seen inactive (busy 0, REGWEN 1 or an even epoch), the
  predicted tag of every swept capability (its own decode of the base, sealing exemption, bitmap
  range). A core write to a swept capability is timed against the engine's reads seen on
  `tbre_tl_h`: answered at or after the read of the capability's lower word was presented, the
  write's tag stands; answered before the engine could have presented it, the engine decides;
  in the few cycles between, either.

It checks every response on the four device ports (data, tag, `d_error`, opcode/size/source, and
response and data integrity, which the subsystem regenerates for every response it makes or
changes), the
engine's every read, and after each sweep and at the end compares the whole meta SRAM model and data
memory with its prediction. The memory models check every request's command and data integrity,
and the meta SRAM model that every tag-word write changes exactly one bit, that requests come only
in CHERIoT mode and only to the meta SRAM. `+cms_sb_corrupt=<kind>,<n>` corrupts one prediction so
the run must fail (the `_corrupt_` runs of `cms_regress.list`).

## Commands

In the EDA environment, from this directory (`hw/ip/cheriot_mem_sys/dv/cms`):

```bash
make build                                  # Xcelium build (COV=1 by default; MAP=earlgrey for the Earl Grey 2 map)
make regress                                # every run in cms_regress.list (KEEP=1 reuses the build)
./cms_regress.sh -m 2                       # the same, every run also checked by the C model
make run TEST=cms_tbre_store_race SEED=3
make lint                                   # Verilator lint only, no simulation
make filelist                               # regenerate cms_xlm.f after changing cms_dv.f or cms_rtl.f
./cms_mutation.sh                           # plant one RTL bug per run; its tests must fail (below)
```

Here, by hand: `make help`; `make build [COV=1] [MAP=earlgrey]`,
`make run TEST=<t> [SEED=n] [GUI=1] [PLUSARGS="+num_ops=20000"]`, `make regress [FILTER=tbre]`.
Logs and verdicts: `xlm_out/results/<name>.<seed>.{log,result}`; the last line of a log is
`CMS_RESULT test=... status=PASS|FAIL errors=N ...`, every failure a `CMS_ERROR` line.

Useful plusargs: `+num_ops=<n>` (cms_random), `+timeout_cycles=<n>`, `+enable_ibex_fcov=1` (set by `-c`),
`+cms_mutate=<name>` (one RTL bug, see "Mutation check").

## Mutation check

The `_corrupt_` runs show the checker can fail; they say nothing about whether the stimulus reaches
a real bug. `+cms_mutate=<name>` plants one: `tb/cms_tb.sv` forces one internal signal of the
unmodified RTL to what a plausible design error would drive, for the whole run (runtime-selected,
so one build serves every mutation; Xcelium only). The run prints `MUTATION <name> active` the
first time the forced value differs from the one the RTL would drive (recomputed in the bench from
the same inputs), not when the force is applied, so a mutation the stimulus never exercises cannot
pass as caught.

`./cms_mutation.sh [-k] [-m <name>]` builds into
`xlm_mutation_out/` without coverage, runs every (mutation, test) pair that
`hw/ip/cheriot_mem_sys/dv/cms/scripts/cms_mutation_check.py` lists at seed 1, keeps
`xlm_mutation_out/results/<mutation>/<name>.1.{log,result}`, and grades them; the regression's
`xlm_out/` is never touched. Per pair: CAUGHT (the bench's verdict is FAIL; the first `CMS_ERROR`
is shown), MISSED (it passed; an RTL assertion that fired is shown but does not count), NO LOG,
CRASHED (no `CMS_RESULT`), INACTIVE (no `MUTATION <name> active`). It exits non-zero on MISSED, NO
LOG, INACTIVE or a mutation none of its tests caught. Run the unmutated regression as the control.

| Mutation | Forces (`hw/ip/cheriot_mem_sys/rtl/`) | Bug | Must fail |
|---|---|---|---|
| `partial_write_keeps_tag` | `cheriot_tag_filter.sv:215` `require_lookup` | A sub-word or PutPartialData store leaves the tag | `cms_tag_clear_subword`, `cms_rmw_same_word`, `cms_random` |
| `datastore_sets_tag` | `cheriot_tag_filter.sv:328` `tag_m_o` | A full-word data store writes tag 1 | `cms_smoke`, `cms_tag_clear_subword`, `cms_tag_store_load`, `cms_random` |
| `capstore_drops_tag` | `cheriot_tag_filter.sv:328` `tag_m_o` | A capability store's upper word writes tag 0 | `cms_smoke`, `cms_tag_store_load`, `cms_cap_load_hint`, `cms_tbre_sweep` |
| `trvk_wrong_bit` | `cheriot_trvk_core.sv:275` `revbm_bit_select` | The engine's bitmap lookup reads the next granule's bit | `cms_tbre_base_decode`, `cms_tbre_sweep` |
| `tbre_skip_last` | `cheriot_mem_sys.sv:548` `tbre_num_words` | A sweep stops one capability early | `cms_tbre_sweep`, `cms_tbre_base_decode`, `cms_tbre_epoch` |
| `tbre_no_inval` | `cheriot_tbre_mover.sv:329` `write_a_valid` | The engine never clears a revoked capability's tag | `cms_tbre_sweep`, `cms_tbre_base_decode`, `cms_rmw_core_tbre_same_word`, `cms_tbre_snoop_window` |
| `tbre_epoch_stuck` | `cheriot_mem_sys.sv:603` `tbre_epoch_en` | TBRE_EPOCH never counts a sweep | `cms_tbre_epoch`, `cms_tbre_csr`, `cms_random` |
| `tbre_done_early` | `cheriot_tbre_mover.sv:504` `busy_o` | busy, tbre_done and the interrupt drop at the last read, before the last clear | `cms_tbre_sweep`, `cms_tbre_intr` |
| `snoop_off` | `cheriot_mem_sys.sv:430` `tbre_snoop_valid` | Core writes are not watched: a clear overwrites a racing store's tag | `cms_tbre_store_race`, `cms_tbre_snoop_window` |
| `meta_intg_unreported` | `cheriot_rmw_filter.sv:359,371` `rsp_intg_error_o`, `data_intg_error_o` | Meta SRAM integrity errors raise no fatal_fault | `cms_err_meta_intg` |
| `data_err_dropped` | `cheriot_tag_filter.sv:392` `tl_d_o` | The data path's d_error does not reach the core | `cms_err_data_path`, `cms_nvm_cap_store`, `cms_tag_store_load` |
| `alert_dropped` | `cheriot_mem_sys.sv:708` `alert_req_i` | No fatal error raises fatal_fault | `cms_err_tag_path`, `cms_err_csr_intg`, `cms_err_tbre_read`, `cms_nvm_cap_store` |
| `mode_loose_mubi` | `cheriot_access_check.sv:61-72` `allow_forward` (corerevbm) | The core's bitmap window takes any `cheriot_ena_i` but MuBi4False as CHERIoT mode | `cms_mode_gating` |
| `tbre_read_err_fatal` | `cheriot_tbre_mover.sv:549` `err_o` | An error response to a read of the swept memory (a read-protected page) counts as a mover fault and raises fatal_fault | `cms_err_tbre_read`, `cms_tbre_epoch`, `cms_tbre_intr` |
| `wtrc_presented_unflagged` | `cheriot_wtrc.sv:366` `req_before_tag_wr` | A request presented between W1 and its tag write is not flagged | `cms_nvm_cap_store` |
| `alert_test_unlocked` | `cheriot_mem_sys_regs_reg_top.sv:239` `alert_test_gated_we` | ALERT_TEST.regwen is ignored: alert testing cannot be disabled | `cms_alert_test` |

`tbre_read_err_fatal` and `wtrc_presented_unflagged` are the RTL before PR #31515 was merged.
`tbre_done_early` is timing-dependent: it shows only when a busy poll or the interrupt falls
between a sweep's last read and its last clear, with a revoked last capability. Not mutated: the
core's own load barrier (bit selection and the revoked decision are in Ibex's `ibex_trvk`; the
subsystem only serves the bitmap word through `corerevbm_tl`). Corrupting that word's address or
data would mean forcing a whole TL-UL struct rebuilt from a `tlul_socket_1n`'s state (`force`
cannot reach one field of a struct variable); `mode_loose_mubi` mutates that window's gate instead.

## Tests from the Earl Grey 2 specification

`tb/cms_tests_eg2.svh` holds four tests whose obligations come from
`eg2_top_verification/eg2_toplevel_verification_spec.md` (block rows of REQ_CMS) and RFC CHERIoT
Memory Subsystem, not from the design documents above. Each has a `_corrupt_` run
(`+cms_sb_corrupt=mubi|lockstep|latency|trvk,1`).

| Test | Row | Needs from the RTL | With the pinned RTL |
|---|---|---|---|
| `cms_tag_sideband_mubi` | REQ_CMS_01 | MuBi4 tag ports: build with `+define+CMS_TAG_MUBI` (`CMS_TAG_D2H_PATH` names the response signal to corrupt) | fails: one-bit sideband, TODO(#31066) |
| `cms_lockstep_glitch` | REQ_CMS_02 | a second copy: `+define+CMS_LOCKSTEP`, `CMS_LS_SHADOW` (and `CMS_LS_MAIN`, default `u_dut`) its root | fails: one copy, no fatal_fault on an upset, TODO(#30572) |
| `cms_latency` | REQ_CMS_07 | nothing | runs |
| `cms_trvk_fault` | REQ_CMS_02 (REQ_CIB_03's rule) | nothing; `+cms_tf_copy=shadow` needs `+define+CMS_LOCKSTEP` | fails: nothing checks the TBRE's load filter, one copy (TODO(#30572)) |
| `cms_trvk_backpressure` | REQ_CMS_07, REQ_CMS_08 (EG2 revocation corner case) | nothing; Xcelium only (`_corrupt_backpressure` run) | not run yet |

`cms_lockstep_glitch` and `cms_trvk_fault` mask the scoreboard (`CMS_MASKED` lines) and kill assertions from each
upset or injection to the reset after it, then clear the bench memories and the model. `cms_trvk_fault`
counts its effective injections (a run with none fails) and runs one site per regression entry
(`+cms_tf_site=verdict|tag_out|bit_select|req_required|out_of_range|sealing|align`).

## Expected results that are not bugs in the bench

- `cms_tbre_snoop_window` logs, without judging beyond the document, how many revoked capabilities
  in the NVM kept their tag through a sweep because a capability store to them failed: the document
  watches every core write, and a failed capability store to the NVM writes nothing.
- `cms_err_meta_intg` logs, without judging, whether a corrupted response to the software bitmap
  window raises `fatal_fault` (the error table does not say).

## Not covered here

- No real core, interconnect or SRAM controller: the RTOS SoC covers integration
  (`cheriot-rtos_testplan.hjson`).
- A malformed (wrong opcode/size) bitmap response to the engine is not injected.
- Capability stores to the NVM with bad command or data integrity, a bad NVM response integrity, or
  a W0 followed by anything but its W1 are not driven; nor is an error on the NVM read of W1 alone
  or on the tag write. (A request presented between W1 and its tag write, which Ibex never issues,
  is driven once, in `cms_nvm_cap_store`, with the WTRC's assertions off for it.)
- A malformed (wrong opcode/size) response to one of the engine's capability reads is not injected
  either; the TB memories only produce well-formed ones.
- Writes by other hosts during a sweep (not watched, by the document) need a second host on the
  data memory; the RTOS SoC has the debug module.
- A capability-hinted load of less than a word: from the RTL it would make the RMW filter's access
  checker refuse the lookup and raise `fatal_fault`; Ibex never issues one, so no test drives it.
- Ping requests on the alert, `cheriot_ena_i` changing while a request is in flight, and the
  Earl Grey map are built but not in the regression.
- Formal properties (see the testplan's `formal_properties`).

## Why not OpenTitan's dvsim environment

`hw/ip/cheriot_mem_sys/dv` is a CIP-library UVM environment (cip_lib, tl_agent,
alert_agent, a reggen-generated RAL) driven by `dvsim.py` and FuseSoC. At the pinned commit it
connects only the CSR port and has an empty smoke sequence and scoreboard. Running it here would
need most of OpenTitan's `hw/dv` (cip_lib, tl_agent, alert agent, csr_utils as OpenTitan has them),
`util/reggen` for the RAL, and FuseSoC cores, none of which are vendored, and dvsim's VCS default
replaced by Xcelium. It is the right long-term home: the CSR, alert and TL-UL-access test suites
come for free there, and the testplan format here is already dvsim's. The trade-off taken: this
bench depends only on what this repository already has, runs today, and its scoreboard and tests are
plain SystemVerilog that can be moved into a `cheriot_scoreboard` / virtual sequences when the
environment is brought up with dvsim (the reference model has no UVM dependency for that reason).
