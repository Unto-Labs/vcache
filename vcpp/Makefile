# vcpp -- a preprocessor for compilation caching.
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Deliberately not wired into the top-level Makefile. vcpp is a separate GPLv3
# program that vcache executes, not a library it links, and building it means
# downloading GCC. Build it here, on purpose:
#
#   ./fetch-libcpp.sh     download and build libcpp (once, ~107 MB)
#   make                  build bin/vcpp
#   make check            compare against the installed gcc -E

HERE    := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
BUILD   := $(HERE)build

# Written by fetch-libcpp.sh, which decides the version. Do not set it here:
# the libcpp release has to match the compiler vcache is caching for, and
# guessing it in two places is how they drift apart.
-include $(BUILD)/libcpp-version.mk
LIBCPP_VERSION ?= 13.3.0
LIBCPP_MAJOR   ?= 13
SRC     := $(HERE)gcc-$(LIBCPP_VERSION)
BINDIR  := $(HERE)bin

CXX      ?= g++
CXXFLAGS ?= -std=c++11 -O2 -g -Wall
INCLUDES := -DVCPP_LIBCPP_MAJOR=$(LIBCPP_MAJOR) -I$(BUILD)/libcpp -I$(SRC)/libcpp -I$(SRC)/libcpp/include -I$(SRC)/include
LIBS     := $(BUILD)/libcpp/libcpp.a $(BUILD)/libiberty/libiberty.a

.PHONY: all check clean distclean

all: $(BINDIR)/vcpp

$(BINDIR)/vcpp: $(HERE)src/vcpp.cc $(BUILD)/libcpp/libcpp.a
	@mkdir -p $(BINDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $< $(LIBS)

# libcpp is fetched and built by the script, not by make: it has its own
# configure and taking it over here would just reimplement that badly.
$(BUILD)/libcpp/libcpp.a:
	@echo "libcpp is not built yet -- run ./fetch-libcpp.sh first" >&2
	@false

check: $(BINDIR)/vcpp
	@$(HERE)test/compare.sh

clean:
	rm -rf $(BINDIR)

# Leaves .dl/ alone so the tarball is not downloaded twice.
distclean: clean
	rm -rf $(BUILD) $(SRC)
