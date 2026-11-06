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

SRCS := eg2sim.cc eg2_spike_cpu.cc eg2_ip_models.cc
HDRS := $(wildcard *.h ip/*.h rtos_soc/*.h) $(EG2_ROOT)/sw/device/lib/testing/eg2/eg2_tables.h

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
LIB_SRCS := dpi/eg2_sysmodel.cc eg2_ip_models.cc $(filter %.c,$(SRCS))
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

$(OUT)/eg2_sysmodel_replay: dpi/eg2_sysmodel_replay.cc dpi/eg2_sysmodel.h $(OUT)/libeg2sysmodel.so
	$(CXX) -std=c++17 -O2 -g -Wall -I$(CURDIR)/dpi $< -o $@ \
	  -L$(abspath $(OUT)) -Wl,-rpath,$(abspath $(OUT)) -leg2sysmodel
