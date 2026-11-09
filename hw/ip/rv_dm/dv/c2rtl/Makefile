# C = RTL proof of rv_dm's Access Register abstract commands (see README.md).
# Run in the c2rtl shell of eg2-git-proposal: nix develop <eg2-git-proposal>#c2rtl -c make equiv

ROOT := $(abspath ../../../../..)
HERE := $(abspath .)
BUILD ?= $(ROOT)/build/c2rtl/rv_dm_ar
DBG := $(ROOT)/hw/vendor/pulp_riscv_dbg
PRIM_RTL := $(ROOT)/hw/ip/prim/rtl
DM_MEM ?= $(DBG)/src/dm_mem.sv
HLS_BUILD := $(BUILD)/hls
HLS_VERILOG := $(HLS_BUILD)/dm_ar_semantic_hls.v

YOSYS ?= yosys
YOSYS_SLANG ?= $(error YOSYS_SLANG is unset: run in the c2rtl shell, nix develop .#c2rtl)
BAMBU ?= bambu
CC ?= gcc

.PHONY: all hls equiv equiv-negative clean
all: equiv equiv-negative

hls: $(HLS_VERILOG)

$(HLS_VERILOG): dm_ar_semantic.c
	mkdir -p $(HLS_BUILD)
	cd $(HLS_BUILD) && cp $(HERE)/dm_ar_semantic.c dm_ar_semantic_hls.c && \
	  sed -i 's/unsigned int dm_ar_semantic(/unsigned int dm_ar_semantic_hls(/' dm_ar_semantic_hls.c && \
	  $(BAMBU) dm_ar_semantic_hls.c -O3 \
	  --top-fname=dm_ar_semantic_hls --top-rtldesign-name=dm_ar_semantic_hls \
	  > bambu.log 2>&1 || { cat bambu.log; exit 1; }
	@test -f $@ || { echo "ERROR: Bambu did not generate $@"; exit 1; }

$(BUILD)/equiv.ys: equiv.ys $(HLS_VERILOG) dm_ar_check.sv dm_ar_equiv.v $(DM_MEM)
	mkdir -p $(BUILD)
	sed -e 's|@PRIM_RTL@|$(PRIM_RTL)|g' -e 's|@DBG@|$(DBG)|g' -e 's|@DM_MEM@|$(abspath $(DM_MEM))|' \
	  -e 's|@HERE@|$(HERE)|g' -e 's|@HLS_VERILOG@|$(HLS_VERILOG)|' equiv.ys > $@

# Proofs 1 and 2 must pass; proofs 3-4 are reported (a failure there is a finding).
equiv: $(BUILD)/equiv.ys
	cd $(BUILD) && env -u LD_LIBRARY_PATH -u LD_PRELOAD $(YOSYS) -m $(YOSYS_SLANG) -s equiv.ys > equiv.log 2>&1 || \
	  { tail -60 equiv.log; exit 1; }
	@grep -E "SAT proof finished|SUCCESS|FAIL" $(BUILD)/equiv.log | grep -v "^ *\\\\" || true
	@echo "log: $(BUILD)/equiv.log"

# A broken dm_mem (reads stored as words, whatever aarsize) must fail proof 1.
equiv-negative: $(HLS_VERILOG)
	mkdir -p $(BUILD)/negative
	sed 's/= dm::store(ac_ar.aarsize, 5.d8,/= dm::store(3'"'"'d2, 5'"'"'d8,/' $(DM_MEM) > $(BUILD)/negative/dm_mem.sv
	@! cmp -s $(DM_MEM) $(BUILD)/negative/dm_mem.sv || { echo "ERROR: negative edit did not apply"; exit 1; }
	sed -e 's|@PRIM_RTL@|$(PRIM_RTL)|g' -e 's|@DBG@|$(DBG)|g' -e 's|@DM_MEM@|$(BUILD)/negative/dm_mem.sv|' \
	  -e 's|@HERE@|$(HERE)|g' -e 's|@HLS_VERILOG@|$(HLS_VERILOG)|' equiv.ys | sed '/^# 2\./,$$d' \
	  > $(BUILD)/negative/equiv.ys
	cd $(BUILD)/negative && if env -u LD_LIBRARY_PATH -u LD_PRELOAD $(YOSYS) -m $(YOSYS_SLANG) -s equiv.ys \
	  > equiv.log 2>&1; then echo "ERROR: broken dm_mem passed the proof"; exit 1; fi
	@grep -q "model found: FAIL" $(BUILD)/negative/equiv.log || \
	  { tail -40 $(BUILD)/negative/equiv.log; echo "ERROR: negative did not reach a failed proof"; exit 1; }
	@echo "negative: broken dm_mem fails the proof as it should ($(BUILD)/negative/equiv.log)"

clean:
	rm -rf $(BUILD)
