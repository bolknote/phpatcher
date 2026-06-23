/*
 * phpatcher-match — find blocks of code that already exist verbatim in the
 * corpus, so a patch can reference them instead of re-printing the text.
 *
 * Given a block of lines (a file, or stdin) and a corpus index built by
 * phpatcher-index, it factorizes the block into:
 *
 *   - COPY references: a maximal run of consecutive lines that occurs verbatim
 *     somewhere in the corpus, emitted as `!sed -n 'A,B p' <file>` plus a drift
 *     guard — the exact byte length (`s:`, default) or, with --hash, a 128-bit
 *     content hash (`h:`); and
 *   - LITERAL lines: everything not covered by a long-enough copy.
 *
 * Algorithm (greedy, rare-anchor + byte-exact extension):
 *
 *   1. Anchors are block lines present in the index (i.e. distinctive: not too
 *      short, not too common — the index already dropped those). Process anchors
 *      rarest-first (fewest corpus occurrences = most distinctive).
 *   2. For an uncovered anchor, try each corpus occurrence and extend the match
 *      up and down as long as the block line equals the corpus line *byte for
 *      byte* (the index match is only a candidate; correctness is decided here,
 *      because `sed` copies the original bytes verbatim — including indentation).
 *   3. Keep the longest run; if it reaches --min-run lines, accept it and mark
 *      those block lines covered. Remaining lines stay literal.
 *
 * Recall vs precision: the index keys are whitespace-trimmed (so differently
 * indented copies are still *candidates*), but a reference is only emitted when
 * the bytes match exactly. A re-indented move therefore falls back to literals
 * in this version — a deliberate, safe limitation.
 *
 * Build:  c++ -std=c++17 -O2 -o tools/phpatcher-match tools/matcher.cpp
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matcher_core.hpp"
#include "../patch.hpp"  /* ref_hash / ref_hash_hex: one source of truth with apply */

namespace fs = std::filesystem;

namespace {

using phpatcher::match::Posting;
using phpatcher::match::Run;

constexpr char kMagic[4] = {'P', 'H', 'I', 'X'};

[[noreturn]] void die(const std::string &msg) {
    std::fprintf(stderr, "phpatcher-match: %s\n", msg.c_str());
    std::exit(2);
}

void usage(const char *argv0) {
    std::fprintf(stderr,
        "phpatcher-match — factorize a block into corpus copy-references + literals\n\n"
        "Usage:\n"
        "  %s [options] <index> [block-file]\n\n"
        "Arguments:\n"
        "  index                binary index from phpatcher-index\n"
        "  block-file           file whose lines to factorize (default: stdin)\n\n"
        "Options:\n"
        "  -r, --root DIR       corpus root to resolve referenced files (default: .)\n"
        "  -n, --min-run N      minimum run length to emit a reference (default: 3)\n"
        "  -H, --hash           guard references with a content hash (h:) instead\n"
        "                       of the byte length (s:, the default)\n"
        "  -v, --verbose        print coverage statistics to stderr\n"
        "  -h, --help           show this help\n",
        argv0);
}

bool read_file(const fs::path &path, std::string &out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        f.seekg(0);
        f.read(out.data(), size);
        if (f.bad()) return false;
        out.resize(static_cast<std::size_t>(f.gcount()));
        return true;
    }
    out.clear();
    return true;
}

/* Split into lines with the trailing newline (and a single preceding CR)
 * removed; the line count and numbering match the on-disk file. */
std::vector<std::string_view> split_lines(std::string_view content) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            std::size_t end = i;
            if (end > start && content[end - 1] == '\r') --end;
            lines.push_back(content.substr(start, end - start));
            start = i + 1;
        }
    }
    if (start < content.size()) lines.push_back(content.substr(start));
    return lines;
}

/* The corpus index, loaded into memory. Keys (string_views) and posting arrays
 * point into `blob`, which is kept alive for the lifetime of the Index. */
class Index {
public:
    void load(const std::string &path);

