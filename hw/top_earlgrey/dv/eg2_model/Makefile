# Copyright lowRISC contributors (OpenTitan project).
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# eg2sim, the EG2 SystemC model run on its own. EG2_SYSTEMC and EG2_SPIKE point at SystemC and Spike
# built with the same toolchain: eg2_top_verification's `make eg2sim` sets them (its flake's
# devShell `model`).

EG2_ROOT := $(abspath $(CURDIR)/../../../..)
OUT      ?= build

CXXFLAGS := -std=c++17 -O2 -g -Wall \
            -I$(EG2_SYSTEMC)/include -I$(EG2_SPIKE)/include -I$(EG2_SPIKE)/include/softfloat \
            -I$(EG2_ROOT)
LDLIBS   := -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc \
            -L$(EG2_SPIKE)/lib -Wl,-rpath,$(EG2_SPIKE)/lib \
            -lriscv -ldisasm -lfesvr -lfdt -lsoftfloat -ldl -lpthread

# IP implementations shared by the standalone executable and DPI library.
IP_SRCS := ip/eg2_otp_ctrl.cc ip/eg2_otbn_accelerators.cc ip/eg2_otbn_execute.cc
SRCS := eg2sim.cc eg2_image.cc eg2_stimulus.cc eg2_spike_cpu.cc eg2_spike_debug.cc \
        eg2_ip_models.cc $(IP_SRCS)
