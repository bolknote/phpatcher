#!/usr/bin/env bash
# Build and run the standalone C++ unit tests for the patch core.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"

CXX="${CXX:-c++}"
out="$here/test_patch"

"$CXX" -std=c++17 -O2 -Wall -Wextra \
    "$here/test_patch.cpp" "$root/patch.cpp" \
    -o "$out"

"$out"
