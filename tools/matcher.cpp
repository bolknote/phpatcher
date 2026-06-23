/*
 * phpatcher-match — find blocks of code that already exist in the corpus, so a
 * patch can reference them instead of re-printing the text.
 *
 * Given a block of lines (a file, or stdin) and a corpus index built by
 * phpatcher-index, it factorizes the block into:
 *
 *   - COPY references: a run of consecutive lines that can be reproduced from
 *     the corpus, emitted as native `r "file" A B` directives plus a drift guard
 *     — exact byte length (`s:`, default) or a 128-bit content hash (`h:`); and
 *   - LITERAL lines: everything not covered by a long-enough copy.
 *
 * Algorithm (greedy, rare-anchor + caller-defined equality):
 *
 *   1. Anchors are block lines present in the index (i.e. distinctive: not too
 *      short, not too common — the index already dropped those). Process anchors
 *      rarest-first (fewest corpus occurrences = most distinctive).
 *   2. For an uncovered anchor, try each corpus occurrence and extend the match
 *      up and down as long as the equality predicate holds. Byte indexes use raw
 *      byte equality; normalized indexes use PHP-token-normalized keys.
 *   3. Keep the longest run; if it reaches --min-run lines, accept it and mark
 *      those block lines covered. Remaining lines stay literal.
 *
 * Precision: even in normalized mode, output is emitted only when an exact `r`
 * copy or a trim+pad `r` transform reproduces the destination bytes. Lines that
 * match semantically but cannot be represented safely stay literal.
 *
 * Build:  c++ -std=c++17 -O2 -o tools/phpatcher-match tools/matcher.cpp
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matcher_core.hpp"
#include "normalizer.hpp"
#include "../patch.hpp"  /* ref_hash / ref_hash_hex: one source of truth with apply */

namespace fs = std::filesystem;

namespace {

using phpatcher::match::Posting;
using phpatcher::match::Run;
using phpatcher::tools::NormLine;
using phpatcher::tools::Normalizer;

constexpr char kMagic[4] = {'P', 'H', 'I', 'X'};
constexpr std::uint32_t kFlagNormalized = 1u << 0;

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
        "      --php PATH        php interpreter for normalized indexes (default: php)\n"
        "      --normalizer PATH path to normalize.php (default: next to this binary)\n"
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

std::vector<std::string> normalize_block_keys(Normalizer &normalizer,
                                              std::string_view block_content) {
    const bool has_open_tag = block_content.size() >= 2 &&
        block_content[0] == '<' && block_content[1] == '?';

    std::string wrapped;
    std::string_view input = block_content;
    std::size_t skip = 0;
    if (!has_open_tag) {
        wrapped = "<?php\n";
        wrapped.append(block_content.data(), block_content.size());
        input = wrapped;
        skip = 1;  /* drop the artificial open-tag line */
    }

    std::vector<NormLine> norm = normalizer.run(input);
    std::vector<std::string> keys;
    if (norm.size() > skip) keys.reserve(norm.size() - skip);
    for (std::size_t i = skip; i < norm.size(); ++i) {
        keys.push_back(std::move(norm[i].key));
    }
    return keys;
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
    bool normalized() const { return normalized_; }

private:
    std::string blob_;
    std::vector<std::string_view> files_;
    std::unordered_map<std::string_view, std::vector<Posting>> lines_;
    bool normalized_ = false;
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
    const std::uint32_t version = u32();
    if (version != 1 && version != 2) die("unsupported index version: " + std::to_string(version));
    std::uint32_t flags = 0;
    if (version >= 2) flags = u32();
    normalized_ = (flags & kFlagNormalized) != 0;
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

/* On-demand cache of corpus files, split into raw lines (EOL stripped) and, for
 * normalized indexes, normalized keys. A failed load is cached as an
 * empty-but-present entry. */
class Corpus {
public:
    explicit Corpus(fs::path root, Normalizer *normalizer = nullptr)
        : root_(std::move(root)), normalizer_(normalizer) {}

    /* Returns the raw line (1-based) or nullptr if unavailable. */
    const std::string_view *line(const Index &idx, std::uint32_t file_id, std::uint32_t lineno) {
        Loaded &lf = get(idx, file_id);
        if (!lf.ok || lineno == 0 || lineno > lf.lines.size()) return nullptr;
        return &lf.lines[lineno - 1];
    }

    const std::string_view *key(const Index &idx, std::uint32_t file_id, std::uint32_t lineno) {
        Loaded &lf = get(idx, file_id);
        if (!lf.ok || lineno == 0 || lineno > lf.key_views.size()) return nullptr;
        return &lf.key_views[lineno - 1];
    }

private:
    struct Loaded {
        bool ok = false;
        std::string content;
        std::vector<std::string_view> lines;
        std::vector<std::string> keys;
        std::vector<std::string_view> key_views;
    };

    Loaded &get(const Index &idx, std::uint32_t file_id) {
        auto it = cache_.find(file_id);
        if (it != cache_.end()) return it->second;
        Loaded lf;
        const fs::path path = root_ / fs::path(std::string(idx.file(file_id)));
        if (read_file(path, lf.content)) {
            lf.ok = true;
            lf.lines = split_lines(lf.content);
            if (normalizer_ != nullptr) {
                std::vector<NormLine> norm = normalizer_->run(lf.content);
                lf.keys.reserve(norm.size());
                lf.key_views.reserve(norm.size());
                for (NormLine &nl : norm) {
                    lf.keys.push_back(std::move(nl.key));
                }
                for (const std::string &k : lf.keys) lf.key_views.push_back(k);
            }
        }
        return cache_.emplace(file_id, std::move(lf)).first->second;
    }

    fs::path root_;
    Normalizer *normalizer_;
    std::unordered_map<std::uint32_t, Loaded> cache_;
};

struct Options {
    std::string index;
    std::string block_file;  /* empty = stdin */
    std::string root = ".";
    std::size_t min_run = 3;
    bool verbose = false;
    bool hash = false;  /* emit a content hash (h:) instead of the length (s:) */
    std::string php = "php";
    std::string normalizer;  /* path to normalize.php (default: next to argv0) */
};

std::string quote_ed(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            case '\n': out += "\\n"; break;
            default:   out.push_back(c); break;
        }
    }
    out += '"';
    return out;
}

