# C-to-RTL equivalence of the revocation decision (README.md). Run in a shell with Bambu and
# Yosys with yosys-slang (the c2rtl shell; make c2rtl-cms in eg2_top_verification enters it).
#   make equiv      the proof: C model decision = cheriot_trvk_core decision
#   make negative   the same proof on two broken RTL copies; each must fail
#   make all        both
ROOT     := $(abspath ../../../../..)
BUILD    ?= $(ROOT)/scratch/c2rtl/cms-trvk
HLS      := $(BUILD)/hls/trvk_decision_hls.v
YOSYS    ?= yosys
BAMBU    ?= bambu
YOSYS_SLANG ?= $(error YOSYS_SLANG is unset: run in the c2rtl shell)
TRVK_CORE := hw/ip/cheriot_mem_sys/rtl/cheriot_trvk_core.sv
HERE     := $(abspath .)

.PHONY: all equiv negative rules clean
all: equiv negative rules

# Includes the positive proofs and checked counterexamples for each added family.
rules:
	python3 run_rules.py --build $(BUILD)/rules

$(HLS): trvk_decision_hls.c ../cmodel/cms_cap.h
	mkdir -p $(BUILD)/hls
	cd $(BUILD)/hls && $(BAMBU) $(HERE)/trvk_decision_hls.c -I$(HERE)/../cmodel -O3 \
	  --top-fname=trvk_decision_hls --top-rtldesign-name=trvk_decision_hls \
	  > bambu.log 2>&1 || { cat bambu.log; exit 1; }

# $(call run,<name>,<rtl file>,<sed for the bitmap size>)
define run
	mkdir -p $(BUILD)
	sed -e 's|@HLS_VERILOG@|$(HLS)|' -e 's|@TRVK_CORE@|$(2)|' $(3) trvk_decision.ys > $(BUILD)/$(1).ys
	cd $(ROOT) && env -u LD_LIBRARY_PATH $(YOSYS) -m $(YOSYS_SLANG) -s $(BUILD)/$(1).ys > $(BUILD)/$(1).log 2>&1
endef

equiv: $(HLS)
	$(call run,equiv,$(TRVK_CORE),)
	@test "$$(grep -c 'no model found: SUCCESS' $(BUILD)/equiv.log)" -eq 2 || { tail -40 $(BUILD)/equiv.log; exit 1; }
	@echo "Proved: the C model's revocation decision equals cheriot_trvk_core's (lookup, word, bit)"

negative: $(HLS)
	mkdir -p $(BUILD)/neg
	sed 's/revbm_out_of_range = revbm_bit_addr >= /revbm_out_of_range = revbm_bit_addr > /' \
	  $(ROOT)/$(TRVK_CORE) > $(BUILD)/neg/cheriot_trvk_core.sv
	@! $(MAKE) --no-print-directory -s _neg NAME=neg_range RTL=$(BUILD)/neg/cheriot_trvk_core.sv SED= || { echo "ERROR: off-by-one range passed"; exit 1; }
	@! $(MAKE) --no-print-directory -s _neg NAME=neg_size RTL=$(TRVK_CORE) SED="-e s/RevBitmapSizeBytes=3072/RevBitmapSizeBytes=4096/" || { echo "ERROR: 4096-byte bitmap passed"; exit 1; }
	@grep -q 'model found: FAIL' $(BUILD)/neg_range.log && grep -q 'model found: FAIL' $(BUILD)/neg_size.log
	@echo "Expected failures: off-by-one range check, 4096-byte bitmap"

_neg:
	$(call run,$(NAME),$(RTL),$(SED))

clean:
	rm -rf $(BUILD)
