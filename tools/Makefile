# Build the phpatcher helper tools (corpus indexer + matcher).
#
#   make -C tools          # build phpatcher-index and phpatcher-match
#   make -C tools clean
#
# These are standalone C++17 command-line tools, independent of the PHP build.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra

BINS := phpatcher-index phpatcher-match

all: $(BINS)

phpatcher-index: indexer.cpp
	$(CXX) $(CXXFLAGS) -o $@ indexer.cpp

phpatcher-match: matcher.cpp matcher_core.hpp ../patch.hpp
	$(CXX) $(CXXFLAGS) -o $@ matcher.cpp

clean:
	rm -f $(BINS)

.PHONY: all clean