HDRS := $(wildcard *.h ip/*.h rtos_soc/*.h) $(EG2_ROOT)/sw/device/lib/testing/eg2/eg2_tables.h
# Shared reference model used by ip/eg2_cheriot_mem_sys.h.
HDRS += $(EG2_ROOT)/hw/ip/cheriot_mem_sys/dv/cmodel/cms_core.h $(EG2_ROOT)/hw/ip/cheriot_mem_sys/dv/cmodel/cms_cap.h

# Crypto group (ip/eg2_crypto.h): the block DVs' reference code, compiled as C++.
SRCS += $(EG2_ROOT)/hw/ip/aes/model/aes.c \
        $(addprefix $(EG2_ROOT)/hw/ip/hmac/dv/cryptoc_dpi/,sha256.c sha384.c sha512.c)

# usbdev's host (ip/eg2_periph_usb.h, --stim usbdev.host=1): usb-host/, the model's own copy of
# OpenTitan's RTL USB host hw/dv/dpi/usbdpi (EG2 fork at cc3c79b207), changed here: its host step
# split from the bit-level driver (usbdpi_frame_tick, usbdpi_host_step, usbdpi_device_eop). Compiled
# as C++; usb-host/svdpi.h stands in for the simulator's header.
USB_HOST := usb-host
SRCS     += $(addprefix $(USB_HOST)/,usbdpi.c usb_transfer.c usbdpi_test.c usbdpi_stream.c usb_monitor.c usb_utils.c \
                              usb_crc.c)
HDRS     += $(wildcard $(USB_HOST)/*.h)
CXXFLAGS += -Iusb-host

# CHERIoT mode: EG2_MPACT points at libeg2_mpact.so, MPACT-CHERIoT behind the C interface of
# $(EG2_MPACT)/include/eg2_mpact.h (eg2_top_verification's flake package mpact-cheriot, in devShell
# `model`). Without it eg2sim stops the run at the CHERIoT lock.
ifneq ($(EG2_MPACT),)
CXXFLAGS += -DEG2_MPACT -I$(EG2_MPACT)/include
LDLIBS   += -L$(EG2_MPACT)/lib -Wl,-rpath,$(EG2_MPACT)/lib -leg2_mpact
SRCS     += eg2_mpact_cpu.cc rtos_soc/rtos_soc.cc
HDRS     += $(EG2_MPACT)/include/eg2_mpact.h
endif

# The tools' nix store paths, as a prerequisite of every binary: a store file is dated 1970, so a
# new SystemC, Spike or MPACT build (a new path) would not make a binary older than its headers,
# and the binary would keep linking the old library (2026-10-31: the MPACT CAndPerm fix was not
# picked up). The file changes only when a path does.
# make with no goal builds eg2sim (eg2_top_verification's make eg2sim), whatever rule comes first.
.DEFAULT_GOAL := $(OUT)/eg2sim
TOOLPATHS := $(OUT)/.toolpaths
_TOOLPATHS_NOW := $(EG2_SYSTEMC) $(EG2_SPIKE) $(EG2_MPACT)
$(TOOLPATHS): FORCE
	@mkdir -p $(OUT)
	@echo "$(_TOOLPATHS_NOW)" | cmp -s - $@ || echo "$(_TOOLPATHS_NOW)" > $@
.PHONY: FORCE
FORCE:

$(OUT)/eg2sim: $(SRCS) $(HDRS) $(TOOLPATHS)
	@test -n "$(EG2_SYSTEMC)" -a -n "$(EG2_SPIKE)" || \
	  { echo "eg2sim: EG2_SYSTEMC and EG2_SPIKE must be set (eg2_top_verification: make eg2sim)"; exit 1; }
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) $(SRCS) -o $@ $(LDLIBS)

# libeg2sysmodel.so: the model as a library for the chip bench (dpi/eg2_sysmodel.h; build mode
# en_eg2_model of chip_sim_cfg.hjson), without eg2sim's Spike: SystemC only. eg2_sysmodel_replay
# drives it from an eg2sim --spike-log trace, as the bench's ISS wrapper does
# (dpi/eg2_sysmodel_replay.cc). eg2_top_verification: make eg2sysmodel.
LIB_SRCS := dpi/eg2_sysmodel.cc eg2_ip_models.cc $(IP_SRCS) $(filter %.c,$(SRCS))
LIB_HDRS := $(HDRS) $(wildcard dpi/*.h)

.PHONY: lib replay
lib: $(OUT)/libeg2sysmodel.so
replay: $(OUT)/eg2_sysmodel_replay

$(OUT)/libeg2sysmodel.so: $(LIB_SRCS) $(LIB_HDRS) $(TOOLPATHS)
	@test -n "$(EG2_SYSTEMC)" || \
	  { echo "libeg2sysmodel: EG2_SYSTEMC must be set (eg2_top_verification: make eg2sysmodel)"; exit 1; }
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I$(CURDIR) -fPIC -shared $(LIB_SRCS) -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread

# nist_cavp_check: the NIST CAVP vectors against the C references (nist/nist_cavp_check.cc).
# The model headers need SystemC (the checker defines its own main). aes.c is C++ here, as in the model (aes.h has no
# extern "C"); CryptoC is C, as in its DV. eg2_top_verification: make nist-cavp-ref.
NIST_SRCS := nist/nist_cavp_check.cc $(EG2_ROOT)/hw/ip/aes/model/aes.c
NIST_C    := $(addprefix $(EG2_ROOT)/hw/ip/hmac/dv/cryptoc_dpi/,sha.c sha256.c sha384.c sha512.c hmac.c util.c)

.PHONY: nist
nist: $(OUT)/nist_cavp_check

$(OUT)/nist_cavp_check: $(NIST_SRCS) $(NIST_C) $(HDRS) $(TOOLPATHS)
	@test -n "$(EG2_SYSTEMC)" || \
	  { echo "nist_cavp_check: EG2_SYSTEMC must be set (eg2_top_verification: make nist-cavp-ref)"; exit 1; }
	@mkdir -p $(OUT)/nist_obj
	cd $(abspath $(OUT))/nist_obj && $(CC) -O2 -g -Wall -c $(NIST_C)
	$(CXX) $(CXXFLAGS) -I$(CURDIR) $(NIST_SRCS) $(abspath $(OUT))/nist_obj/*.o -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread

# dm_test: the debug module reference model's self-test through its DPI functions (no simulator,
# no SystemC); EG2_DM_TEST_BREAK=1 must make it fail.
.PHONY: dm_test
dm_test: $(OUT)/eg2_dm_dpi_test
	$(OUT)/eg2_dm_dpi_test
	! EG2_DM_TEST_BREAK=1 $(OUT)/eg2_dm_dpi_test > /dev/null

$(OUT)/eg2_dm_dpi_test: dpi/eg2_dm_dpi.cc dpi/eg2_dm_dpi_test.cc ip/eg2_dm_core.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror dpi/eg2_dm_dpi.cc dpi/eg2_dm_dpi_test.cc -o $@

# rram_test: the rram_ctrl memory-protection and scrambling models' self-test through their DPI
# functions (no simulator); EG2_RRAM_TEST_BREAK=1 must make it fail.
.PHONY: rram_test
rram_test: $(OUT)/eg2_rram_dpi_test
	$(OUT)/eg2_rram_dpi_test
	! EG2_RRAM_TEST_BREAK=1 $(OUT)/eg2_rram_dpi_test > /dev/null

$(OUT)/eg2_rram_dpi_test: dpi/eg2_rram_dpi.cc dpi/eg2_rram_dpi_test.cc ip/eg2_rram_mp_core.h \
    ip/eg2_rram_scramble_core.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror dpi/eg2_rram_dpi.cc dpi/eg2_rram_dpi_test.cc -o $@

# pwrmgr_test: the power manager reference model's self-test through its DPI functions (no
# simulator, no SystemC); EG2_PWR_TEST_BREAK=1 must make it fail.
.PHONY: pwrmgr_test
pwrmgr_test: $(OUT)/eg2_pwrmgr_dpi_test
	$(OUT)/eg2_pwrmgr_dpi_test
	! EG2_PWR_TEST_BREAK=1 $(OUT)/eg2_pwrmgr_dpi_test > /dev/null

$(OUT)/eg2_pwrmgr_dpi_test: dpi/eg2_pwrmgr_dpi.cc dpi/eg2_pwrmgr_dpi_test.cc ip/eg2_pwrmgr_core.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror dpi/eg2_pwrmgr_dpi.cc dpi/eg2_pwrmgr_dpi_test.cc -o $@

# rstmgr_test: the reset manager reference model's self-test through its DPI functions (no
# simulator, no SystemC); EG2_RST_TEST_BREAK=1 must make it fail.
.PHONY: rstmgr_test
rstmgr_test: $(OUT)/eg2_rstmgr_dpi_test
	$(OUT)/eg2_rstmgr_dpi_test
	! EG2_RST_TEST_BREAK=1 $(OUT)/eg2_rstmgr_dpi_test > /dev/null

$(OUT)/eg2_rstmgr_dpi_test: dpi/eg2_rstmgr_dpi.cc dpi/eg2_rstmgr_dpi_test.cc ip/eg2_rstmgr_core.h
	@mkdir -p $(OUT)
	$(CXX) -std=c++17 -O2 -g -Wall -Wextra -Werror dpi/eg2_rstmgr_dpi.cc dpi/eg2_rstmgr_dpi_test.cc -o $@

$(OUT)/eg2_sysmodel_replay: dpi/eg2_sysmodel_replay.cc dpi/eg2_sysmodel.h $(OUT)/libeg2sysmodel.so
	$(CXX) -std=c++17 -O2 -g -Wall -I$(CURDIR)/dpi $< -o $@ \
	  -L$(abspath $(OUT)) -Wl,-rpath,$(abspath $(OUT)) -leg2sysmodel

# Public entry point for the existing standalone core tests. No RTL simulator
# is needed. Each test also runs its deliberately broken expectation, which
# must exit 1; a crash or a successful negative test is a failure.
CORE_TESTS := $(notdir $(basename $(wildcard dpi/*_test.cc)))
CORE_TEST_FLAGS := -std=c++17 -O1 -g -Wall -Wextra -I$(CURDIR) -I$(EG2_ROOT)

define core_test_rule
$(OUT)/selftest/$(1): dpi/$(1).cc $(wildcard dpi/$(patsubst %_test,%,$(1)).cc) $(HDRS)
	@mkdir -p $$(@D)
	$(CXX) $(CORE_TEST_FLAGS) $$(filter %.cc,$$^) -o $$@
endef
$(foreach test,$(CORE_TESTS),$(eval $(call core_test_rule,$(test))))

$(OUT)/selftest/eg2_rram_lcmgr_dpi_test_fail: dpi/eg2_rram_lcmgr_dpi_test.cc \
    dpi/eg2_rram_lcmgr_dpi.cc $(HDRS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -DEG2_LCMGR_SELFTEST_FAIL $(filter %.cc,$^) -o $@

.PHONY: core_tests model_test sysmodel_test replay_test check
core_tests: $(addprefix $(OUT)/selftest/,$(CORE_TESTS)) \
    $(OUT)/selftest/eg2_rram_lcmgr_dpi_test_fail
	bash tests/run_core_tests.sh $(abspath $(OUT)/selftest)

$(OUT)/eg2_model_test: tests/eg2_model_test.cc $(IP_SRCS) $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $(filter %.cc,$^) -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread

$(OUT)/eg2_sysmodel_test: tests/eg2_sysmodel_test.cc tests/rom_fixture.h dpi/eg2_sysmodel.h \
    eg2_model_regs.h $(OUT)/libeg2sysmodel.so
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) $< -o $@ \
	  -L$(abspath $(OUT)) -Wl,-rpath,$(abspath $(OUT)) -leg2sysmodel

model_test: $(OUT)/eg2_model_test
	@mkdir -p $(OUT)/model-test-logs
	cd $(OUT)/model-test-logs && $(abspath $(OUT))/eg2_model_test

sysmodel_test: $(OUT)/eg2_sysmodel_test
	@mkdir -p $(OUT)/sysmodel-test-logs
	cd $(OUT)/sysmodel-test-logs && $(abspath $(OUT))/eg2_sysmodel_test

replay_test: $(OUT)/eg2_sysmodel_replay $(OUT)/eg2_rom_test
	bash tests/run_replay_tests.sh $(abspath $(OUT))/eg2_sysmodel_replay $(abspath $(OUT))/replay-test-logs $(abspath $(OUT))/eg2_rom_test

# Parser and cryptographic known-answer checks need only a C++17 compiler.
.PHONY: utility_tests
utility_tests: $(OUT)/eg2_elf_test $(OUT)/eg2_otp_crypto_test
	$(OUT)/eg2_elf_test
	$(OUT)/eg2_otp_crypto_test

$(OUT)/eg2_elf_test: tests/eg2_elf_test.cc eg2_elf.h
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) $< -o $@

$(OUT)/eg2_otp_crypto_test: tests/eg2_otp_crypto_test.cc ip/eg2_otp_crypto.h
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) $< -o $@

$(OUT)/eg2_transport_test: tests/eg2_transport_test.cc $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $< -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread

.PHONY: transport_test
transport_test: $(OUT)/eg2_transport_test
	$(OUT)/eg2_transport_test

check: core_tests model_test sysmodel_test replay_test utility_tests transport_test

$(OUT)/eg2_rom_test: tests/eg2_rom_test.cc tests/rom_fixture.h $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $< -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread
.PHONY: rom_test
rom_test: $(OUT)/eg2_rom_test
	@mkdir -p $(OUT)/rom-test-logs
	cd $(OUT)/rom-test-logs && $(abspath $(OUT))/eg2_rom_test
check: rom_test

$(OUT)/eg2_shadow_test: tests/eg2_shadow_test.cc $(IP_SRCS) $(EG2_ROOT)/hw/ip/aes/model/aes.c $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $(filter %.cc %.c,$^) -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread
.PHONY: shadow_test
shadow_test: $(OUT)/eg2_shadow_test
	@mkdir -p $(OUT)/shadow-test-logs
	cd $(OUT)/shadow-test-logs && $(abspath $(OUT))/eg2_shadow_test
check: shadow_test

$(OUT)/eg2_keymgr_test: tests/eg2_keymgr_test.cc $(IP_SRCS) $(EG2_ROOT)/hw/ip/aes/model/aes.c $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $(filter %.cc %.c,$^) -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread
.PHONY: keymgr_test
keymgr_test: $(OUT)/eg2_keymgr_test
	@mkdir -p $(OUT)/keymgr-test-logs
	cd $(OUT)/keymgr-test-logs && $(abspath $(OUT))/eg2_keymgr_test
check: keymgr_test

$(OUT)/eg2_otp_adapter_test: tests/eg2_otp_adapter_test.cc $(IP_SRCS) $(EG2_ROOT)/hw/ip/aes/model/aes.c $(HDRS) $(TOOLPATHS)
	@mkdir -p $(@D)
	$(CXX) $(CORE_TEST_FLAGS) -I$(EG2_SYSTEMC)/include $(filter %.cc %.c,$^) -o $@ \
	  -L$(EG2_SYSTEMC)/lib -Wl,-rpath,$(EG2_SYSTEMC)/lib -lsystemc -lpthread
.PHONY: otp_adapter_test
otp_adapter_test: $(OUT)/eg2_otp_adapter_test
	@mkdir -p $(OUT)/otp_adapter-test-logs
	cd $(OUT)/otp_adapter-test-logs && $(abspath $(OUT))/eg2_otp_adapter_test
check: otp_adapter_test
