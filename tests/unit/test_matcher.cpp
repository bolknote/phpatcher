/*
 * Unit tests for the matcher core (tools/matcher_core.hpp). No index file or
 * filesystem is involved: the corpus and its line index are built in memory and
 * fed to factorize() through the same accessor interface the CLI uses.
 */
#include "../../tools/matcher_core.hpp"

#include <cstdio>
#include <deque>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

using phpatcher::match::factorize;
using phpatcher::match::LineFn;
using phpatcher::match::Posting;
using phpatcher::match::PostingsFn;
using phpatcher::match::Run;
using phpatcher::match::trim;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do {                                                                  \
        ++g_checks;                                                       \
        auto _va = (a);                                                   \
        auto _vb = (b);                                                   \
        if (!(_va == _vb)) {                                              \
            ++g_failures;                                                 \
            std::fprintf(stderr, "  FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                 \
    } while (0)

namespace {

/* A list of lines that owns its bytes and exposes stable string_views. The
 * underlying strings are constructed once and never reallocated, so the views
 * remain valid for the lifetime of the holder. */
struct Lines {
    std::vector<std::string> owned;
    std::vector<std::string_view> views;

    Lines() = default;
    Lines(std::initializer_list<std::string> l) : owned(l) { rebuild(); }
    void rebuild() {
        views.clear();
        views.reserve(owned.size());
        for (const std::string& s : owned) views.push_back(s);
    }
};

/* In-memory corpus: a set of files (each a Lines) plus a posting list keyed by
 * trimmed line content. Tests choose explicitly which lines are "indexed" so we
 * can simulate the indexer's min-len / frequency filtering. */
class TestCorpus {
public:
    std::uint32_t add(std::initializer_list<std::string> lines) {
        const auto id = static_cast<std::uint32_t>(files_.size());
        files_.emplace_back(lines);
        return id;
    }

    void index_line(std::uint32_t file, std::uint32_t lineno) {
        const std::string key(trim(files_[file].owned[lineno - 1]));
        postings_[key].push_back({file, lineno});
    }
    void index_all(std::uint32_t file) {
        for (std::uint32_t i = 1; i <= files_[file].owned.size(); ++i) index_line(file, i);
    }

    PostingsFn postings_fn() const {
        return [this](std::string_view k) -> const std::vector<Posting>* {
            auto it = postings_.find(std::string(k));
            return it == postings_.end() ? nullptr : &it->second;
        };
    }
    LineFn line_fn() const {
        return [this](std::uint32_t f, std::uint32_t ln) -> const std::string_view* {
            if (f >= files_.size() || ln == 0 || ln > files_[f].views.size()) return nullptr;
            return &files_[f].views[ln - 1];
        };
    }

private:
    std::deque<Lines> files_;  /* deque: element addresses are stable on growth */
    std::unordered_map<std::string, std::vector<Posting>> postings_;
};

std::vector<Run> run(const TestCorpus& c, const Lines& block, std::size_t min_run) {
    return factorize(block.views, min_run, c.postings_fn(), c.line_fn());
}

}  // namespace

/* A block that is exactly one corpus line, with min_run = 1. */
static void test_single_line_match() {
    TestCorpus c;
    const std::uint32_t f = c.add({"alpha", "beta", "gamma"});
    c.index_all(f);
    const Lines block{"beta"};
    const std::vector<Run> runs = run(c, block, 1);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(1));
    if (runs.size() == 1) {
        CHECK_EQ(runs[0].block_begin, static_cast<std::size_t>(0));
        CHECK_EQ(runs[0].block_end, static_cast<std::size_t>(1));
        CHECK_EQ(runs[0].file_id, f);
        CHECK_EQ(runs[0].corpus_begin, 2u);
        CHECK_EQ(runs[0].corpus_end, 2u);
    }
}

/* A multi-line consecutive block collapses into one range reference. */
static void test_range_match() {
    TestCorpus c;
    const std::uint32_t f = c.add({"one", "two", "three", "four"});
    c.index_all(f);
    const Lines block{"two", "three", "four"};
    const std::vector<Run> runs = run(c, block, 2);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(1));
    if (runs.size() == 1) {
        CHECK_EQ(runs[0].length(), static_cast<std::size_t>(3));
        CHECK_EQ(runs[0].corpus_begin, 2u);
        CHECK_EQ(runs[0].corpus_end, 4u);
    }
}

/* A run shorter than --min-run is not emitted (stays literal). */
static void test_min_run_gate() {
    TestCorpus c;
    const std::uint32_t f = c.add({"one", "two", "three"});
    c.index_all(f);
    const Lines block{"two", "three"};   /* a 2-line run */
    CHECK_EQ(run(c, block, 3).size(), static_cast<std::size_t>(0));   /* needs >= 3 */
    CHECK_EQ(run(c, block, 2).size(), static_cast<std::size_t>(1));   /* exactly 2 ok */
}

/* Copy + novel literal + copy: two runs around the uncovered middle line. */
static void test_mixed_literals_and_copies() {
    TestCorpus c;
    const std::uint32_t f = c.add({"a-one", "a-two", "a-three", "a-four"});
    c.index_all(f);
    const Lines block{"a-one", "a-two", "NOVEL", "a-three", "a-four"};
    const std::vector<Run> runs = run(c, block, 2);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(2));
    if (runs.size() == 2) {
        CHECK_EQ(runs[0].block_begin, static_cast<std::size_t>(0));
        CHECK_EQ(runs[0].block_end, static_cast<std::size_t>(2));
        CHECK_EQ(runs[0].corpus_begin, 1u);
        CHECK_EQ(runs[0].corpus_end, 2u);
        CHECK_EQ(runs[1].block_begin, static_cast<std::size_t>(3));
        CHECK_EQ(runs[1].block_end, static_cast<std::size_t>(5));
        CHECK_EQ(runs[1].corpus_begin, 3u);
        CHECK_EQ(runs[1].corpus_end, 4u);
    }
}

