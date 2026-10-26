// Copyright lowRISC contributors (OpenTitan project).
// Licensed under the Apache License, Version 2.0, see LICENSE for details.
// SPDX-License-Identifier: Apache-2.0
//
// The EG2 SystemC model as a library for the chip bench (libeg2sysmodel.so; milestones M5 and M6
// of doc/system_model/eg2_system_model_plan.md in eg2_top_verification). It is the platform eg2sim
// builds (every window of the top: the IP models and the memories) without eg2sim's
// instruction-set model: the caller is the bus initiator. In the chip bench that is the ISS wrapper
// (eg2_iss_wrapper.sv, build mode en_eg2_model): each data access of each retired instruction
// becomes a read or write here, so a load is a prediction to compare with the chip's, and a store
// configures the devices as it configures the chip's.
//
// A C API, imported by eg2_sysmodel.sv as DPI-C functions (every type is a DPI type):
//
//   eg2_sysmodel_create(rom, test, otp, log_dir)  build the platform and load the images the bench
//                                     loads into the chip: the ROM and test ELFs (each segment at
//                                     its load address) and the OTP image (a .24.vmem, into
//                                     otp_ctrl's SW_CFG_WINDOW); test and otp may be "". One
//                                     model per process (SystemC elaborates once); NULL on error.
//   eg2_sysmodel_reset(h, now_ps)     the chip reset at now_ps: every model does what its IP does on
//                                     that reset. Returns the cause the model predicted (its pending
//                                     reset request: 0 power-on, 1 low-power exit, 2 software,
//                                     3 hardware request, 4 escalation, 5 NDM), or -1 if it predicted
//                                     none (it then takes a power-on reset).
//   eg2_sysmodel_step(h, now_ps)      time advances to now_ps (the chip's time, monotonic): every
//                                     device event due by then runs, in time order.
//   eg2_sysmodel_read(h, addr, size, &data, &err)
//                                     a load of 1, 2 or 4 bytes within one 32-bit word: the model's
//                                     data (little-endian, in the low bytes) and error response.
//                                     Returns 1 if the value is the model's prediction (a memory
//                                     byte the model knows; a register no hardware updates), 0 if it
//                                     depends on timing the model does not have (a register hardware
//                                     updates, a memory window inside a register block) or on
//                                     memory the model never saw written: such a value is an input
//                                     event (spec: what the model cannot know is an input event,
//                                     never copied from the chip as a prediction). The error is
//                                     always a prediction (the address map and the registers).
//   eg2_sysmodel_load_event(h, addr, size, data)
//                                     the chip's value of a load read() returned 0 for: memory
//                                     takes it (and knows those bytes from then on); a register
//                                     keeps its own value.
//   eg2_sysmodel_write(h, addr, size, data)
//                                     a store of 1, 2 or 4 bytes within one word; returns 1 if the
//                                     model answers an error.
//   eg2_sysmodel_forget(h, addr, size)  memory bytes whose value the model cannot know from then on
//                                     (a capability's metadata word, which a CHERIoT store writes
//                                     in a format the bench does not give).
//   eg2_sysmodel_backdoor_write8(h, addr, byte)
//                                     a bench backdoor write into memory (the vseq's
//                                     sw_symbol_backdoor_overwrite): no bus access, no error.
//   eg2_sysmodel_input(h, key, value)  an agent's item as the chip received it: uart<N>.rx (a byte
//                                     from the peer), gpio.in (the level outside drives), or a chip
//                                     clock's measured frequency in Hz from now on: clk.main,
//                                     clk.io_div4, clk.aon. Returns 1 for a key no model takes.
//   eg2_sysmodel_wfi(h)               the core retired WFI: pwrmgr decides whether this is a
//                                     low-power entry (its wake-up then predicts the reset).
//   eg2_sysmodel_fault(h, name, now_ps)
//                                     the bench injected a fault into the chip at now_ps (an input
//                                     event, as eg2sim --fault): "lockstep" (the core's lockstep
//                                     comparator mismatch: rv_core_ibex's fatal_hw_err, the core
//                                     halted, then whatever the alert handler's configuration
//                                     escalates to) or "cheriot_ena_low" (the core's CHERIoT enable
//                                     forced to MuBi4False from now on; the bench's ISS wrapper must
//                                     then keep the core's model in RISC-V mode). Returns 1 for a
//                                     name the model does not know. The model prints
//                                     "eg2_sysmodel: fault <name> applied at ..." when the fault
//                                     changes what the chip does.
//   eg2_sysmodel_core_halted(h)       "" while the core may fetch; else why the model has its
//                                     fetch disabled (rv_core_ibex's fatal_hw_err, lc_ctrl's
//                                     lc_cpu_en in the escalation state) until the next reset: the
//                                     core retires nothing from then on.
//   eg2_sysmodel_pending(h, instance) how many output items of an instance ("uart0") wait.
//   eg2_sysmodel_pull(h, instance)    the next output item of an instance, in the format of the
//                                     chip bench's <instance>.chip.log ("%02h %c" for a UART); ""
//                                     if none. Every item also goes to <log_dir>/<instance>.model.log.
//   eg2_sysmodel_verdict(h)           "" while the software has not ended the test; then "passed:
//                                     ..." or "failed: ..." (the DV window's status word).
//   eg2_sysmodel_create_error()       why the last create returned null ("" otherwise).
//   eg2_sysmodel_stop_reason(h)       "" while the model follows; else why it stopped (a feature
//                                     not modelled yet). From then on read() returns 0 (nothing is
//                                     a prediction). Software polling a register no model updates,
//                                     which stops eg2sim, does not stop the library: the chip's
//                                     value of such a register is an event.
//   eg2_sysmodel_release(h)           flush the logs (the UARTs' partial lines).

#ifndef EG2_MODEL_DPI_EG2_SYSMODEL_H_
#define EG2_MODEL_DPI_EG2_SYSMODEL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void *eg2_sysmodel_create(const char *rom_elf, const char *test_elf, const char *otp_vmem,
                          const char *log_dir);
int eg2_sysmodel_reset(void *h, uint64_t now_ps);
void eg2_sysmodel_step(void *h, uint64_t now_ps);
int eg2_sysmodel_read(void *h, uint32_t addr, uint32_t size, uint32_t *data, int *err);
void eg2_sysmodel_load_event(void *h, uint32_t addr, uint32_t size, uint32_t data);
int eg2_sysmodel_write(void *h, uint32_t addr, uint32_t size, uint32_t data);
void eg2_sysmodel_forget(void *h, uint32_t addr, uint32_t size);
void eg2_sysmodel_backdoor_write8(void *h, uint32_t addr, uint8_t data);
int eg2_sysmodel_input(void *h, const char *key, uint32_t value);
void eg2_sysmodel_wfi(void *h);
int eg2_sysmodel_fault(void *h, const char *name, uint64_t now_ps);
const char *eg2_sysmodel_core_halted(void *h);
int eg2_sysmodel_pending(void *h, const char *instance);
const char *eg2_sysmodel_pull(void *h, const char *instance);
const char *eg2_sysmodel_verdict(void *h);
const char *eg2_sysmodel_stop_reason(void *h);
const char *eg2_sysmodel_create_error(void);
void eg2_sysmodel_release(void *h);

#ifdef __cplusplus
}
#endif

#endif  // EG2_MODEL_DPI_EG2_SYSMODEL_H_