bool ref_bytes(Corpus &corpus, const Index &idx, std::uint32_t file_id,
               std::uint32_t begin, std::uint32_t end,
               std::string &bytes, std::size_t &len) {
    bytes.clear();
    len = 0;
    for (std::uint32_t ln = begin; ln <= end; ++ln) {
        const std::string_view *cl = corpus.line(idx, file_id, ln);
        if (cl == nullptr) return false;
        len += cl->size() + 1;
        bytes.append(cl->data(), cl->size());
        bytes.push_back('\n');
    }
    return true;
}

std::string make_guard(Corpus &corpus, const Index &idx, const Options &opt,
                       std::uint32_t file_id, std::uint32_t begin, std::uint32_t end) {
    std::string bytes;
    std::size_t len = 0;
    if (!ref_bytes(corpus, idx, file_id, begin, end, bytes, len)) return {};
    return opt.hash
        ? "h:" + phpatcher::ref_hash_hex(phpatcher::ref_hash(bytes))
        : "s:" + std::to_string(len);
}

bool trim_pad_for(std::string_view src, std::string_view dst,
                  std::string_view &lpad, std::string_view &rpad) {
    const std::string_view core = phpatcher::match::trim(src);
    if (core.empty()) return false;
    const std::string_view dtrim = phpatcher::match::trim(dst);
    if (dtrim != core) return false;
    const std::size_t off = static_cast<std::size_t>(dtrim.data() - dst.data());
    lpad = dst.substr(0, off);
    rpad = dst.substr(off + dtrim.size());
    return true;
}

