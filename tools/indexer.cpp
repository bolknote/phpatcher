/*
 * phpatcher-index — corpus indexer for reference-based ("copy from original")
 * patch generation.
 *
 * It scans a source tree, and for every "anchor-worthy" source line records
 * where that exact line occurs (file + 1-based line number). A later patch
 * generator uses this to replace a block of inserted code with a reference to
 * the original lines it already exists as (a move/copy), instead of re-printing
 * the code into the patch.
 *
 * Design decisions baked in here:
 *
 *   - Collision-free by construction. The index key is the *full normalized
 *     line text*, never just a hash. Two different lines can never collide into
 *     the same posting list. (A downstream matcher must still read the real file
 *     bytes before emitting a reference, but the index itself never lies.)
 *
 *   - Normalization trims only horizontal whitespace on both sides
 *     (space/tab/CR). Internal bytes are preserved, so the recorded location is
 *     a faithful anchor for the line's content.
 *
 *   - Garbage is left out. Lines shorter than --min-len after trimming are
 *     skipped, and lines that occur more than --max-postings times are dropped
 *     entirely: ubiquitous lines ("}", "return;", ...) are useless anchors and
 *     would bloat both the index and later matching. A run that merely *contains*
 *     such a line is still recoverable — the matcher anchors on a rarer line and
 *     extends across the common one by comparing real file bytes.
 *
 *   - Physical line numbers. Every physical line advances the counter, including
 *     skipped/garbage lines, so a recorded line number maps directly onto the
 *     file on disk (e.g. `sed -n 'N p'`).
 *
 * Output is a compact binary index (see write_index) or, with --dump, a
 * human-readable listing for inspection. Without -o/--dump it only reports stats.
 *
 * Build:  c++ -std=c++17 -O2 -o tools/phpatcher-index tools/indexer.cpp
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "normalizer.hpp"

namespace fs = std::filesystem;

using phpatcher::tools::Normalizer;

namespace {

constexpr char kMagic[4] = {'P', 'H', 'I', 'X'};
constexpr std::uint32_t kVersion = 2;
constexpr std::uint32_t kFlagNormalized = 1u << 0;

struct Options {
    std::string root = ".";
    std::vector<std::string> exts = {"php", "inc"};
    std::unordered_set<std::string> excludes;
    std::size_t min_len = 6;
    std::size_t max_postings = 64;  /* 0 = no cap */
    std::string output;
    bool dump = false;
    bool verbose = false;
    bool normalize = false;        /* tokenize via php coprocess */
    std::size_t jobs = 0;          /* normalizer workers; 0 = auto (CPU count) */
    std::string php = "php";       /* php interpreter for normalization */
    std::string normalizer;        /* path to normalize.php (default: next to argv0) */
};

void usage(const char *argv0) {
    std::fprintf(stderr,
        "phpatcher-index — index a source tree for reference-based patching\n\n"
        "Usage:\n"
        "  %s [options] [root]\n\n"
        "Arguments:\n"
        "  root                  directory to scan (default: .)\n\n"
        "Options:\n"
        "  -e, --ext LIST        comma-separated extensions (default: php,inc)\n"
        "  -x, --exclude NAME    skip any directory named NAME (repeatable)\n"
        "  -m, --min-len N       skip lines shorter than N chars after trim (default: 6)\n"
        "  -c, --max-postings N  drop lines occurring > N times (default: 64; 0 = no cap)\n"
        "  -o, --output FILE     write the binary index to FILE\n"
        "      --dump            print the index as text to stdout\n"
        "  -n, --normalize       tokenize via php and collapse inter-token whitespace\n"
        "  -j, --jobs N          parallel normalizer workers (default: auto = CPU count)\n"
        "      --php PATH         php interpreter for --normalize (default: php)\n"
        "      --normalizer PATH  path to normalize.php (default: next to this binary)\n"
        "  -v, --verbose         report stats and the most common lines to stderr\n"
        "  -h, --help            show this help\n\n"
        "Without -o/--dump the tool only scans and reports statistics.\n",
        argv0);
}

[[noreturn]] void die(const std::string &msg) {
    std::fprintf(stderr, "phpatcher-index: %s\n", msg.c_str());
    std::exit(2);
}

/* Parse a non-negative integer option value, failing with a clear message
 * instead of an uncaught std::stoul exception (e.g. when the value is missing
 * and the next flag got swallowed in its place). */
