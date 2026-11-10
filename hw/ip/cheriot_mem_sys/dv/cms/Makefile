# Copyright lowRISC contributors.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# CHERIoT memory subsystem block-level bench (cms), on this tree's hw/ip/cheriot_mem_sys: build, run
# and lint by hand, in the EDA environment (xrun, verilator and python3 with hjson on PATH). See
# README.md.
#
#   make build   [COV=0] [MAP=earlgrey]            Xcelium build (elaborate only)
#   make run     TEST=<test> [SEED=n] [NAME=..] [COV=0] [GUI=1] [PLUSARGS=..]
#   make regress [COV=0] [KEEP=1] [SEED_BASE=n] [FILTER=<regex>]   every run of cms_regress.list
#   make lint                                      Verilator lint (warnings fatal; no simulation)
#   make lint-wall                                 The same with -Wall (style; informational)
#   make filelist                                  Regenerate cms_xlm.f (Xcelium's file list)
#   make tests                                     List the tests
#   make clean

COV       ?= 1
GUI       ?= 0
MAP       ?=
TEST      ?= cms_smoke
SEED      ?= 1
NAME      ?=
PLUSARGS  ?=
KEEP      ?= 0
SEED_BASE ?= 1
FILTER    ?= .

REPO_TOP  := $(realpath $(CURDIR)/../../../../..)
LINT_ARGS := --lint-only --timing --top-module cms_tb hw/ip/cheriot_mem_sys/dv/cms/cms_lint.vlt \
             -f hw/ip/cheriot_mem_sys/dv/cms/cms_dv.f

.PHONY: help build run regress lint lint-wall filelist tests clean

help:
	@sed -n '/^#   make/p' $(firstword $(MAKEFILE_LIST)) | sed 's/^#   //'

build:
	./cms_xlm_build.sh $(if $(filter 1,$(COV)),-c) $(if $(filter earlgrey,$(MAP)),-e)

run:
	./cms_xlm_run.sh -t $(TEST) -s $(SEED) $(if $(NAME),-n $(NAME)) $(if $(filter 1,$(COV)),-c) \
	  $(if $(filter 1,$(GUI)),-g) -- $(PLUSARGS)

regress:
	./cms_regress.sh $(if $(filter 1,$(COV)),-c) $(if $(filter 1,$(KEEP)),-k) -b $(SEED_BASE) -f '$(FILTER)'

# Both address maps; then check the committed Xcelium file list still matches the sources.
lint:
	cd $(REPO_TOP) && verilator $(LINT_ARGS)
	cd $(REPO_TOP) && verilator $(LINT_ARGS) +define+CMS_EARLGREY_MAP
	./gen_xlm_filelist.sh $(CURDIR)/.cms_xlm.f.check >/dev/null
	@if cmp -s .cms_xlm.f.check cms_xlm.f; then rm -f .cms_xlm.f.check; echo "lint: clean; cms_xlm.f current"; \
	 else rm -f .cms_xlm.f.check; echo "lint: cms_xlm.f is stale -- run 'make filelist'" >&2; exit 1; fi

lint-wall:
	cd $(REPO_TOP) && verilator $(LINT_ARGS) -Wall -Wno-fatal

filelist:
	./gen_xlm_filelist.sh

tests:
	@grep -o '^    "cms_[a-z_]*"' tb/cms_tests.svh | tr -d ' "'

clean:
	rm -rf xlm_out xlm_mutation_out lint_out cov_work .cms_xlm.f.check
