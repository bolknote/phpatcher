#!/usr/bin/env bash
# Build and run the standalone C++ unit tests for the patch core.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"

CXX="${CXX:-c++}"

status=0

build_run() {
    local name="$1"; shift
    local out="$here/$name"
    echo "== $name =="
    "$CXX" -std=c++17 -O2 -Wall -Wextra "$@" -o "$out"
    "$out" || status=1
}

# Patch core (needs patch.cpp); matcher core is header-only.
build_run test_patch   "$here/test_patch.cpp" "$root/patch.cpp"
build_run test_matcher "$here/test_matcher.cpp"

exit "$status"
