/*
 * phpatcher - in-memory unified-diff patching core.
 *
 * This translation unit is intentionally free of any PHP/Zend dependency so
 * that it can be unit-tested in isolation and reused outside of the extension.
 */
#ifndef PHPATCHER_PATCH_HPP
#define PHPATCHER_PATCH_HPP

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phpatcher {

/* A single contiguous change region within a unified diff. */
struct Hunk {
    long orig_start = 0;  /* 1-based first line of the hunk in the original file */
    long orig_count = 0;  /* number of original lines covered by the hunk       */
    long new_start = 0;   /* 1-based first line of the hunk in the patched file  */
    long new_count = 0;   /* number of patched lines produced by the hunk       */

    /* Raw body lines, each still carrying its leading marker:
     *   ' ' context, '-' removal, '+' addition, '\' no-newline-at-eof note. */
    std::vector<std::string> lines;
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
    long start = 0;  /* 1-based first affected line (or the "after" line for Append) */
    long end = 0;    /* 1-based last affected line (ranges only)                     */
    std::vector<std::string> lines;  /* replacement/inserted content (no markers)   */
};

/* All edits targeting one concrete file. A file patch is expressed either as
 * unified-diff hunks or as an ed script, never both. */
struct FilePatch {
    std::string target_path;        /* canonical absolute path of the file      */
    std::string diff_old_path;      /* path as written after "--- "             */
    std::string diff_new_path;      /* path as written after "+++ "             */
    bool is_ed = false;             /* true => use `ed`, false => use `hunks`    */
    std::vector<Hunk> hunks;        /* unified-diff representation               */
    std::vector<EdCommand> ed;      /* ed-script representation                  */
};

/*
 * Parsed collection of file patches, indexed by canonical absolute path for
 * O(1) average lookup. The structure is immutable after parse() and therefore
 * safe to share read-only across threads.
 */
class PatchSet {
public:
    /*
     * Parse a patch file (unified diff or ed-script bundle; auto-detected).
     * `base_dir` resolves the diff's relative a/ b/ paths into absolute,
     * canonical filesystem paths. Replaces any previously parsed content.
     * Returns false and fills `error` on a hard parse error; an empty or
     * hunk-less patch is a successful no-op.
     */
    bool parse(const std::string& diff_text, const std::string& base_dir, std::string& error);

    /* Look up the patch for a canonical absolute path, or nullptr. */
    [[nodiscard]] const FilePatch* find(const std::string& canonical_path) const;

    [[nodiscard]] std::size_t file_count() const { return index_.size(); }

    [[nodiscard]] const std::unordered_map<std::string, FilePatch>& files() const {
        return index_;
    }

    /*
     * O(1) negative pre-filter for the compile hot path: returns true only if
     * some indexed file shares this basename. The compile hook uses it to skip
     * the (filesystem-touching) path canonicalization for the overwhelming
     * majority of files that are not patched, without any syscall.
     */
    [[nodiscard]] bool has_basename(const std::string& basename) const {
        return basenames_.find(basename) != basenames_.end();
    }

    /* Return the final path component of `path` (text after the last '/'). */
    [[nodiscard]] static std::string basename_of(const std::string& path);

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

    /* Insert a fully-built file patch into the index and basename filter.
     * Returns false (and fills `error`) if another section already targets the
     * same canonical path, so a duplicate never silently overwrites the first. */
    bool add_file(FilePatch&& fp, std::string& error);

    std::unordered_map<std::string, FilePatch> index_;
    std::unordered_set<std::string> basenames_;  /* basenames of indexed targets */
};

/* Read an entire file into `out`. Returns false on I/O error. */
[[nodiscard]] bool read_file(const std::string& path, std::string& out);

}  // namespace phpatcher

#endif  // PHPATCHER_PATCH_HPP
