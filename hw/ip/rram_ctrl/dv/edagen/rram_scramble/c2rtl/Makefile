# C-to-RTL equivalence of rram_ctrl's scrambling mask, the proposal's AES c2rtl flow
# (crypto-engines/aes/dv/c2rtl) applied to rram_scramble: Bambu compiles rram_gf_mask_hls.c (the
# reference model's GF(2^128) mask in C, checked against the model by `make differential`), and a
# Yosys SAT miter compares it with the RTL for every input. Tools from eg2-git-proposal's c2rtl
# shell: nix develop <eg2-git-proposal>#c2rtl --command make -C <this directory> all
REPO     ?= $(abspath ../../../../../../..)
BUILD    ?= $(REPO)/../scratch/c2rtl/rram_gf_mask
SEQ      ?= 160
SKIP     ?= 159
YOSYS    ?= yosys
YOSYS_SLANG ?= $(error YOSYS_SLANG is unset: run in eg2-git-proposal's c2rtl shell)
BAMBU    ?= bambu
CXX      ?= g++
CC       ?= gcc
RTL      := $(shell sed -n 's|^\$${REPO}|$(REPO)|p' ../rram_scramble_rtl.f)
HLS      := $(BUILD)/hls/rram_gf_mask_hls.v

.PHONY: all differential differential-negative hls equiv equiv-negative
all: differential differential-negative equiv equiv-negative

differential:
	mkdir -p $(BUILD)
	$(CC) -O2 -c rram_gf_mask_hls.c -o $(BUILD)/gfm.o
	$(CXX) -std=c++17 -O2 -Wall -Wextra rram_gf_mask_test.cc $(BUILD)/gfm.o -o $(BUILD)/gfm_test
	$(BUILD)/gfm_test
differential-negative: differential
	! RRAM_GF_TEST_BREAK=1 $(BUILD)/gfm_test

$(HLS): rram_gf_mask_hls.c
	mkdir -p $(BUILD)/hls
	cd $(BUILD)/hls && $(BAMBU) $(abspath rram_gf_mask_hls.c) -O3 --top-fname=rram_gf_mask_hls \
	  --top-rtldesign-name=rram_gf_mask_hls > bambu.log 2>&1 || { cat bambu.log; exit 1; }
	@test -f $@ || { echo "ERROR: Bambu did not generate $@"; exit 1; }
hls: $(HLS)

# $(1): RTL file list (one may be replaced), $(2): log
define run_equiv
	sed -e 's|@RTL@|$(1)|' -e 's|@INC@|$(REPO)/hw/ip/prim/rtl|' -e 's|@HLS@|$(HLS)|' \
	  -e 's|@MITER@|$(abspath rram_gf_mask_equiv.v)|' -e 's|@SEQ@|$(SEQ)|' -e 's|@SKIP@|$(SKIP)|' \
	  equiv.ys > $(BUILD)/equiv.ys
	cd $(BUILD)/hls && env -u LD_LIBRARY_PATH $(YOSYS) -m $(YOSYS_SLANG) -s $(BUILD)/equiv.ys > $(2) 2>&1  # in hls/: Bambu's $$readmemb files are relative
endef

equiv: $(HLS)
	$(call run_equiv,$(RTL),$(BUILD)/equiv.log) || { tail -40 $(BUILD)/equiv.log; exit 1; }
	@test "$$(grep -c 'SUCCESS' $(BUILD)/equiv.log)" -eq 2 || { tail -40 $(BUILD)/equiv.log; exit 1; }
	@echo "Formal C-to-RTL equivalence proved: rram_ctrl scrambling mask (bound $(SEQ) cycles)"

# Must fail: the RTL with one reduction-polynomial bit changed (x^7 + x^2 + x + 1 -> x^7 + x^2 + 1).
equiv-negative: $(HLS)
	mkdir -p $(BUILD)/negative
	sed 's/DataWidth.(1.b1) << 1 |/DataWidth'"'"'(1'"'"'b0) << 1 |/' $(REPO)/hw/ip/rram_ctrl/rtl/rram_ctrl_pkg.sv \
	  > $(BUILD)/negative/rram_ctrl_pkg.sv
	@! cmp -s $(BUILD)/negative/rram_ctrl_pkg.sv $(REPO)/hw/ip/rram_ctrl/rtl/rram_ctrl_pkg.sv || { echo "ERROR: the mutation did not apply"; exit 1; }
	@if $(call run_equiv,$(subst $(REPO)/hw/ip/rram_ctrl/rtl/rram_ctrl_pkg.sv,$(BUILD)/negative/rram_ctrl_pkg.sv,$(RTL)),$(BUILD)/negative/equiv.log); then \
	  echo "ERROR: the mutated RTL passed the equivalence"; exit 1; \
	else grep -q 'model found: FAIL\|proof did fail' $(BUILD)/negative/equiv.log && echo "Expected C-to-RTL equivalence failure (mutated polynomial)"; fi
