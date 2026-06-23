# Build the phpatcher helper tools.
#
#   make -C tools          # build phpatcher-index, phpatcher-match, phpatcher-changes
#   make -C tools clean
#
# These are standalone C++17 command-line tools, independent of the PHP build.

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
LIBGIT2_CFLAGS ?= $(shell pkg-config --cflags libgit2 2>/dev/null)
LIBGIT2_LIBS   ?= $(shell pkg-config --libs libgit2 2>/dev/null)

BINS := phpatcher-index phpatcher-match phpatcher-changes

all: $(BINS)

phpatcher-index: indexer.cpp normalizer.hpp
	$(CXX) $(CXXFLAGS) -pthread -o $@ indexer.cpp

phpatcher-match: matcher.cpp matcher_core.hpp normalizer.hpp ../patch.hpp
	$(CXX) $(CXXFLAGS) -o $@ matcher.cpp

phpatcher-changes: changes.cpp
	$(CXX) $(CXXFLAGS) $(LIBGIT2_CFLAGS) -o $@ changes.cpp $(LIBGIT2_LIBS)

clean:
	rm -f $(BINS)

.PHONY: all clean
