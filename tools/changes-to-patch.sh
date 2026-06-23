#!/usr/bin/env bash
#
# changes-to-patch.sh — turn uncommitted git changes into a phpatcher-ed
# bundle.
#
# For every file that differs from the base revision it emits:
#
#     # file: <path-relative-to-repo-root>
#     <output of `diff -e <base version> <working-tree version>`>
#
# The patch contains only line numbers and the new text — never the
# original/removed lines. Corpus mode may also emit phpatcher-specific `r`
# directives inside input blocks, so the result is not raw `diff -e` syntax.
#
# Usage:
#   tools/changes-to-patch.sh                       # all changes -> stdout
#   tools/changes-to-patch.sh -o my.patch           # ... -> file
#   tools/changes-to-patch.sh src/A.php src/B.php   # only these files
#   tools/changes-to-patch.sh -b HEAD~3             # diff against another base
#   tools/changes-to-patch.sh -c                    # de-duplicate moved code
#   tools/changes-to-patch.sh -c --normalize        # token-normalized matching
#
# Corpus mode (-c/--corpus):
#   Refactoring that moves a block of code between files would otherwise quote
#   the block twice (deleted from the source, re-added at the destination). With
#   -c the inserted text is factorized against the *base* revision of the tree:
#   runs of lines that already exist in the corpus are replaced by a reference
#
#       r "path/to/original.php" A B  # h:<hash>
#
#   that phpatcher resolves (and hash-verifies) at apply time. This needs the
#   compiled helper tools next to this script (phpatcher-index, phpatcher-match;
#   build them with: make -C tools).
#
# Notes:
#   * Run it from inside the git repository whose changes you want to capture.
#   * Explicit file arguments must be given relative to the repository root.
#   * Set phpatcher.base_dir to the repository root so the "# file:" paths and
#     the corpus references resolve to the right files on disk.
#   * References point at the *base* revision, which must match the code deployed
#     on the target (the pre-patch sources phpatcher reads). 
#   * Added (brand-new) and deleted files are skipped: phpatcher patches files
#     that already exist on disk, so those cases are not representable.

set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

BASE="HEAD"
OUT=""
CORPUS=""
MIN_RUN="3"
INDEX_FILE=""
CORPUS_ROOT=""
HASH=""
NORMALIZE=""
EXCLUDES=()

usage() {
    cat <<'EOF'
Usage:
  tools/changes-to-patch.sh                       # all changes -> stdout
  tools/changes-to-patch.sh -o my.patch           # ... -> file
  tools/changes-to-patch.sh src/A.php src/B.php   # only these files
  tools/changes-to-patch.sh -b HEAD~3             # diff against another base
  tools/changes-to-patch.sh -c                    # de-duplicate moved code
  tools/changes-to-patch.sh -c --normalize        # token-normalized matching

Options:
  -b, --base <rev>      base revision to diff against (default: HEAD)
  -o, --output <file>   write the bundle to <file> instead of stdout
  -c, --corpus          replace moved/duplicated code with corpus references
      --normalize       build a PHP-token-normalized index (corpus mode)
  -n, --min-run <n>     min run length for a reference (corpus mode; default 3)
  -x, --exclude <dir>   directory name to skip when indexing (repeatable)
  -H, --hash            guard references with a content hash (h:) instead of the
                        byte length (s:, the default)
      --index <file>    reuse a prebuilt index instead of building one
      --corpus-root <d> base-revision tree to resolve references against
                        (default: a temporary checkout of <base>)
  -h, --help            show this help
EOF
}

die() { echo "$@" >&2; exit 1; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--base)    BASE="${2:?missing value for $1}"; shift 2 ;;
        -o|--output)  OUT="${2:?missing value for $1}"; shift 2 ;;
        -c|--corpus)  CORPUS=1; shift ;;
        --normalize)  NORMALIZE=1; shift ;;
        -n|--min-run) MIN_RUN="${2:?missing value for $1}"; shift 2 ;;
        -x|--exclude) EXCLUDES+=("${2:?missing value for $1}"); shift 2 ;;
        -H|--hash)    HASH=1; shift ;;
        --index)      INDEX_FILE="${2:?missing value for $1}"; shift 2 ;;
        --corpus-root) CORPUS_ROOT="${2:?missing value for $1}"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        --)           shift; break ;;
        -*)           die "Unknown option: $1" ;;
        *)            break ;;
    esac
done

# Remaining positional arguments are explicit file paths to capture.
args=("$@")

command -v git >/dev/null 2>&1 || die "git not found"

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" \
    || die "Not a git repository (run this from inside the project)."

git -C "$repo_root" rev-parse --verify --quiet "$BASE^{commit}" >/dev/null \
    || die "Base revision not found: $BASE"

