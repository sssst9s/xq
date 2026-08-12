# SPDX-License-Identifier: Apache-2.0
#
# Portable hand-written Makefile. No cmake, no meson, no configure: the
# library has zero dependencies, so a generator would only add a build-time
# dependency to a project whose selling point is not having any.
#
#   make              build the static library and the tests
#   make test         build and run the test suite
#   make ubsan        rebuild under UBSan and run the tests
#   make asan         rebuild under ASan + UBSan and run the tests
#   make sanitize     both of the above
#   make fuzz         build the libFuzzer targets (clang only)
#   make WITH_ZSTD=1  additionally build the optional zstd codec
#
# KNOWN ISSUE: on macOS 26 (Darwin 25.x) with Apple clang 17, the AddressSanitizer
# runtime hangs inside __asan::AsanInitFromRtl() during dyld startup -- before
# main() runs -- for every binary, including `int main(void){return 0;}`. It is
# not specific to this project. `make ubsan` works and is what to run locally on
# such a host; ASan coverage comes from CI on Linux.

CC      ?= cc
AR      ?= ar
BUILD   ?= build

WARNINGS = -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
           -Wcast-qual -Wpointer-arith -Wwrite-strings -Wmissing-prototypes \
           -Wstrict-prototypes -Wold-style-definition -Wvla

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 $(WARNINGS)
LDLIBS  += -lpthread
DEPFLAGS = -MMD -MP
CPPFLAGS += -Iinclude -Isrc/common -Isrc/format -Isrc/codec -Isrc/core -Isrc/platform

# Where Homebrew puts zstd on macOS; harmless elsewhere.
ZSTD_CPPFLAGS ?= $(if $(wildcard /opt/homebrew/include/zstd.h),-I/opt/homebrew/include,)
ZSTD_LDFLAGS  ?= $(if $(wildcard /opt/homebrew/lib/libzstd.a),-L/opt/homebrew/lib,)

# The optional zstd codec. Off by default: the shipping codecs have no
# dependencies. Used as a known-good strong codec to develop the container
# and the dictionary against, and as an honest ceiling for benchmarks.
#
# A configuration-specific build directory is not optional here. make tracks
# file timestamps, not the flags a file was built with, so without it,
# switching WITH_ZSTD on or off leaves incompatible objects in place and the
# link fails -- or worse, succeeds against a mixed build.
LIB_EXTRA =
ifdef WITH_ZSTD
CPPFLAGS += -DXQ_WITH_ZSTD $(ZSTD_CPPFLAGS)
LDFLAGS  += $(ZSTD_LDFLAGS)
LDLIBS   += -lzstd
BUILD    := $(BUILD)/with-zstd
LIB_EXTRA = src/codec/xq_codec_zstd.c
endif

LIB_SRC = src/common/xq_common.c \
          src/common/xq_crc32c.c \
          src/common/xq_xxh64.c \
          src/format/xq_format.c \
          src/codec/xq_codec.c \
          src/codec/xq_codec_stored.c \
          src/codec/xq_codec_lzb.c \
          src/platform/xq_file.c \
          src/platform/xq_thread.c \
          src/core/xq_encoder.c \
          src/core/xq_decoder.c \
          src/core/xq_dict.c \
          src/core/xq_reader.c \
          src/core/xq_repair.c \
          src/core/xq_api.c

LIB_SRC += $(LIB_EXTRA)
LIB_OBJ = $(LIB_SRC:%.c=$(BUILD)/%.o)
LIB     = $(BUILD)/libxq.a

TEST_SRC = $(wildcard tests/test_*.c)
TEST_BIN = $(TEST_SRC:tests/%.c=$(BUILD)/%)

CLI_SRC = cli/main.c
CLI_BIN = $(BUILD)/xq