/* Extension crosses a line that is NOT in the index: the anchor is a rare line
 * and the run grows by comparing real bytes, not index membership. */
static void test_extension_over_unindexed_line() {
    TestCorpus c;
    const std::uint32_t f = c.add({"rare-x", "common", "rare-y"});
    c.index_line(f, 1);  /* index only the two rare lines */
    c.index_line(f, 3);  /* "common" is deliberately left out of the index */
    const Lines block{"rare-x", "common", "rare-y"};
    const std::vector<Run> runs = run(c, block, 1);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(1));
    if (runs.size() == 1) {
        CHECK_EQ(runs[0].length(), static_cast<std::size_t>(3));
        CHECK_EQ(runs[0].corpus_begin, 1u);
        CHECK_EQ(runs[0].corpus_end, 3u);
    }
}

/* The index match is only a candidate: differing indentation (same trimmed
 * content) must NOT produce a reference, because emission is byte-exact. */
static void test_byte_exact_precision() {
    TestCorpus c;
    const std::uint32_t f = c.add({"    foo();"});  /* indented in the corpus */
    c.index_all(f);
    const Lines block{"foo();"};                    /* not indented in the block */
    /* Sanity: the trimmed forms DO match (so it is a candidate)... */
    CHECK(c.postings_fn()(trim(block.views[0])) != nullptr);
    /* ...but the raw bytes differ, so no run is emitted. */
    CHECK_EQ(run(c, block, 1).size(), static_cast<std::size_t>(0));
}

/* With two corpus occurrences of the anchor, the longest extension wins. */
static void test_longest_candidate_wins() {
    TestCorpus c;
    const std::uint32_t f0 = c.add({"k", "x"});
    const std::uint32_t f1 = c.add({"k", "y", "z"});
    c.index_line(f0, 1);
    c.index_line(f1, 1);
    const Lines block{"k", "y", "z"};
    const std::vector<Run> runs = run(c, block, 1);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(1));
    if (runs.size() == 1) {
        CHECK_EQ(runs[0].file_id, f1);            /* the 3-line match, not the 1-line one */
        CHECK_EQ(runs[0].length(), static_cast<std::size_t>(3));
    }
}

/* A block whose lines do not occur in the corpus yields no references. */
static void test_no_match() {
    TestCorpus c;
    const std::uint32_t f = c.add({"a", "b"});
    c.index_all(f);
    const Lines block{"zzz", "yyy"};
    CHECK_EQ(run(c, block, 1).size(), static_cast<std::size_t>(0));
}

/* Two distinct runs in one block, separated by a literal, do not overlap and
 * come back sorted by block position. */
static void test_two_nonoverlapping_runs() {
    TestCorpus c;
    const std::uint32_t f0 = c.add({"p1", "p2", "p3"});
    const std::uint32_t f1 = c.add({"q1", "q2", "q3"});
    c.index_all(f0);
    c.index_all(f1);
    const Lines block{"p1", "p2", "p3", "GAP", "q1", "q2", "q3"};
    const std::vector<Run> runs = run(c, block, 2);
    CHECK_EQ(runs.size(), static_cast<std::size_t>(2));
    if (runs.size() == 2) {
        CHECK_EQ(runs[0].file_id, f0);
        CHECK_EQ(runs[0].block_begin, static_cast<std::size_t>(0));
        CHECK_EQ(runs[0].block_end, static_cast<std::size_t>(3));
        CHECK_EQ(runs[1].file_id, f1);
        CHECK_EQ(runs[1].block_begin, static_cast<std::size_t>(4));
        CHECK_EQ(runs[1].block_end, static_cast<std::size_t>(7));
    }
}

/* An empty block is a no-op. */
static void test_empty_block() {
    TestCorpus c;
    const std::uint32_t f = c.add({"a"});
    c.index_all(f);
    const Lines block;  /* no lines */
    CHECK_EQ(run(c, block, 1).size(), static_cast<std::size_t>(0));
}

int main() {
    test_single_line_match();
    test_range_match();
    test_min_run_gate();
    test_mixed_literals_and_copies();
    test_extension_over_unindexed_line();
    test_byte_exact_precision();
    test_longest_candidate_wins();
    test_no_match();
    test_two_nonoverlapping_runs();
    test_empty_block();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