# Determine the list of files to consider: explicit arguments, or every tracked
# file that differs from BASE in the working tree (staged + unstaged).
files=()
explicit_files=""
if [[ ${#args[@]} -gt 0 ]]; then
    files=("${args[@]}")
    explicit_files=1
else
    while IFS= read -r -d '' f; do
        files+=("$f")
    done < <(git -C "$repo_root" diff --no-renames -z --name-only --diff-filter=M "$BASE" --)
fi

[[ ${#files[@]} -gt 0 ]] || die "No changed files relative to $BASE."

# Single scratch directory, cleaned up on any exit.
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

# Materialize the base revision once. The older implementation used
# `git show "$BASE:$file"` per changed file, which is painfully slow for large
# cross-year diffs; a single archive is seconds even for a large tree.
base_tree=""
if [[ -n "$CORPUS_ROOT" ]]; then
    base_tree="$CORPUS_ROOT"
    [[ -d "$base_tree" ]] || die "corpus root not found: $base_tree"
else
    base_tree="$tmpdir/base"
    mkdir -p "$base_tree"
    git -C "$repo_root" archive "$BASE" | tar -x -C "$base_tree"
fi

# Binary files cannot be represented as ed scripts. Compute the skip set once
# instead of probing every file individually. Keep it as a plain file (rather
# than a bash associative array) so macOS' old /bin/bash remains supported.
binary_list="$tmpdir/binary-files"
: >"$binary_list"
if [[ -n "$explicit_files" ]]; then
    while IFS=$'\t' read -r added deleted path; do
        if [[ "$added" == "-" && "$deleted" == "-" && -n "$path" ]]; then
            printf '%s\n' "$path" >>"$binary_list"
        fi
    done < <(git -C "$repo_root" diff --no-renames --numstat --diff-filter=M "$BASE" -- "${args[@]}" 2>/dev/null || true)
else
    while IFS=$'\t' read -r added deleted path; do
        if [[ "$added" == "-" && "$deleted" == "-" && -n "$path" ]]; then
            printf '%s\n' "$path" >>"$binary_list"
        fi
    done < <(git -C "$repo_root" diff --no-renames --numstat --diff-filter=M "$BASE" -- 2>/dev/null || true)
fi

# --- corpus mode setup ------------------------------------------------------
# Materialize the base revision (so references read pre-change content), build
# (or reuse) an index over it, and remember where the matcher binary lives.
MATCHER=""
INDEX=""
if [[ -n "$CORPUS" ]]; then
    MATCHER="$script_dir/phpatcher-match"
    indexer="$script_dir/phpatcher-index"
    [[ -x "$MATCHER" ]]  || die "corpus mode needs $MATCHER (build it with: make -C tools)"

    if [[ -n "$CORPUS_ROOT" ]]; then
        corpus_dir="$base_tree"
    else
        corpus_dir="$base_tree"
    fi

    if [[ -n "$INDEX_FILE" ]]; then
        INDEX="$INDEX_FILE"
        [[ -f "$INDEX" ]] || die "index file not found: $INDEX"
    else
        [[ -x "$indexer" ]] || die "corpus mode needs $indexer (build it with: make -C tools)"
        INDEX="$tmpdir/corpus.idx"
        xargs=()
        for d in "${EXCLUDES[@]:-}"; do [[ -n "$d" ]] && xargs+=("-x" "$d"); done
        [[ -n "$NORMALIZE" ]] && xargs+=("--normalize")
        echo "Indexing base tree..." >&2
        "$indexer" "${xargs[@]}" -o "$INDEX" "$corpus_dir"
    fi
fi

# Read a `diff -e` script on stdin and, for every a/c input block, replace runs
# of lines that exist verbatim in the corpus with `!sed` references. Lines that
# are not part of an input block (commands, `d`, the lone `.`) pass through.
transform_ed_script() {
    local line
    while IFS= read -r line; do
        if [[ "$line" =~ ^[0-9]+(,[0-9]+)?[ac]$ ]]; then
            printf '%s\n' "$line"
            local block="" il
            while IFS= read -r il; do
                [[ "$il" == "." ]] && break
                block+="$il"$'\n'
            done
            local match_opts=(--root "$corpus_dir" --min-run "$MIN_RUN")
            [[ -n "$HASH" ]] && match_opts+=(--hash)
            printf '%s' "$block" | "$MATCHER" "${match_opts[@]}" "$INDEX"
            printf '.\n'
        else
            printf '%s\n' "$line"
        fi
    done
}

emit() {
    echo "# phpatcher-ed v1"
    echo "# Generated by tools/changes-to-patch.sh from changes vs ${BASE}."

    local f new orig emitted=0
    for f in "${files[@]}"; do
        [[ -z "$f" ]] && continue

        # Skip binary files (diff -e is meaningless for them).
        grep -Fqx -- "$f" "$binary_list" && continue

        new="$repo_root/$f"
        if [[ ! -f "$new" ]]; then
            continue
        fi

        orig="$base_tree/$f"
        if [[ ! -f "$orig" ]]; then
            orig="$tmpdir/empty"
            : >"$orig"
        fi

        if [[ -n "$explicit_files" ]] && diff -q "$orig" "$new" >/dev/null 2>&1; then
            continue
        fi

        echo "# file: $f"
        # `diff -e` exits 1 when files differ — expected here, not an error.
        if [[ -n "$CORPUS" ]]; then
            { diff -e "$orig" "$new" 2>/dev/null || true; } | transform_ed_script
        else
            diff -e "$orig" "$new" 2>/dev/null || true
        fi
        emitted=$((emitted + 1))
    done

    [[ "$emitted" -gt 0 ]] || die "Nothing to patch: no applicable changes found."
}

if [[ -n "$OUT" ]]; then
    emit >"$OUT"
    echo "Wrote $OUT" >&2
else
    emit
fi