    const std::vector<Posting> *find(std::string_view key) const {
        auto it = lines_.find(key);
        return it == lines_.end() ? nullptr : &it->second;
    }
    std::string_view file(std::uint32_t id) const { return files_.at(id); }
    std::size_t file_count() const { return files_.size(); }
    std::size_t key_count() const { return lines_.size(); }

private:
    std::string blob_;
    std::vector<std::string_view> files_;
    std::unordered_map<std::string_view, std::vector<Posting>> lines_;
};

void Index::load(const std::string &path) {
    if (!read_file(path, blob_)) die("cannot read index: " + path);
    const char *p = blob_.data();
    const char *end = p + blob_.size();

    const auto need = [&](std::size_t n) {
        if (static_cast<std::size_t>(end - p) < n) die("index file is truncated or corrupt");
    };
    const auto u16 = [&]() -> std::uint16_t {
        need(2); std::uint16_t v; std::memcpy(&v, p, 2); p += 2; return v;
    };
    const auto u32 = [&]() -> std::uint32_t {
        need(4); std::uint32_t v; std::memcpy(&v, p, 4); p += 4; return v;
    };

    need(4);
    if (std::memcmp(p, kMagic, 4) != 0) die("not a phpatcher index (bad magic)");
    p += 4;
    (void)u32();  /* version */
    (void)u32();  /* min_len */
    (void)u32();  /* max_postings */

    const std::uint32_t file_count = u32();
    files_.reserve(file_count);
    for (std::uint32_t i = 0; i < file_count; ++i) {
        const std::uint16_t len = u16();
        need(len);
        files_.emplace_back(p, len);
        p += len;
    }

    const std::uint32_t key_count = u32();
    lines_.reserve(key_count * 2);
    for (std::uint32_t i = 0; i < key_count; ++i) {
        const std::uint32_t klen = u32();
        need(klen);
        std::string_view key(p, klen);
        p += klen;
        const std::uint32_t n = u32();
        std::vector<Posting> postings;
        postings.reserve(n);
        for (std::uint32_t j = 0; j < n; ++j) {
            Posting pp;
            pp.file_id = u32();
            pp.lineno = u32();
            postings.push_back(pp);
        }
        lines_.emplace(key, std::move(postings));
    }
}

/* On-demand cache of corpus files, split into lines (EOL stripped) for
 * byte-exact extension. A failed load is cached as an empty-but-present entry. */
class Corpus {
public:
    explicit Corpus(fs::path root) : root_(std::move(root)) {}

    /* Returns the raw line (1-based) or nullptr if unavailable. */
    const std::string_view *line(const Index &idx, std::uint32_t file_id, std::uint32_t lineno) {
        Loaded &lf = get(idx, file_id);
        if (!lf.ok || lineno == 0 || lineno > lf.lines.size()) return nullptr;
        return &lf.lines[lineno - 1];
    }

private:
    struct Loaded {
        bool ok = false;
        std::string content;
        std::vector<std::string_view> lines;
    };

    Loaded &get(const Index &idx, std::uint32_t file_id) {
        auto it = cache_.find(file_id);
        if (it != cache_.end()) return it->second;
        Loaded lf;
        const fs::path path = root_ / fs::path(std::string(idx.file(file_id)));
        if (read_file(path, lf.content)) {
            lf.ok = true;
            lf.lines = split_lines(lf.content);
        }
        return cache_.emplace(file_id, std::move(lf)).first->second;
    }

    fs::path root_;
    std::unordered_map<std::uint32_t, Loaded> cache_;
};