bool emit_ref(Corpus &corpus, const Index &idx, const Options &opt,
              std::uint32_t file_id, std::uint32_t begin, std::uint32_t end,
              bool trim_pad = false, std::string_view lpad = {}, std::string_view rpad = {}) {
    const std::string guard = make_guard(corpus, idx, opt, file_id, begin, end);
    if (guard.empty()) return false;

    const std::string path = quote_ed(idx.file(file_id));
    if (trim_pad) {
        std::printf("r %s %u %u %s %s # %s\n", path.c_str(), begin, end,
                    quote_ed(lpad).c_str(), quote_ed(rpad).c_str(), guard.c_str());
    } else {
        std::printf("r %s %u %u # %s\n", path.c_str(), begin, end, guard.c_str());
    }
    return true;
}

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
        else if (a == "--php") opt.php = next(a);
        else if (a == "--normalizer") opt.normalizer = next(a);
        else if (!a.empty() && a[0] == '-' && a != "-") die("unknown option: " + a);
        else positional.push_back(a);
    }
    if (positional.empty()) { usage(argv[0]); die("missing <index> argument"); }
    opt.index = positional[0];
    if (positional.size() > 1) opt.block_file = positional[1];
    if (positional.size() > 2) die("too many arguments");

    Index idx;
    idx.load(opt.index);
    if (idx.normalized()) {
        opt.hash = true;  /* normalized references must guard raw bytes, not just length */
    }

    Normalizer normalizer;
    if (idx.normalized()) {
        if (opt.normalizer.empty()) {
            fs::path self(argv[0]);
            fs::path dir = self.parent_path();
            opt.normalizer = (dir.empty() ? fs::path("normalize.php")
                                          : dir / "normalize.php").string();
        }
        if (!fs::exists(opt.normalizer)) {
            die("normalizer script not found: " + opt.normalizer +
                " (pass --normalizer PATH)");
        }
        normalizer.start(opt.php, opt.normalizer);
    }

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
    std::vector<std::string> block_key_storage;
    std::vector<std::string_view> block_keys;
    if (idx.normalized()) {
        block_key_storage = normalize_block_keys(normalizer, block_content);
        block_keys.reserve(block_key_storage.size());
        for (const std::string &k : block_key_storage) block_keys.push_back(k);
    } else {
        block_keys.reserve(block.size());
        for (std::string_view line : block) block_keys.push_back(phpatcher::match::trim(line));
    }
    Corpus corpus(opt.root, idx.normalized() ? &normalizer : nullptr);

    /* The factorization (runs are returned sorted by block position). */
    const std::vector<Run> runs = phpatcher::match::factorize(
        block_keys, opt.min_run,
        [&](std::string_view key) { return idx.find(key); },
        [&](std::size_t bpos, std::uint32_t file_id, std::uint32_t lineno) {
            if (idx.normalized()) {
                const std::string_view *ck = corpus.key(idx, file_id, lineno);
                return ck != nullptr && *ck == block_keys[bpos];
            }
            const std::string_view *cl = corpus.line(idx, file_id, lineno);
            return cl != nullptr && *cl == block[bpos];
        });

    /* Anchor count, for the summary only. */
    std::size_t anchor_count = 0;
    for (const std::string_view &key : block_keys) {
        if (idx.find(key)) ++anchor_count;
    }

    std::size_t covered_lines = 0, emitted_refs = 0;
    std::size_t pos = 0, run_idx = 0;
    while (pos < block.size()) {
        if (run_idx < runs.size() && runs[run_idx].block_begin == pos) {
            const Run &r = runs[run_idx++];
            if (!idx.normalized()) {
                if (emit_ref(corpus, idx, opt, r.file_id, r.corpus_begin, r.corpus_end, false)) {
                    covered_lines += r.length();
                    ++emitted_refs;
                } else {
                    for (std::size_t b = r.block_begin; b < r.block_end; ++b)
                        std::printf("%.*s\n", static_cast<int>(block[b].size()), block[b].data());
                }
            } else {
                std::size_t b = r.block_begin;
                std::uint32_t c = r.corpus_begin;
                while (b < r.block_end) {
                    const std::string_view *cl = corpus.line(idx, r.file_id, c);
                    if (cl != nullptr && *cl == block[b]) {
                        const std::size_t b0 = b;
                        const std::uint32_t c0 = c;
                        do {
                            ++b; ++c;
                            if (b >= r.block_end) break;
                            cl = corpus.line(idx, r.file_id, c);
                        } while (cl != nullptr && *cl == block[b]);
                        if (emit_ref(corpus, idx, opt, r.file_id, c0, c - 1, false)) {
                            covered_lines += b - b0;
                            ++emitted_refs;
                        } else {
                            for (std::size_t k = b0; k < b; ++k)
                                std::printf("%.*s\n", static_cast<int>(block[k].size()), block[k].data());
                        }
                        continue;
                    }

                    std::string_view lpad, rpad;
                    if (cl != nullptr && trim_pad_for(*cl, block[b], lpad, rpad) &&
                        emit_ref(corpus, idx, opt, r.file_id, c, c, true, lpad, rpad)) {
                        ++covered_lines;
                        ++emitted_refs;
                    } else {
                        std::printf("%.*s\n", static_cast<int>(block[b].size()), block[b].data());
                    }
                    ++b; ++c;
                }
            }
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
            "  index            %s (%zu files, %zu keys%s)\n"
            "  guard            %s\n"
            "  block lines      %zu\n"
            "  anchors          %zu\n"
            "  copy references  %zu (%zu lines, %.1f%% covered)\n"
            "  literal lines    %zu\n",
            opt.index.c_str(), idx.file_count(), idx.key_count(),
            idx.normalized() ? ", normalized" : "",
            opt.hash ? "h:" : "s:",
            block.size(), anchor_count, emitted_refs, covered_lines, pct, literal_lines);
    }

    return 0;
}
