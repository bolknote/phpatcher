#!/usr/bin/env bash
#
# changes-to-patch.sh — compatibility wrapper for the libgit2 generator.
#
# New generation happens in tools/phpatcher-changes. This wrapper keeps the old
# command name and convenience options, including auto-building a corpus index
# when -c is used without --index.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE="HEAD"
TARGET=""
OUT=""
CORPUS=""
MIN_RUN="3"
INDEX_FILE=""
CORPUS_ROOT=""
HASH=""
NORMALIZE=""
PHP_BIN="php"
NORMALIZER=""
EXCLUDES=()
PATHS=()

usage() {
    cat <<'EOF'
Usage:
  tools/changes-to-patch.sh                       # all changes -> stdout
  tools/changes-to-patch.sh -o my.patch           # ... -> file
  tools/changes-to-patch.sh src/A.php src/B.php   # only these files
  tools/changes-to-patch.sh -b HEAD~3             # diff against another base
  tools/changes-to-patch.sh -b HEAD~3 --to HEAD   # base -> target revision
  tools/changes-to-patch.sh -c                    # de-duplicate moved code
  tools/changes-to-patch.sh -c --normalize        # token-normalized matching

Options:
  -b, --base <rev>       base revision to diff against (default: HEAD)
      --to <rev>         target revision (default: working tree)
  -o, --output <file>    write the bundle to <file> instead of stdout
  -c, --corpus           replace moved/duplicated code with corpus references
      --normalize        build a PHP-token-normalized index (corpus mode)
  -n, --min-run <n>      min run length for a reference (corpus mode; default 3)
  -x, --exclude <dir>    directory name to skip when indexing (repeatable)
  -H, --hash             guard references with a content hash (h:) instead of the
                         byte length (s:, the default)
      --index <file>     reuse a prebuilt index instead of building one
      --corpus-root <d>  base-revision tree to resolve references against
                         (default: a temporary checkout of <base>)
      --php <path>       PHP interpreter for normalized indexes (default: php)
      --normalizer <p>   normalize.php path (default: next to phpatcher-changes)
  -h, --help             show this help
EOF
}

die() {
    echo "$@" >&2
    exit 2
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--base)       BASE="${2:?missing value for $1}"; shift 2 ;;
        --to)            TARGET="${2:?missing value for $1}"; shift 2 ;;
        -o|--output)     OUT="${2:?missing value for $1}"; shift 2 ;;
        -c|--corpus)     CORPUS=1; shift ;;
        --normalize)     NORMALIZE=1; shift ;;
        -n|--min-run)    MIN_RUN="${2:?missing value for $1}"; shift 2 ;;
        -x|--exclude)    EXCLUDES+=("${2:?missing value for $1}"); shift 2 ;;
        -H|--hash)       HASH=1; shift ;;
        --index)         INDEX_FILE="${2:?missing value for $1}"; shift 2 ;;
        --corpus-root)   CORPUS_ROOT="${2:?missing value for $1}"; shift 2 ;;
        --php)           PHP_BIN="${2:?missing value for $1}"; shift 2 ;;
        --normalizer)    NORMALIZER="${2:?missing value for $1}"; shift 2 ;;
        -h|--help)       usage; exit 0 ;;
        --)              shift; PATHS+=("$@"); break ;;
        -*)              die "Unknown option: $1" ;;
        *)               PATHS+=("$1"); shift ;;
    esac
done

changes="$script_dir/phpatcher-changes"
indexer="$script_dir/phpatcher-index"
[[ -x "$changes" ]] || die "phpatcher-changes not found: $changes (build it with: make -C tools)"

command -v git >/dev/null 2>&1 || die "git not found"
repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
    || die "Not a git repository (run this from inside the project)."
git -C "$repo_root" rev-parse --verify --quiet "$BASE^{commit}" >/dev/null \
    || die "Base revision not found: $BASE"

tmpdir=""
cleanup() {
    [[ -z "$tmpdir" ]] || rm -rf "$tmpdir"
}
trap cleanup EXIT

cmd=("$changes" -b "$BASE")
[[ -z "$TARGET" ]] || cmd+=(--to "$TARGET")
[[ -z "$OUT" ]] || cmd+=(-o "$OUT")

if [[ -n "$CORPUS" ]]; then
    cmd+=(-c -n "$MIN_RUN")
    [[ -z "$HASH" ]] || cmd+=(-H)
    [[ -z "$NORMALIZER" ]] || cmd+=(--normalizer "$NORMALIZER")
    [[ "$PHP_BIN" == "php" ]] || cmd+=(--php "$PHP_BIN")

    if [[ -z "$CORPUS_ROOT" ]]; then
        tmpdir="$(mktemp -d)"
        CORPUS_ROOT="$tmpdir/corpus"
        mkdir -p "$CORPUS_ROOT"
        git -C "$repo_root" archive "$BASE" | tar -x -C "$CORPUS_ROOT"
    fi
    [[ -d "$CORPUS_ROOT" ]] || die "corpus root not found: $CORPUS_ROOT"
    cmd+=(--corpus-root "$CORPUS_ROOT")

    if [[ -z "$INDEX_FILE" ]]; then
        [[ -x "$indexer" ]] || die "phpatcher-index not found: $indexer (build it with: make -C tools)"
        if [[ -z "$tmpdir" ]]; then
            tmpdir="$(mktemp -d)"
        fi
        INDEX_FILE="$tmpdir/corpus.idx"
        index_args=()
        for d in "${EXCLUDES[@]:-}"; do
            [[ -z "$d" ]] || index_args+=(-x "$d")
        done
        [[ -z "$NORMALIZE" ]] || index_args+=(--normalize)
        "$indexer" "${index_args[@]}" -o "$INDEX_FILE" "$CORPUS_ROOT" >&2
    fi
    [[ -f "$INDEX_FILE" ]] || die "index file not found: $INDEX_FILE"
    cmd+=(--index "$INDEX_FILE")
else
    [[ -z "$NORMALIZE" ]] || die "--normalize requires --corpus"
    [[ -z "$INDEX_FILE" ]] || die "--index requires --corpus"
    [[ -z "$CORPUS_ROOT" ]] || die "--corpus-root requires --corpus"
    [[ ${#EXCLUDES[@]} -eq 0 ]] || die "--exclude requires --corpus"
fi

cmd+=("${PATHS[@]}")

cd "$repo_root"
"${cmd[@]}"
