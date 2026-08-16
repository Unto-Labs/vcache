# vcache -- build via plain make, as specified in the plan.
#
#   make            build bin/vcache
#   make test       build and run the test suite
#   make clean      remove build output
#
# Third-party sources live under third-party/ and are committed, except
# gperftools, which is fetched and built by third-party/fetch.sh. Run
# `make deps` once on a fresh checkout.

CXX      ?= g++
CC       ?= gcc
BUILD    ?= release

TOP      := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
SRC      := $(TOP)/src
TP       := $(TOP)/third-party
OBJDIR   := $(TOP)/build/$(BUILD)
BINDIR   := $(TOP)/bin

# ---- third-party locations --------------------------------------------------

# Minimal Boost subset, committed to the repository: 932 of Boost 1.86.0's
# 15,828 headers, which is everything Spirit X3 reaches. Regenerate with
# `make boost-subset` if a new include reaches further into Boost.
BOOST_INC   := $(TP)/boost
BLAKE3_DIR  := $(TP)/blake3
TOMLPP_INC  := $(TP)/tomlplusplus
# gperftools splits its shared internals (spinlock, sysinfo, logging) into a
# second archive, so both are needed and tcmalloc must come first.
TCMALLOC_A  := $(TP)/gperftools/build/libtcmalloc_minimal.a \
               $(TP)/gperftools/build/libcommon.a

# libcurl headers only: the library itself is dlopen'd at runtime, so it is not
# a link-time dependency. See src/storage/curl_api.h for why. Multiarch puts the
# headers outside the default include path on Debian/Ubuntu.
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)

# ---- BLAKE3 architecture selection ------------------------------------------
#
# Only the x86-64 ELF kernels are vendored (see third-party/blake3/PROVENANCE.md).
# Elsewhere the portable C implementation is built instead: slower, still
# correct. The BLAKE3_NO_* defines tell blake3_dispatch.c not to look for
# kernels that are not there.
#
# Defined before the flags section because CFLAGS is expanded immediately.

BLAKE3_ARCH := $(shell uname -m)
ifeq ($(BLAKE3_ARCH),x86_64)
  BLAKE3_S    := $(BLAKE3_DIR)/blake3_sse2_x86-64_unix.S \
                 $(BLAKE3_DIR)/blake3_sse41_x86-64_unix.S \
                 $(BLAKE3_DIR)/blake3_avx2_x86-64_unix.S \
                 $(BLAKE3_DIR)/blake3_avx512_x86-64_unix.S
  BLAKE3_DEFS :=
else
  BLAKE3_S    :=
  # BLAKE3_USE_NEON=0 is not optional on aarch64: upstream's blake3_impl.h
  # autodetects it to 1 there, and blake3_dispatch.c then calls
  # blake3_hash_many_neon, which lives in blake3_neon.c -- a file this subset
  # does not vendor. Without this the link fails on undefined references
  # rather than falling back to the portable path. See third-party/blake3/
  # PROVENANCE.md if ARM hashing throughput ever justifies vendoring it.
  #
  # -Wno-unused-variable: with every SIMD path disabled, upstream's
  # blake3_dispatch.c computes a cpu_feature value it then never consults.
  # Upstream code, so suppressed rather than patched.
  BLAKE3_DEFS := -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 \
                 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0 -Wno-unused-variable
endif

# ---- flags ------------------------------------------------------------------

# -Wno-missing-field-initializers: every aggregate here declares default member
# initializers, so a C++20 designated-initializer list that omits a field is
# correct by construction and the warning is pure noise.
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
INCLUDES := -I$(SRC) -I$(BOOST_INC) -I$(BLAKE3_DIR) -I$(TOMLPP_INC) $(CURL_CFLAGS)

# ---- capability probes ------------------------------------------------------
#
# Both features are recent, so detect rather than assume: -gz=zstd needs a gcc
# built with zstd support (13+) and binutils 2.40+, and LTO needs a working
# linker plugin. A toolchain without them still builds, just larger.

PROBE_SRC := $(shell mktemp --suffix=.cc 2>/dev/null || echo /tmp/vcache-probe.cc)
$(shell echo 'int main(){return 0;}' > $(PROBE_SRC))

HAVE_GZ_ZSTD := $(shell $(CXX) -ggdb3 -gz=zstd -c $(PROBE_SRC) -o /dev/null >/dev/null 2>&1 && echo 1)
HAVE_LTO     := $(shell $(CXX) -flto=auto -O2 $(PROBE_SRC) -o /dev/null >/dev/null 2>&1 && echo 1)

# ---- optimisation, debug info and LTO ---------------------------------------

ifeq ($(BUILD),debug)
  OPT     := -O0 -ggdb3 -fno-omit-frame-pointer
  LTO     :=
  SECTIONS :=
else
  # -ggdb3 keeps macro definitions, which matters for a codebase this
  # macro-light only in that it makes gdb able to expand VCACHE_LOG.
  OPT := -O3 -ggdb3

ifeq ($(HAVE_LTO),1)
  # -flto=auto parallelises the link step across available cores.
  # Size comes from the section-splitting pair below plus --gc-sections at link
  # time: LTO alone tends to *grow* a binary through inlining, so the dead-code
  # removal is what actually pays for it here.
  LTO := -flto=auto -fno-fat-lto-objects
else
  LTO :=
endif
  SECTIONS := -ffunction-sections -fdata-sections
endif

