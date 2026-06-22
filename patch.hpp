/*
 * phpatcher - in-memory unified-diff patching core.
 *
 * This translation unit is intentionally free of any PHP/Zend dependency so
 * that it can be unit-tested in isolation and reused outside of the extension.
 */
#ifndef PHPATCHER_PATCH_HPP
#define PHPATCHER_PATCH_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace phpatcher {

/* Role of a single unified-diff body line. Keeping the marker as a typed enum
 * (rather than the leading byte of the text) means apply() never re-parses a
 * character and an empty line is just an empty `text`, not a sentinel space. */
enum class LineKind : std::uint8_t {
    Context,    /* ' ' line, present unchanged on both sides           */
    Remove,     /* '-' line, removed from the original                 */
    Add,        /* '+' line, added in the patched file                 */
    NoNewline   /* '\' "No newline at end of file" annotation          */
};

/* One body line of a unified-diff hunk: its role plus its content (without the
 * leading marker; empty for a NoNewline annotation). */
struct DiffLine {
    LineKind kind = LineKind::Context;
    std::string text;
};

/* A single contiguous change region within a unified diff. */
struct Hunk {
    std::int64_t orig_start = 0;  /* 1-based first line of the hunk in the original */
    std::int64_t orig_count = 0;  /* number of original lines covered by the hunk   */
    std::int64_t new_start = 0;   /* 1-based first line of the hunk in the patched   */
    std::int64_t new_count = 0;   /* number of patched lines produced by the hunk   */
    std::vector<DiffLine> lines;  /* the hunk body, marker-decoded                  */
};

/*
 * A single ed-script editing command (the kind produced by `diff -e`).
 *
 * ed scripts describe a change purely by line numbers and replacement text;
 * they never quote the original/removed lines. `diff -e` also emits commands in
 * descending line order so they can be applied sequentially without the line
 * numbers shifting.
 */
struct EdCommand {
    enum Kind {
        Delete,  /* "M,Nd"  - remove lines M..N                       */
        Change,  /* "M,Nc"  - replace lines M..N with `lines`         */
        Append,  /* "Na"    - insert `lines` after line N (N may be 0)*/
        Insert   /* "Ni"    - insert `lines` before line N            */
    };

    Kind kind = Delete;
    std::int64_t start = 0;  /* 1-based first affected line (or "after" line for Append) */
    std::int64_t end = 0;    /* 1-based last affected line (ranges only)                 */
    std::vector<std::string> lines;  /* replacement/inserted content (no markers)        */
};

/*
 * All edits targeting one concrete file. A file patch is expressed either as
 * unified-diff hunks or as an ed script, never both: the variant makes the
 * illegal "both / neither" states unrepresentable. A default-constructed
 * FilePatch is an (empty) unified-diff patch.
 */
struct FilePatch {
    std::variant<std::vector<Hunk>, std::vector<EdCommand>> body{std::in_place_index<0>};

    [[nodiscard]] bool is_ed() const {
        return body.index() == 1;
    }

    [[nodiscard]] std::vector<Hunk>& hunks() { return std::get<0>(body); }
    [[nodiscard]] const std::vector<Hunk>& hunks() const { return std::get<0>(body); }
    [[nodiscard]] std::vector<EdCommand>& ed() { return std::get<1>(body); }
    [[nodiscard]] const std::vector<EdCommand>& ed() const { return std::get<1>(body); }

    /* Switch this patch to the ed-script representation (empty). */
    void make_ed() { body.emplace<1>(); }
};

/*
 * Parsed collection of file patches, held in a flat array sorted by canonical
 * absolute path. The structure is immutable after parse()/recanonicalize() and
 * therefore safe to share read-only across threads. A sorted vector (rather than
 * a node-based hash map) keeps the index contiguous: better cache behaviour, a
 * smaller footprint, and copy-on-write friendliness under a forking SAPI.
 */
class PatchSet {
public:
    using Entry = std::pair<std::string, FilePatch>;  /* (canonical path, edits) */

    /*
     * Parse a patch file (unified diff or ed-script bundle; auto-detected).
     * `base_dir` resolves the diff's relative a/ b/ paths into absolute,
     * canonical filesystem paths. Replaces any previously parsed content.
     * Returns false and fills `error` on a hard parse error (including a file
     * targeted by more than one section); an empty or hunk-less patch is a
     * successful no-op.
     */
    bool parse(const std::string& diff_text, const std::string& base_dir, std::string& error);

    /* Look up the patch for a canonical absolute path, or nullptr. */
    [[nodiscard]] const FilePatch* find(std::string_view canonical_path) const;

    [[nodiscard]] std::size_t file_count() const { return index_.size(); }

    /* The indexed file patches, sorted by canonical path. */
    [[nodiscard]] const std::vector<Entry>& files() const { return index_; }

    /*
     * O(log n) negative pre-filter for the compile hot path: returns true only
     * if some indexed file shares this basename. The compile hook uses it to
     * skip the (filesystem-touching) path canonicalization for the overwhelming
     * majority of files that are not patched, without any syscall — and without
     * materialising a std::string, since it accepts a string_view directly.
     */
    [[nodiscard]] bool has_basename(std::string_view basename) const;

    /* Return the final path component of `path` (text after the last '/'). */
    [[nodiscard]] static std::string basename_of(std::string_view path);

    /*
     * Apply `fp` to `original`, producing `out`. Returns false and fills
     * `error` if the patch does not cleanly apply (context/removal mismatch,
     * out-of-range or overlapping edits). This is a pure function: it never
     * touches the filesystem.
     */
    [[nodiscard]] static bool apply(const FilePatch& fp, const std::string& original,
                                    std::string& out, std::string& error);

    /*
     * Canonicalize `path` (resolving against `base_dir` when relative). Uses
     * realpath() when the path exists, falling back to a lexical normalization
     * otherwise. Exposed so the extension and the index agree on keys.
     */
    [[nodiscard]] static std::string canonicalize(const std::string& path,
                                                  const std::string& base_dir);

    /*
     * Re-key every indexed file through `fn` (and rebuild the basename filter).
     * The extension uses this to re-canonicalize the index with the *engine's*
     * path resolver (VCWD_REALPATH) after parse(), so the index keys match the
     * canonical paths PHP produces for includes. Keeping it here lets patch.cpp
     * stay free of any PHP/Zend dependency while the glue supplies the resolver.
     */
    void recanonicalize(const std::function<std::string(const std::string&)>& fn);

private:
    /* Parse our ed-script bundle format (see README). Selected automatically by
     * parse() when the input begins with the "# phpatcher-ed" magic line. */
    bool parse_ed_bundle(const std::string& diff_text, const std::string& base_dir,
                         std::string& error);

    /* Sort the index by path, reject duplicate targets, and (re)build the
     * sorted basename filter. Returns false and fills `error` on a duplicate. */
    bool finalize(std::string& error);

    std::vector<Entry> index_;            /* sorted by path after finalize()      */
    std::vector<std::string> basenames_;  /* basenames of targets, sorted, unique */
};

/* Read an entire file into `out`. Returns false on I/O error. */
[[nodiscard]] bool read_file(const std::string& path, std::string& out);

}  // namespace phpatcher

#endif  // PHPATCHER_PATCH_HPP
