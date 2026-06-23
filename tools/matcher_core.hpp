/*
 * phpatcher matcher core — the pure block-factorization algorithm, decoupled
 * from the on-disk index format and the filesystem so it can be unit-tested in
 * isolation. Callers (the CLI) inject two accessors:
 *
 *   - postings_of(trimmed_line)  -> the corpus occurrences of a line, or null
 *   - corpus_line(file_id, line) -> the raw bytes of a 1-based corpus line, or null
 *
 * factorize() returns the maximal verbatim runs (sorted by block position);
 * lines not covered by any returned run are literals.
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

using PostingsFn = std::function<const std::vector<Posting>*(std::string_view trimmed)>;
using LineFn = std::function<const std::string_view*(std::uint32_t file_id, std::uint32_t lineno)>;

/* Trim horizontal whitespace (space/tab/CR/FF/VT) from both ends — the same
 * normalization phpatcher-index applies to its keys. */
inline std::string_view trim(std::string_view s) {
    const auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
}

/*
 * Greedy rare-anchor factorization with byte-exact extension.
 *
 *   1. Anchors = block lines whose trimmed form is in the index, processed
 *      rarest-first (the most distinctive lines drive matching).
 *   2. For an uncovered anchor, try each corpus occurrence and extend up/down
 *      while the block line equals the corpus line *byte for byte* (the index
 *      hit is only a candidate; emission must be exact because a reference
 *      reproduces the original bytes verbatim). Extension can cross lines that
 *      are not indexed themselves — it compares real bytes, not the index.
 *   3. Keep the longest run; accept it if it reaches `min_run` lines and mark
 *      those positions covered so later anchors do not overlap it.
 */
inline std::vector<Run> factorize(const std::vector<std::string_view>& block,
                                  std::size_t min_run,
                                  const PostingsFn& postings_of,
                                  const LineFn& corpus_line) {
    if (min_run == 0) min_run = 1;

    struct Anchor { std::size_t pos; std::size_t weight; };
    std::vector<Anchor> anchors;
    for (std::size_t i = 0; i < block.size(); ++i) {
        if (const std::vector<Posting>* post = postings_of(trim(block[i]))) {
            anchors.push_back({i, post->size()});
        }
    }
    std::sort(anchors.begin(), anchors.end(),
              [](const Anchor& a, const Anchor& b) { return a.weight < b.weight; });

    std::vector<char> covered(block.size(), 0);
    std::vector<Run> runs;

    const auto raw_equal = [&](std::size_t bpos, std::uint32_t file_id, std::uint32_t lineno) {
        const std::string_view* cl = corpus_line(file_id, lineno);
        return cl != nullptr && *cl == block[bpos];
    };

    for (const Anchor& anchor : anchors) {
        if (covered[anchor.pos]) continue;
        const std::vector<Posting>* post = postings_of(trim(block[anchor.pos]));
        if (post == nullptr) continue;

        Run best;
        bool found = false;
        for (const Posting& pp : *post) {
            if (!raw_equal(anchor.pos, pp.file_id, pp.lineno)) continue;  /* anchor must match exactly */

            std::size_t b0 = anchor.pos, b1 = anchor.pos;
            std::uint32_t c0 = pp.lineno, c1 = pp.lineno;
            while (b0 > 0 && c0 > 1 && !covered[b0 - 1] &&
                   raw_equal(b0 - 1, pp.file_id, c0 - 1)) { --b0; --c0; }
            while (b1 + 1 < block.size() && !covered[b1 + 1] &&
                   raw_equal(b1 + 1, pp.file_id, c1 + 1)) { ++b1; ++c1; }

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