ifeq ($(HAVE_GZ_ZSTD),1)
  # zstd compresses DWARF far better than zlib at similar cost, and -ggdb3
  # produces a lot of DWARF.
  DEBUG_FMT    := -gz=zstd
  DEBUG_FMT_LD := -Wl,--compress-debug-sections=zstd
else
  DEBUG_FMT    :=
  DEBUG_FMT_LD :=
endif

CXXFLAGS := -std=c++20 $(OPT) $(LTO) $(SECTIONS) $(DEBUG_FMT) $(WARN) $(INCLUDES) \
            -pthread -DTOML_EXCEPTIONS=0 -fno-strict-aliasing
# Only third-party C (BLAKE3) is compiled with these.
CFLAGS   := -std=c11 $(OPT) $(LTO) $(SECTIONS) $(DEBUG_FMT) $(WARN) -I$(BLAKE3_DIR) \
            $(BLAKE3_DEFS)
# The BLAKE3 kernels are hand-written assembly: no optimisation or LTO applies,
# but they should still carry compressed debug info.
ASFLAGS  := -g $(DEBUG_FMT)

# Static where practical, as the plan asks. Only libc, libm and the loader are
# dynamic; libm comes in via tcmalloc's use of log2. libcurl is not linked at
# all -- it is dlopen'd only when an S3 layer is constructed.
#
# LTO flags must be repeated at link time, and the optimisation level with them,
# since that is when code generation actually happens.
LDFLAGS  := -static-libstdc++ -static-libgcc -pthread $(OPT) $(LTO) $(DEBUG_FMT_LD) \
            -Wl,--gc-sections -Wl,--as-needed -Wl,-O1
# No -lcurl and no -lcrypto: libcurl is dlopen'd on demand, and SHA-256/HMAC are
# vendored in src/hash/sha256.cc. Between them that removes about thirty shared
# objects from the load set of every compilation.
#
# -ldl is a no-op on glibc 2.34+, where dlopen moved into libc; --as-needed drops
# it from DT_NEEDED. Kept for older glibc, which needs it for dlopen.
LDLIBS   := $(TCMALLOC_A) -ldl

# ---- sources ----------------------------------------------------------------

VCACHE_SRCS := \
  $(SRC)/util/str.cc \
  $(SRC)/util/fs.cc \
  $(SRC)/util/subprocess.cc \
  $(SRC)/util/log.cc \
  $(SRC)/hash/hasher.cc \
  $(SRC)/hash/sha256.cc \
  $(SRC)/core/roots.cc \
  $(SRC)/core/config.cc \
  $(SRC)/core/depfile.cc \
  $(SRC)/core/preprocessed.cc \
  $(SRC)/core/stats.cc \
  $(SRC)/core/compile.cc \
  $(SRC)/args/compiler_args.cc \
  $(SRC)/args/rustc_args.cc \
  $(SRC)/rust/rust_compile.cc \
  $(SRC)/storage/disk_storage.cc \
  $(SRC)/storage/s3_storage.cc \
  $(SRC)/storage/chain.cc \
  $(SRC)/storage/curl_api.cc

MAIN_SRC := $(SRC)/main.cc

# BLAKE3 portable C. The architecture-specific kernels are selected above.
BLAKE3_C  := $(BLAKE3_DIR)/blake3.c $(BLAKE3_DIR)/blake3_dispatch.c \
             $(BLAKE3_DIR)/blake3_portable.c

VCACHE_OBJS := $(patsubst $(TOP)/%.cc,$(OBJDIR)/%.o,$(VCACHE_SRCS))
MAIN_OBJ    := $(patsubst $(TOP)/%.cc,$(OBJDIR)/%.o,$(MAIN_SRC))
BLAKE3_OBJS := $(patsubst $(TP)/%.c,$(OBJDIR)/tp/%.o,$(BLAKE3_C)) \
               $(patsubst $(TP)/%.S,$(OBJDIR)/tp/%.o,$(BLAKE3_S))

TEST_SRCS := $(wildcard $(TOP)/tests/*.cc)
TEST_OBJS := $(patsubst $(TOP)/%.cc,$(OBJDIR)/%.o,$(TEST_SRCS))

ALL_OBJS := $(VCACHE_OBJS) $(MAIN_OBJ) $(BLAKE3_OBJS) $(TEST_OBJS)
DEPS     := $(ALL_OBJS:.o=.d)

# ---- targets ----------------------------------------------------------------

.PHONY: all deps test clean distclean boost-subset

all: $(BINDIR)/vcache

deps:
	@$(TP)/fetch.sh

# Rebuilds third-party/boost from a full Boost tree. Only needed when an
# include starts reaching a part of Boost the subset does not carry.
boost-subset:
	@$(TP)/regen-boost-subset.sh $(CXX)

$(BINDIR)/vcache: $(VCACHE_OBJS) $(MAIN_OBJ) $(BLAKE3_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BINDIR)/vcache_test: $(VCACHE_OBJS) $(BLAKE3_OBJS) $(TEST_OBJS)
	@mkdir -p $(BINDIR)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: $(BINDIR)/vcache_test $(BINDIR)/vcache
	$(BINDIR)/vcache_test
	@$(TOP)/tests/integration_test.sh

$(OBJDIR)/%.o: $(TOP)/%.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/tp/%.o: $(TP)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR)/tp/%.o: $(TP)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c -o $@ $<

clean:
	rm -rf $(TOP)/build $(BINDIR)

distclean: clean
	rm -rf $(TP)/gperftools $(TP)/.dl $(TP)/boost-full-*

-include $(DEPS)
