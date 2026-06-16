# Standalone C++ unit tests for the phpatcher patch core (no PHP required).
#
#   make            build and run the optimized tests
#   make sanitize   build and run under AddressSanitizer + UBSan
#   make clean      remove build artifacts

CXX    ?= c++
CXXSTD := -std=c++17
WARN   := -Wall -Wextra -Wshadow -Wconversion -Wsign-conversion -Wpedantic
OPT    ?= -O2

ROOT := $(abspath $(CURDIR)/../..)
SRC  := test_patch.cpp $(ROOT)/patch.cpp
DEPS := test_patch.cpp $(ROOT)/patch.cpp $(ROOT)/patch.hpp

.PHONY: all test sanitize clean
all: test

test: test_patch
	./test_patch

test_patch: $(DEPS)
	$(CXX) $(CXXSTD) $(OPT) $(WARN) $(SRC) -o $@

# How the core is validated during development: AddressSanitizer + the full
# UndefinedBehaviorSanitizer (aborting on the first finding). The unsigned-wrap
# sub-checks are disabled because they flag legal wraparound inside libc++'s
# std::hash, which is defined behavior, not a bug.
SAN := address,undefined,integer,implicit-conversion,nullability,local-bounds,float-divide-by-zero
sanitize: $(DEPS)
	$(CXX) $(CXXSTD) -O1 -g $(WARN) -fsanitize=$(SAN) \
		-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base \
		-fno-sanitize-recover=all -fno-omit-frame-pointer $(SRC) -o test_patch_san
	UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./test_patch_san

clean:
	rm -rf test_patch test_patch_san *.dSYM
