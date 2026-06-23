/*
 * phpatcher - in-memory phpatcher-ed patching core.
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
#include <variant>
#include <vector>

namespace phpatcher {

/* A 128-bit content hash (two independent FNV-1a-style lanes). Shared verbatim
 * by the corpus-reference generator (tools/) and the apply path so both compute
 * the same value. It is a drift guard for references, not a security mechanism. */
struct RefHash {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    bool operator==(const RefHash& o) const { return a == o.a && b == o.b; }
    bool operator!=(const RefHash& o) const { return !(*this == o); }
};

inline RefHash ref_hash(std::string_view s) {
    std::uint64_t h1 = 14695981039346656037ull;
    std::uint64_t h2 = 1469598103934665603ull;
    for (char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        h1 = (h1 ^ c) * 1099511628211ull;
        h2 = (h2 ^ c) * 0x9E3779B97F4A7C15ull;
    }
    const auto n = static_cast<std::uint64_t>(s.size());
    h1 = (h1 ^ n) * 1099511628211ull;
    h2 = (h2 ^ n) * 0x9E3779B97F4A7C15ull;
    return {h1, h2};
}

/* Lower-case, fixed-width 32-hex-digit rendering of a RefHash (lane a, then b). */
inline std::string ref_hash_hex(RefHash h) {
    static const char digits[] = "0123456789abcdef";
    std::string out(32, '0');
    for (std::size_t i = 0; i < 16; ++i) {
        const auto shift = static_cast<unsigned int>(i * 4);
        const auto a = static_cast<std::size_t>((h.a >> shift) & 0xfu);
        const auto b = static_cast<std::size_t>((h.b >> shift) & 0xfu);
        out[15u - i] = digits[a];
        out[31u - i] = digits[b];
    }
    return out;
}

/* Inverse of ref_hash_hex. Returns false on a malformed (non 32-hex) string. */
inline bool ref_hash_parse(std::string_view hex, RefHash& out) {
    if (hex.size() != 32) return false;
    std::uint64_t a = 0, b = 0;
    for (std::size_t i = 0; i < 32; ++i) {
        const char c = hex[i];
        std::uint64_t v;
        if (c >= '0' && c <= '9') v = static_cast<std::uint64_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<std::uint64_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = static_cast<std::uint64_t>(c - 'A' + 10);
        else return false;
        if (i < 16) a = (a << 4) | v; else b = (b << 4) | v;
    }
    out = {a, b};
    return true;
}

/*
 * A reference to a run of lines that already exists verbatim in the original
 * source tree, used in place of re-quoting that text inside the patch (a move or
 * copy). It is resolved at apply time against the on-disk file and guarded
 * against the deployed tree having drifted from the one the patch was generated
 * against.
 *
 * The guard is the exact byte length (`s:`, the cheap default) and/or a 128-bit
 * content hash (`h:`, opt-in for the paranoid). At least one must be present;
 * when both are, both are verified. `bytes >= 0` indicates an `s:` guard and
 * `has_hash` an `h:` guard.
 */
struct EdRef {
    enum Transform : std::uint8_t {
        Exact,      /* insert resolved lines unchanged                       */
        TrimPad     /* insert lpad + trim(resolved line) + rpad per line     */
    };

    std::string path;        /* file path, relative to the patch base_dir       */
    std::int64_t begin = 0;  /* inclusive, 1-based first line                    */
    std::int64_t end = 0;    /* inclusive, 1-based last line                     */
    std::int64_t bytes = -1; /* expected byte length of the run (s:); <0 if none */
    bool has_hash = false;   /* whether `hash` carries an expected value (h:)    */
    RefHash hash;            /* expected content hash, valid iff has_hash        */
    Transform transform = Exact;
    std::string lpad;        /* used by TrimPad                                  */
    std::string rpad;        /* used by TrimPad                                  */
};

/* One element of a phpatcher-ed input block: either a literal line or a corpus
 * reference that expands to one or more lines. */
using EdPiece = std::variant<std::string, EdRef>;

/*
 * A single phpatcher-ed editing command.
 *
 * phpatcher-ed borrows the small line-addressed command set from ed (`a`, `c`,
 * `d`, `i`) but extends input blocks with typed pieces such as corpus
 * references. Generators emit commands in descending line order so they can be
 * applied sequentially without line numbers shifting.
 */
struct EdCommand {
    enum Kind {
        Delete,  /* "M,Nd"  - remove lines M..N                       */
        Change,  /* "M,Nc"  - replace lines M..N with `input`         */
        Append,  /* "Na"    - insert `input` after line N (N may be 0)*/
        Insert   /* "Ni"    - insert `input` before line N            */
    };

    Kind kind = Delete;
    std::int64_t start = 0;  /* 1-based first affected line (or "after" line for Append) */
    std::int64_t end = 0;    /* 1-based last affected line (ranges only)                 */
    std::vector<EdPiece> input;  /* replacement/inserted content: literal lines and/or refs */
};

/*
 * All edits targeting one concrete file. phpatcher intentionally supports only
 * its source-hiding phpatcher-ed format, so a file patch is simply a list of
 * line-addressed commands.
 */
struct FilePatch {
    std::vector<EdCommand> commands;
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
     * Parse a phpatcher-ed patch file. `base_dir` resolves `# file:` paths into
     * absolute, canonical filesystem paths. Replaces any previously parsed content.
     * Returns false and fills `error` on a hard parse error (including a file
     * targeted by more than one section); an empty patch is a successful no-op.
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
     * Resolves a corpus reference to its concrete lines (each EOL-stripped, in
     * file order). Returns false and fills `error` if the file/range is
     * unavailable or its content hash does not match `ref.hash`. Injected by the
     * caller so patch.cpp stays free of any filesystem/PHP policy.
     */
    using RefResolver =
        std::function<bool(const EdRef& ref, std::vector<std::string>& lines, std::string& error)>;

    /*
     * Apply `fp` to `original`, producing `out`. Returns false and fills
     * `error` if the patch does not cleanly apply: an address out of range, or
     * commands that are not in strictly descending, non-overlapping line order
     * (the order phpatcher's generators always emit, required so sequential
     * application matches the original line numbers). apply() itself never
     * touches the filesystem; a patch that contains corpus references needs a
     * `resolve` callback to expand them, and without one such a reference is an
     * error.
     */
    [[nodiscard]] static bool apply(const FilePatch& fp, const std::string& original,
                                    std::string& out, std::string& error,
                                    const RefResolver& resolve = {});

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
    /* Parse our phpatcher-ed bundle format (see README). Selected automatically by
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