std::size_t parse_uint(const std::string &name, const std::string &value) {
    try {
        std::size_t pos = 0;
        const unsigned long v = std::stoul(value, &pos);
        if (pos != value.size()) throw std::invalid_argument("trailing characters");
        return static_cast<std::size_t>(v);
    } catch (const std::exception &) {
        die(name + " expects a non-negative integer, got '" + value + "'");
    }
}

std::string to_lower(std::string s) {
    for (char &c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == ',') {
            if (i > start) {
                std::string item(s.substr(start, i - start));
                /* tolerate a leading dot on the extension */
                if (!item.empty() && item.front() == '.') item.erase(item.begin());
                if (!item.empty()) out.push_back(to_lower(std::move(item)));
            }
            start = i + 1;
        }
    }
    return out;
}

/* Trim horizontal whitespace (space/tab/CR/FF/VT) from both ends. */
std::string_view trim(std::string_view s) {
    const auto is_ws = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!s.empty() && is_ws(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_ws(s.back())) s.remove_suffix(1);
    return s;
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

std::string ext_of(const fs::path &p) {
    std::string e = p.extension().string();
    if (!e.empty() && e.front() == '.') e.erase(e.begin());
    return to_lower(std::move(e));
}

/* A posting packs (file_id, line_number) into one 64-bit word: high 32 bits are
 * the file id (index into `files`), low 32 bits are the 1-based line number. */
inline std::uint64_t pack(std::uint32_t file_id, std::uint32_t lineno) {
    return (static_cast<std::uint64_t>(file_id) << 32) | lineno;
}
inline std::uint32_t posting_file(std::uint64_t p) { return static_cast<std::uint32_t>(p >> 32); }
inline std::uint32_t posting_line(std::uint64_t p) { return static_cast<std::uint32_t>(p & 0xffffffffu); }

struct Index {
    std::vector<std::string> files;  /* file_id -> path (relative to root) */
    std::unordered_map<std::string, std::vector<std::uint64_t>> lines;  /* norm line -> postings */
};

template <typename T>
void put(std::string &buf, T v) {
    buf.append(reinterpret_cast<const char *>(&v), sizeof(T));
}

/* Binary layout (native endianness):
 *   magic[4] "PHIX", u32 version, u32 flags, u32 min_len, u32 max_postings,
 *   u32 file_count, { u16 len, bytes }*,
 *   u32 key_count,  { u32 len, bytes, u32 n, { u32 file_id, u32 lineno }* }*
 * flags bit 0 = keys are php-token-normalized (matcher must normalize too).
 * Keys with > max_postings occurrences are omitted. */
bool write_index(const Index &idx, const Options &opt, std::size_t &keys_written,
                 std::size_t &postings_written) {
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    put<std::uint32_t>(buf, kVersion);
    put<std::uint32_t>(buf, opt.normalize ? kFlagNormalized : 0u);
    put<std::uint32_t>(buf, static_cast<std::uint32_t>(opt.min_len));
    put<std::uint32_t>(buf, static_cast<std::uint32_t>(opt.max_postings));

    put<std::uint32_t>(buf, static_cast<std::uint32_t>(idx.files.size()));
    for (const std::string &p : idx.files) {
        put<std::uint16_t>(buf, static_cast<std::uint16_t>(p.size()));
        buf.append(p);
    }

    /* Reserve a slot for the key count; backfill once we know how many survive. */
    const std::size_t count_pos = buf.size();
    put<std::uint32_t>(buf, 0);

    keys_written = 0;
    postings_written = 0;
    for (const auto &kv : idx.lines) {
        if (opt.max_postings != 0 && kv.second.size() > opt.max_postings) continue;
        put<std::uint32_t>(buf, static_cast<std::uint32_t>(kv.first.size()));
        buf.append(kv.first);
        put<std::uint32_t>(buf, static_cast<std::uint32_t>(kv.second.size()));
        for (std::uint64_t p : kv.second) {
            put<std::uint32_t>(buf, posting_file(p));
            put<std::uint32_t>(buf, posting_line(p));
        }
        ++keys_written;
        postings_written += kv.second.size();
    }
    const auto kw = static_cast<std::uint32_t>(keys_written);
    std::memcpy(&buf[count_pos], &kw, sizeof(kw));

    std::ofstream f(opt.output, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(buf.data(), static_cast<std::streamsize>(buf.size()));
    return static_cast<bool>(f);
}

/* A matched source file awaiting normalization. Its index in the collected list
 * is its scan-order file id, so postings are numbered deterministically
 * regardless of which worker happens to process it. */
struct NormFile {
    std::string abs;  /* absolute path, read by the worker          */
    std::string rel;  /* path relative to root, stored in the index */
};

/* Per-worker partial index plus line counters, merged after the threads join. */
struct WorkerResult {
    std::unordered_map<std::string, std::vector<std::uint64_t>> lines;
    std::size_t total = 0, nonempty = 0, indexed = 0;
};

/* Portable default worker count: std::thread::hardware_concurrency() is the
 * standard, OS-agnostic way (Windows/macOS/Linux); it returns 0 when it cannot
 * tell, in which case we fall back to a single worker. */
std::size_t auto_jobs() {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc ? static_cast<std::size_t>(hc) : 1u;
}

/* Pull files off the shared cursor and normalize each via this worker's own
 * coprocess, accumulating anchors into a private map (no shared state, so no
 * locking on the hot path). */
void normalize_worker(Normalizer &nz, const std::vector<NormFile> &files,
                      std::atomic<std::size_t> &cursor, std::size_t min_len,
                      WorkerResult &out) {
    for (;;) {
        const std::size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
        if (i >= files.size()) break;

        std::string content;
        if (!read_file(files[i].abs, content)) {
            std::fprintf(stderr, "  skip (unreadable): %s\n", files[i].abs.c_str());
            continue;
        }
        const std::uint32_t fid = static_cast<std::uint32_t>(i);
        nz.run_each(content, [&](std::uint32_t li, std::uint8_t, std::string_view key) {
            ++out.total;
            if (key.empty()) return;
            ++out.nonempty;
            if (trim(key).size() < min_len) return;
            out.lines[std::string(key)].push_back(
                pack(fid, static_cast<std::uint32_t>(li + 1)));
            ++out.indexed;
        });
    }
}

/* Normalize `files` across N worker coprocesses and fold the results into `idx`.
 * Returns the worker count actually used. The output is content-equivalent to a
 * serial build: file ids are compacted to the files that contribute, in scan
 * order, and every posting list is sorted, so it does not depend on thread
 * timing. */
std::size_t process_normalized_parallel(const std::vector<NormFile> &files,
                                        const Options &opt, Index &idx,
                                        std::size_t &total_lines,
                                        std::size_t &nonempty_lines,
                                        std::size_t &indexed_lines) {
    const std::size_t nfiles = files.size();
    if (nfiles == 0) return 0;

    std::size_t jobs = opt.jobs ? opt.jobs : auto_jobs();
    if (jobs < 1) jobs = 1;
    if (jobs > nfiles) jobs = nfiles;

    /* Spawn the coprocesses while still single-threaded: fork() in a process that
     * already has worker threads running is unsafe, so do it first. */
    std::vector<std::unique_ptr<Normalizer>> nz;
    nz.reserve(jobs);
    for (std::size_t j = 0; j < jobs; ++j) {
        auto n = std::make_unique<Normalizer>();
        n->start(opt.php, opt.normalizer);
        nz.push_back(std::move(n));
    }

    std::vector<WorkerResult> results(jobs);
    std::atomic<std::size_t> cursor{0};
    if (jobs == 1) {
        normalize_worker(*nz[0], files, cursor, opt.min_len, results[0]);
    } else {
        std::vector<std::thread> threads;
        threads.reserve(jobs);
        for (std::size_t j = 0; j < jobs; ++j) {
            threads.emplace_back(normalize_worker, std::ref(*nz[j]), std::cref(files),
                                 std::ref(cursor), opt.min_len, std::ref(results[j]));
        }
        for (std::thread &t : threads) t.join();
    }
    nz.clear();  /* dtor closes pipes (EOF) and reaps each coprocess */

    /* Merge: adopt the first worker's map, fold the rest in by moving posting
     * lists (a key is copied only when it is new to the merged map). */
    std::unordered_map<std::string, std::vector<std::uint64_t>> merged;
    for (std::size_t j = 0; j < jobs; ++j) {
        total_lines += results[j].total;
        nonempty_lines += results[j].nonempty;
        indexed_lines += results[j].indexed;
        if (merged.empty()) {
            merged = std::move(results[j].lines);
            continue;
        }
        for (auto &kv : results[j].lines) {
            std::vector<std::uint64_t> &dst = merged[kv.first];
            if (dst.empty()) {
                dst = std::move(kv.second);
            } else {
                dst.insert(dst.end(), std::make_move_iterator(kv.second.begin()),
                           std::make_move_iterator(kv.second.end()));
            }
        }
    }

    /* Compact file ids down to the files that actually contributed, keeping scan
     * order, then sort each posting list so the result is independent of which
     * worker saw which file (matches the serial build's content). */
    std::vector<char> used(nfiles, 0);
    for (const auto &kv : merged)
        for (std::uint64_t p : kv.second) used[posting_file(p)] = 1;

    std::vector<std::uint32_t> remap(nfiles, 0);
    idx.files.clear();
    for (std::size_t id = 0; id < nfiles; ++id) {
        if (used[id]) {
            remap[id] = static_cast<std::uint32_t>(idx.files.size());
            idx.files.push_back(files[id].rel);
        }
    }
    for (auto &kv : merged) {
        for (std::uint64_t &p : kv.second)
            p = pack(remap[posting_file(p)], posting_line(p));
        std::sort(kv.second.begin(), kv.second.end());
    }
    idx.lines = std::move(merged);
    return jobs;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    bool ext_overridden = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) die(std::string("missing value for ") + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "-e" || a == "--ext") {
            opt.exts = split_csv(next(a.c_str()));
            ext_overridden = true;
        }
        else if (a == "-x" || a == "--exclude") opt.excludes.insert(next(a.c_str()));
        else if (a == "-m" || a == "--min-len") opt.min_len = parse_uint(a, next(a.c_str()));
        else if (a == "-c" || a == "--max-postings") opt.max_postings = parse_uint(a, next(a.c_str()));
        else if (a == "-o" || a == "--output") opt.output = next(a.c_str());
        else if (a == "--dump") opt.dump = true;
        else if (a == "-n" || a == "--normalize") opt.normalize = true;
        else if (a == "-j" || a == "--jobs") opt.jobs = parse_uint(a, next(a.c_str()));
        else if (a == "--php") opt.php = next(a.c_str());
        else if (a == "--normalizer") opt.normalizer = next(a.c_str());
        else if (a == "-v" || a == "--verbose") opt.verbose = true;
        else if (!a.empty() && a[0] == '-') die("unknown option: " + a);
        else opt.root = a;
    }
    if (ext_overridden && opt.exts.empty()) die("no valid extensions given to --ext");

    std::error_code ec;
    if (!fs::is_directory(opt.root, ec)) die("not a directory: " + opt.root);
    const fs::path root = fs::path(opt.root);

    const std::unordered_set<std::string> exts(opt.exts.begin(), opt.exts.end());

    if (opt.normalize) {
        if (opt.normalizer.empty()) {
            fs::path self(argv[0]);
            fs::path dir = self.parent_path();
            opt.normalizer = (dir.empty() ? fs::path("normalize.php")
                                          : dir / "normalize.php").string();
        }
        if (!fs::exists(opt.normalizer))
            die("normalizer script not found: " + opt.normalizer +
                " (pass --normalizer PATH)");
    }

    Index idx;
    std::vector<NormFile> norm_files;  /* matched files awaiting normalization */
    std::size_t scanned_files = 0, matched_files = 0, excluded_dirs = 0;
    std::size_t total_lines = 0, nonempty_lines = 0, indexed_lines = 0;

    const auto t0 = std::chrono::steady_clock::now();

    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry &entry = *it;
        std::error_code ec2;

        if (entry.is_directory(ec2)) {
            if (opt.excludes.count(entry.path().filename().string())) {
                ++excluded_dirs;
                it.disable_recursion_pending();  /* do not descend */
            }
            continue;
        }
        if (!entry.is_regular_file(ec2)) continue;
        ++scanned_files;
        if (!exts.count(ext_of(entry.path()))) continue;
        ++matched_files;

        /* Normalized indexing is deferred: record the file now and process the
         * whole list in parallel after the (serial) directory walk, so both the
         * php tokenization and the per-file disk reads spread across cores. */
        if (opt.normalize) {
            norm_files.push_back(NormFile{
                entry.path().string(),
                fs::relative(entry.path(), root, ec2).string()});
            continue;
        }

        std::string content;
        if (!read_file(entry.path(), content)) {
            std::fprintf(stderr, "  skip (unreadable): %s\n", entry.path().c_str());
            continue;
        }

        /* Assign a file id lazily, only if the file contributes a posting. */
        std::uint32_t file_id = 0;
        bool have_id = false;
        const auto ensure_id = [&] {
            if (!have_id) {
                file_id = static_cast<std::uint32_t>(idx.files.size());
                idx.files.push_back(fs::relative(entry.path(), root, ec2).string());
                have_id = true;
            }
            return file_id;
        };

        std::uint32_t lineno = 0;
        std::size_t start = 0;
        const std::size_t n = content.size();
        for (std::size_t i = 0; i <= n; ++i) {
            if (i == n && start == i) break;  /* no trailing empty line after final '\n' */
            if (i == n || content[i] == '\n') {
                ++lineno;
                ++total_lines;
                std::string_view raw(content.data() + start, i - start);
                const std::string_view norm = trim(raw);
                start = i + 1;
                if (norm.empty()) continue;
                ++nonempty_lines;
                if (norm.size() < opt.min_len) continue;
                idx.lines[std::string(norm)].push_back(pack(ensure_id(), lineno));
                ++indexed_lines;
            }
        }
    }
    if (ec) die("directory scan failed: " + ec.message());

    std::size_t norm_jobs = 0;
    if (opt.normalize) {
        norm_jobs = process_normalized_parallel(norm_files, opt, idx, total_lines,
                                                nonempty_lines, indexed_lines);
    }

    /* Frequency stats and cap accounting. */
    std::size_t distinct_keys = idx.lines.size();
    std::size_t capped_keys = 0, capped_postings = 0;
    for (const auto &kv : idx.lines) {
        if (opt.max_postings != 0 && kv.second.size() > opt.max_postings) {
            ++capped_keys;
            capped_postings += kv.second.size();
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    if (opt.verbose) {
        /* Top lines by frequency (helps tune --min-len / --max-postings). */
        std::vector<std::pair<std::size_t, const std::string *>> top;
        top.reserve(idx.lines.size());
        for (const auto &kv : idx.lines) top.emplace_back(kv.second.size(), &kv.first);
        const std::size_t k = std::min<std::size_t>(15, top.size());
        std::partial_sort(top.begin(), top.begin() + k, top.end(),
                          [](const auto &a, const auto &b) { return a.first > b.first; });
        std::fprintf(stderr, "\nMost common indexed lines:\n");
        for (std::size_t i = 0; i < k; ++i) {
            std::string s = *top[i].second;
            if (s.size() > 70) s = s.substr(0, 67) + "...";
            std::fprintf(stderr, "  %8zu  %s\n", top[i].first, s.c_str());
        }
    }

    std::fprintf(stderr,
        "\nphpatcher-index summary\n"
        "  root             %s\n"
        "  extensions       %s\n"
        "  excluded dirs    %zu skipped\n"
        "  files scanned    %zu\n"
        "  files matched    %zu\n"
        "  physical lines   %zu\n"
        "  non-empty lines  %zu\n"
        "  indexed lines    %zu (>= %zu chars, trimmed)\n"
        "  distinct keys    %zu\n"
        "  frequency cap    %zu keys / %zu postings dropped (> %zu occurrences)\n"
        "  build time       %.2fs\n",
        opt.root.c_str(),
        [&] { std::string s; for (auto &e : opt.exts) { if (!s.empty()) s += ','; s += e; } return s; }().c_str(),
        excluded_dirs, scanned_files, matched_files, total_lines, nonempty_lines,
        indexed_lines, opt.min_len, distinct_keys,
        capped_keys, capped_postings, opt.max_postings, secs);

    if (opt.normalize)
        std::fprintf(stderr, "  normalize jobs   %zu\n", norm_jobs);

    if (opt.dump) {
        for (const auto &kv : idx.lines) {
            if (opt.max_postings != 0 && kv.second.size() > opt.max_postings) continue;
            std::printf("%zu\t%s\n", kv.second.size(), kv.first.c_str());
        }
    }

    if (!opt.output.empty()) {
        std::size_t keys_written = 0, postings_written = 0;
        if (!write_index(idx, opt, keys_written, postings_written)) {
            die("cannot write index to " + opt.output);
        }
        std::error_code szec;
        const auto bytes = fs::file_size(opt.output, szec);
        std::fprintf(stderr,
            "  wrote index      %s (%zu keys, %zu postings, %s bytes)\n",
            opt.output.c_str(), keys_written, postings_written,
            szec ? "?" : std::to_string(bytes).c_str());
    }

    return 0;
}