BENCH_SRC = $(wildcard benchmarks/*.c)
BENCH_BIN = $(BENCH_SRC:benchmarks/%.c=$(BUILD)/bench/%)

.PHONY: all test check bench clean asan ubsan sanitize fuzz fuzz-run

all: $(LIB) $(CLI_BIN) $(TEST_BIN)

$(LIB): $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Rebuild when a header changes, not just when the .c does.
-include $(LIB_OBJ:.o=.d)

$(BUILD)/%: tests/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(CLI_BIN): $(CLI_SRC) $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CLI_SRC) $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/bench/%: benchmarks/%.c $(LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

bench: $(BENCH_BIN)

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do echo "--- $$t"; $$t || exit 1; done

# Everything that can run on any host, in one command. This is what CI runs
# and what a contributor should run before sending a patch.
check:
	@echo "=== unit + malformed-input tests ==="
	@$(MAKE) --no-print-directory test
	@echo
	@echo "=== undefined behaviour sanitiser ==="
	@$(MAKE) --no-print-directory ubsan
	@echo
	@echo "=== format fuzzing ==="
	@$(MAKE) --no-print-directory fuzz-run
	@echo
	@echo "all checks passed"

# Sanitisers are not optional for a format parser. -fno-sanitize-recover makes
# the first undefined operation a hard failure rather than a log line, so a
# passing run is a real signal rather than something to grep.
#
# Signed/unsigned overflow are included deliberately: this code does arithmetic
# on lengths and offsets taken from untrusted input, which is exactly where
# wraparound bugs turn into overreads.
UBSAN_FLAGS = -fsanitize=undefined,integer-divide-by-zero,unsigned-integer-overflow \
              -fno-sanitize-recover=all -fno-omit-frame-pointer

ubsan:
	$(MAKE) test BUILD=$(BUILD)/ubsan CFLAGS="-O1 -g -std=c11 $(UBSAN_FLAGS)"

asan:
	$(MAKE) test BUILD=$(BUILD)/asan \
	    CFLAGS="-O1 -g -std=c11 -fsanitize=address $(UBSAN_FLAGS)"

sanitize: ubsan asan

# Prefer coverage-guided libFuzzer where its runtime exists. Apple's clang does
# not ship one, so fall back to the standalone driver in tests/fuzz/fuzz_driver.c
# -- otherwise the harnesses would silently become unbuildable on macOS and rot.
FUZZ_HARNESS = $(filter-out tests/fuzz/fuzz_driver.c,$(wildcard tests/fuzz/fuzz_*.c))
HAVE_LIBFUZZER := $(shell printf 'int LLVMFuzzerTestOneInput(const unsigned char*e,unsigned long s){(void)e;(void)s;return 0;}' > /tmp/.xqfz.c 2>/dev/null && $(CC) -fsanitize=fuzzer /tmp/.xqfz.c -o /tmp/.xqfz.bin >/dev/null 2>&1 && echo yes || echo no)

fuzz:
	@mkdir -p $(BUILD)/fuzz
ifeq ($(HAVE_LIBFUZZER),yes)
	@echo "using libFuzzer (coverage-guided)"
	@for f in $(FUZZ_HARNESS); do \
	    echo "--- $$f"; \
	    $(CC) $(CPPFLAGS) -O1 -g -std=c11 \
	        -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
	        $$f $(LIB_SRC) $(LDFLAGS) $(LDLIBS) \
	        -o $(BUILD)/fuzz/$$(basename $$f .c) || exit 1; \
	done
else
	@echo "libFuzzer runtime unavailable; building standalone driver instead"
	@for f in $(FUZZ_HARNESS); do \
	    echo "--- $$f"; \
	    $(CC) $(CPPFLAGS) -O1 -g -std=c11 $(UBSAN_FLAGS) \
	        $$f tests/fuzz/fuzz_driver.c $(LIB_SRC) $(LDFLAGS) $(LDLIBS) \
	        -o $(BUILD)/fuzz/$$(basename $$f .c) || exit 1; \
	done
endif

# Short deterministic run, suitable for CI on any host.
fuzz-run: fuzz
	@for b in $(BUILD)/fuzz/*; do \
	    [ -f "$$b" ] && [ -x "$$b" ] || continue; \
	    echo "--- $$b"; \
	    XQ_FUZZ_ITERS=$${XQ_FUZZ_ITERS:-200000} $$b || exit 1; \
	done

clean:
	rm -rf $(BUILD)
