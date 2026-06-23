#!/usr/bin/env bash
# Round-trip smoke test for tools/phpatcher-changes.
#
# It is standalone (not wired into CI) because it needs git, ed, and a
# phpatcher-changes binary built against libgit2 — none of which the lean unit
# CI image provides. Run it locally after building the tools:
#
#   make -C tools && tests/tools/roundtrip.sh
#
# What it proves:
#   1. plain mode  — the generated phpatcher-ed bundle, applied with stock `ed`
#                    to the base file, reproduces the modified file byte-for-byte
#                    (so the emitted commands and their descending order are
#                    actually correct, not merely well-formed).
#   2. corpus mode — when an added run already exists in the corpus index, the
#                    bundle factors it out into an `r "..."` reference instead of
#                    quoting it verbatim.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
tools="$root/tools"
changes="$tools/phpatcher-changes"
index="$tools/phpatcher-index"

skip() { echo "SKIP: $*"; exit 0; }

[ -x "$changes" ] || skip "$changes not built (run: make -C tools)"
[ -x "$index" ]   || skip "$index not built (run: make -C tools)"
command -v git >/dev/null 2>&1 || skip "git not available"
command -v ed  >/dev/null 2>&1 || skip "ed not available"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Plain round-trip: generate -> apply with ed -> must equal the new file.
# ---------------------------------------------------------------------------
repo="$work/repo"
mkdir -p "$repo"
(
    cd "$repo"
    git init -q
    git config user.email t@example.com
    git config user.name test
    printf 'line one\nline two\nline three\nline four\n' > sample.php
    git add sample.php
    git commit -qm base
    # The new working-tree content: change line 2, append a line.
    printf 'line one\nline two CHANGED\nline three\nline four\nline five added\n' > sample.php
)
new_content="$(cat "$repo/sample.php")"

bundle="$work/plain.patch"
( cd "$repo" && "$changes" -b HEAD sample.php ) > "$bundle"

head -n1 "$bundle" | grep -q '^# phpatcher-ed' || fail "bundle missing magic header"
grep -q '^# file: sample.php' "$bundle" || fail "bundle missing file header"

# Reconstruct: start from the base file, feed the ed commands (everything that
# is not a phpatcher comment/header) to stock ed, and compare to the new file.
rebuilt="$work/rebuilt.php"
( cd "$repo" && git show HEAD:sample.php ) > "$rebuilt"
{ grep -v '^#' "$bundle"; printf 'w\nq\n'; } | ed -s "$rebuilt" >/dev/null

if [ "$(cat "$rebuilt")" != "$new_content" ]; then
    echo "--- expected ---"; printf '%s\n' "$new_content"
    echo "--- rebuilt ----"; cat "$rebuilt"
    fail "ed round-trip did not reproduce the modified file"
fi
echo "PASS: plain round-trip reproduces the modified file"

# ---------------------------------------------------------------------------
# 2. Corpus mode: an added run already present in the corpus becomes an `r` ref.
# ---------------------------------------------------------------------------
corpus="$work/corpus"
mkdir -p "$corpus"
# A donor file whose 4-line body we will "add" to the target verbatim.
cat > "$corpus/donor.php" <<'PHP'
<?php
function shared_helper(): string
{
    return "shared body that is long enough to be worth referencing";
}
PHP

idx="$work/corpus.idx"
"$index" -o "$idx" "$corpus" >/dev/null 2>&1 || fail "phpatcher-index failed"

crepo="$work/crepo"
mkdir -p "$crepo"
(
    cd "$crepo"
    git init -q
    git config user.email t@example.com
    git config user.name test
    printf '<?php\n// target\n' > target.php
    git add target.php
    git commit -qm base
    # Append the donor's helper body verbatim.
    cat >> target.php <<'PHP'
function shared_helper(): string
{
    return "shared body that is long enough to be worth referencing";
}
PHP
)

cbundle="$work/corpus.patch"
( cd "$crepo" && "$changes" -b HEAD -c --index "$idx" --corpus-root "$corpus" \
    --min-run 3 target.php ) > "$cbundle"

if grep -Eq '^r "?[^"]*donor\.php"? ' "$cbundle"; then
    echo "PASS: corpus mode factored the added run into an r reference"
else
    echo "--- corpus bundle ---"; cat "$cbundle"
    fail "corpus mode did not emit an r reference for a known run"
fi

echo "roundtrip OK"
