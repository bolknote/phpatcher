/*
 * phpatcher matcher core — the pure block-factorization algorithm, decoupled
 * from the on-disk index format, the filesystem, and the matching strategy so
 * it can be unit-tested in isolation. Callers inject:
 *
 *   - anchor_keys[i]            -> the lookup key for block line i (its trimmed
 *                                  or php-normalized form); empty = never an
 *                                  anchor
 *   - postings_of(key)          -> the corpus occurrences of that key, or null
 *   - equal(block_pos, file, ln)-> whether block line block_pos matches corpus
 *                                  line `ln` of `file` under the caller's chosen
 *                                  notion of equality (raw bytes for a byte
 *                                  index, normalized keys for a normalized one)
 *
 * factorize() returns the maximal runs (sorted by block position); lines not
 * covered by any returned run are literals. It is agnostic to *what* "equal"
 * means — byte-exact precision or token-normalized matching is the caller's
 * decision, kept in one place so the same greedy algorithm serves both.
 */
#ifndef PHPATCHER_MATCHER_CORE_HPP
#define PHPATCHER_MATCHER_CORE_HPP

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string_view>
#include <vector>

namespace phpatcher {
namespace match {

struct Posting {
    std::uint32_t file_id;
    std::uint32_t lineno;  /* 1-based */
};

struct Run {
    std::size_t block_begin = 0;   /* [begin, end) in the block               */
    std::size_t block_end = 0;
    std::uint32_t file_id = 0;
    std::uint32_t corpus_begin = 0;  /* inclusive, 1-based corpus line range   */
    std::uint32_t corpus_end = 0;
    std::size_t length() const { return block_end - block_begin; }
};

using PostingsFn = std::function<const std::vector<Posting>*(std::string_view key)>;
using EqualFn = std::function<bool(std::size_t block_pos, std::uint32_t file_id,
                                   std::uint32_t lineno)>;

/* Trim horizontal whitespace (space/tab/CR/FF/VT) from both ends — the byte
 * index's notion of a key, also handy for callers building anchor keys. */
inline std::string_view trim(std::string_view s) {
    const auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

/*
 * Greedy rare-anchor factorization.
 *
 *   1. Anchors = block lines whose key is in the index, processed rarest-first
 *      (the most distinctive lines drive matching).
 *   2. For an uncovered anchor, try each corpus occurrence and extend up/down
 *      while `equal` holds. Extension can cross lines that are not indexed
 *      themselves — it asks `equal`, not the index.
 *   3. Keep the longest run; accept it if it reaches `min_run` lines and mark
 *      those positions covered so later anchors do not overlap it.
 */
inline std::vector<Run> factorize(const std::vector<std::string_view>& anchor_keys,
                                  std::size_t min_run,
                                  const PostingsFn& postings_of,
                                  const EqualFn& equal) {
    if (min_run == 0) min_run = 1;

    struct Anchor { std::size_t pos; std::size_t weight; };
    std::vector<Anchor> anchors;
    for (std::size_t i = 0; i < anchor_keys.size(); ++i) {
        if (const std::vector<Posting>* post = postings_of(anchor_keys[i])) {
            anchors.push_back({i, post->size()});
        }
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor& a, const Anchor& b) { return a.weight < b.weight; });

    std::vector<char> covered(anchor_keys.size(), 0);
    std::vector<Run> runs;

    for (const Anchor& anchor : anchors) {
        if (covered[anchor.pos]) continue;
        const std::vector<Posting>* post = postings_of(anchor_keys[anchor.pos]);
        if (post == nullptr) continue;

        Run best;
        bool found = false;
        for (const Posting& pp : *post) {
            if (!equal(anchor.pos, pp.file_id, pp.lineno)) continue;  /* verify the candidate */

            std::size_t b0 = anchor.pos, b1 = anchor.pos;
            std::uint32_t c0 = pp.lineno, c1 = pp.lineno;
            while (b0 > 0 && c0 > 1 && !covered[b0 - 1] &&
                   equal(b0 - 1, pp.file_id, c0 - 1)) { --b0; --c0; }
            while (b1 + 1 < anchor_keys.size() && !covered[b1 + 1] &&
                   equal(b1 + 1, pp.file_id, c1 + 1)) { ++b1; ++c1; }

            const std::size_t len = b1 - b0 + 1;
            if (!found || len > best.length()) {
                best = Run{b0, b1 + 1, pp.file_id, c0, c1};
                found = true;
            }
        }

        if (found && best.length() >= min_run) {
            for (std::size_t k = best.block_begin; k < best.block_end; ++k) covered[k] = 1;
            runs.push_back(best);
        }
    }

    std::sort(runs.begin(), runs.end(),
              [](const Run& a, const Run& b) { return a.block_begin < b.block_begin; });
    return runs;
}

}  // namespace match
}  // namespace phpatcher

#endif  // PHPATCHER_MATCHER_CORE_HPP
