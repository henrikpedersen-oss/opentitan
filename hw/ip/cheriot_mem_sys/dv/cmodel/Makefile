# The CHERIoT memory subsystem's reference model (README.md).
#   make test   unit test of cms_core.h (every scenario, and a wrong response each must flag)
#   make lib    libcms_cmodel.so for DPI (cms_dpi.cc), with cms_cmodel_pkg.sv
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -O2
OUT      ?= build

.PHONY: all test lib clean
all: test lib

$(OUT)/cms_core_test: cms_core_test.cc cms_core.h cms_cap.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ cms_core_test.cc

test: $(OUT)/cms_core_test
	$(OUT)/cms_core_test

$(OUT)/libcms_cmodel.so: cms_dpi.cc cms_core.h cms_cap.h
	mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -fPIC -shared -o $@ cms_dpi.cc

lib: $(OUT)/libcms_cmodel.so

clean:
	rm -rf $(OUT)