struct Options {
    std::string index;
    std::string block_file;  /* empty = stdin */
    std::string root = ".";
    std::size_t min_run = 3;
    bool verbose = false;
    bool hash = false;  /* emit a content hash (h:) instead of the length (s:) */
};

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) die("missing value for " + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "-r" || a == "--root") opt.root = next(a);
        else if (a == "-n" || a == "--min-run") {
            const std::string v = next(a);
            try {
                std::size_t pos = 0;
                opt.min_run = std::stoul(v, &pos);
                if (pos != v.size()) throw std::invalid_argument("trailing");
            } catch (const std::exception &) {
                die(a + " expects a positive integer, got '" + v + "'");
            }
            if (opt.min_run == 0) opt.min_run = 1;
        }
        else if (a == "-v" || a == "--verbose") opt.verbose = true;
        else if (a == "-H" || a == "--hash") opt.hash = true;
        else if (!a.empty() && a[0] == '-' && a != "-") die("unknown option: " + a);
        else positional.push_back(a);
    }
    if (positional.empty()) { usage(argv[0]); die("missing <index> argument"); }
    opt.index = positional[0];
    if (positional.size() > 1) opt.block_file = positional[1];
    if (positional.size() > 2) die("too many arguments");

    Index idx;
    idx.load(opt.index);

    std::string block_content;
    if (opt.block_file.empty()) {
        std::string chunk(1 << 16, '\0');
        std::size_t got;
        while ((got = std::fread(chunk.data(), 1, chunk.size(), stdin)) > 0) {
            block_content.append(chunk.data(), got);
        }
    } else if (!read_file(opt.block_file, block_content)) {
        die("cannot read block file: " + opt.block_file);
    }

    const std::vector<std::string_view> block = split_lines(block_content);
    Corpus corpus(opt.root);

    /* The factorization (runs are returned sorted by block position). */
    const std::vector<Run> runs = phpatcher::match::factorize(
        block, opt.min_run,
        [&](std::string_view key) { return idx.find(key); },
        [&](std::uint32_t file_id, std::uint32_t lineno) {
            return corpus.line(idx, file_id, lineno);
        });

    /* Anchor count, for the summary only. */
    std::size_t anchor_count = 0;
    for (const std::string_view &line : block) {
        if (idx.find(phpatcher::match::trim(line))) ++anchor_count;
    }

    std::size_t covered_lines = 0;
    std::size_t pos = 0, run_idx = 0;
    while (pos < block.size()) {
        if (run_idx < runs.size() && runs[run_idx].block_begin == pos) {
            const Run &r = runs[run_idx++];
            /* The exact bytes the reference reproduces (each line + '\n'): used
             * for the length (s:) or, when requested, the content hash (h:). */
            std::string bytes;
            std::size_t len = 0;
            for (std::uint32_t ln = r.corpus_begin; ln <= r.corpus_end; ++ln) {
                if (const std::string_view *cl = corpus.line(idx, r.file_id, ln)) {
                    len += cl->size() + 1;
                    if (opt.hash) {
                        bytes.append(cl->data(), cl->size());
                        bytes.push_back('\n');
                    }
                }
            }
            const std::string guard = opt.hash
                ? "h:" + phpatcher::ref_hash_hex(phpatcher::ref_hash(bytes))
                : "s:" + std::to_string(len);
            /* The ';B q' makes the printed command quit after the last copied
             * line, so a human pasting it into a shell does not scan the rest of
             * a large file. phpatcher itself never runs sed — it slices the file
             * directly — so the suffix is purely for shell ergonomics. */
            std::printf("!sed -n '%u,%u p;%u q' %.*s  # %s\n",
                        r.corpus_begin, r.corpus_end, r.corpus_end,
                        static_cast<int>(idx.file(r.file_id).size()), idx.file(r.file_id).data(),
                        guard.c_str());
            covered_lines += r.length();
            pos = r.block_end;
        } else {
            std::printf("%.*s\n", static_cast<int>(block[pos].size()), block[pos].data());
            ++pos;
        }
    }

    if (opt.verbose) {
        const std::size_t literal_lines = block.size() - covered_lines;
        const double pct = block.empty() ? 0.0 : 100.0 * covered_lines / block.size();
        std::fprintf(stderr,
            "\nphpatcher-match summary\n"
            "  index            %s (%zu files, %zu keys)\n"
            "  block lines      %zu\n"
            "  anchors          %zu\n"
            "  copy references  %zu (%zu lines, %.1f%% covered)\n"
            "  literal lines    %zu\n",
            opt.index.c_str(), idx.file_count(), idx.key_count(),
            block.size(), anchor_count, runs.size(), covered_lines, pct, literal_lines);
    }

    return 0;
}
