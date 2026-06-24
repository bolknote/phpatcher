/*
 * phpatcher-changes — generate a phpatcher-ed bundle from git changes.
 *
 * This is the libgit2 replacement for the old shell pipeline. It compares a
 * base revision to either another revision (--to, e.g. HEAD) or the working
 * tree, asks libgit2 for text hunks, and emits only phpatcher edit commands
 * plus added text: no removed/context source is quoted in the patch.
 *
 * Build: c++ -std=c++17 -O2 -o tools/phpatcher-changes tools/changes.cpp \
 *            $(pkg-config --cflags --libs libgit2)
 */
#include <git2.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "matcher_core.hpp"
#include "normalizer.hpp"
#include "../patch.hpp"

namespace {

using phpatcher::match::Posting;
using phpatcher::match::Run;
using phpatcher::tools::NormLine;
using phpatcher::tools::Normalizer;
constexpr char kIndexMagic[4] = {'P', 'H', 'I', 'X'};
constexpr std::uint32_t kFlagNormalized = 1u << 0;

[[noreturn]] void die(const std::string &msg) {
    std::fprintf(stderr, "phpatcher-changes: %s\n", msg.c_str());
    std::exit(2);
}

[[noreturn]] void die_git(const std::string &what, int rc) {
    const git_error *e = git_error_last();
    std::string msg = what + " failed";
    if (rc != 0) msg += " (" + std::to_string(rc) + ")";
    if (e && e->message) msg += ": " + std::string(e->message);
    die(msg);
}

void usage(const char *argv0) {
    std::fprintf(stderr,
        "phpatcher-changes — generate a phpatcher-ed bundle via libgit2\n\n"
        "Usage:\n"
        "  %s [options] [path ...]\n\n"
        "Options:\n"
        "  -b, --base <rev>     base revision to diff against (default: HEAD)\n"
        "      --to <rev>       target revision (default: working tree)\n"
        "  -o, --output <file>  write bundle to file instead of stdout\n"
        "  -c, --corpus         de-duplicate added lines using a corpus index\n"
        "      --index <file>   corpus index from phpatcher-index (required with -c)\n"
        "      --corpus-root <d> root used to resolve indexed corpus files\n"
        "  -n, --min-run <n>    minimum run length for corpus refs (default: 3)\n"
        "  -H, --hash           guard refs with h: instead of s: (s: is default)\n"
        "      --php PATH       php interpreter for normalized indexes (default: php)\n"
        "      --normalizer PATH path to normalize.php (default: next to this binary)\n"
        "  -h, --help           show this help\n\n"
        "Modified text files become ed-script sections; added text files become\n"
        "'# newfile:' sections that phpatcher materializes in memory at compile\n"
        "time (the file need not exist on the target). Deleted and binary files\n"
        "are skipped: a phpatcher-ed bundle cannot encode binary data, and\n"
        "phpatcher patches compilation rather than removing files from disk.\n",
        argv0);
}

struct Options {
    std::string base = "HEAD";
    std::string target;  /* empty = working tree */
    std::string output;
    std::vector<std::string> paths;
    bool corpus = false;
    std::string index;
    std::string corpus_root;
    std::size_t min_run = 3;
    bool hash = false;
    std::string php = "php";
    std::string normalizer;
};

template <typename T, void (*FreeFn)(T*)>
class GitPtr {
public:
    GitPtr() = default;
    explicit GitPtr(T *p) : p_(p) {}
    ~GitPtr() { reset(nullptr); }
    GitPtr(const GitPtr&) = delete;
    GitPtr& operator=(const GitPtr&) = delete;
    T *get() const { return p_; }
    T **out() { reset(nullptr); return &p_; }
    T *release() { T *p = p_; p_ = nullptr; return p; }
    void reset(T *p) { if (p_) FreeFn(p_); p_ = p; }
private:
    T *p_ = nullptr;
};

using RepoPtr = GitPtr<git_repository, git_repository_free>;
using ObjectPtr = GitPtr<git_object, git_object_free>;
using CommitPtr = GitPtr<git_commit, git_commit_free>;
using TreePtr = GitPtr<git_tree, git_tree_free>;
using DiffPtr = GitPtr<git_diff, git_diff_free>;
using BlobPtr = GitPtr<git_blob, git_blob_free>;

struct Change {
    std::int64_t old_start = 0;     /* first removed old line; 0 for pure append */
    std::int64_t old_count = 0;
    std::int64_t append_after = 0;  /* old line after which a pure addition goes */
    std::vector<std::string> added;
};

std::string strip_eol(const git_diff_line *line) {
    std::string_view s(line->content, line->content_len);
    if (!s.empty() && s.back() == '\n') s.remove_suffix(1);
    if (!s.empty() && s.back() == '\r') s.remove_suffix(1);
    return std::string(s);
}

struct DiffPayload {
    std::vector<Change> changes;
    bool in_change = false;
    Change current;
    std::int64_t last_old = 0;

    void flush() {
        if (!in_change) return;
        changes.push_back(std::move(current));
        current = Change{};
        in_change = false;
    }

    void start_change(std::int64_t old_start, std::int64_t append_after) {
        if (in_change) return;
        in_change = true;
        current = Change{};
        current.old_start = old_start;
        current.append_after = append_after;
    }
};

int hunk_cb(const git_diff_delta*, const git_diff_hunk *hunk, void *payload) {
    auto *p = static_cast<DiffPayload*>(payload);
    p->flush();
    p->last_old = hunk->old_start > 0 ? hunk->old_start - 1 : 0;
    return 0;
}

int line_cb(const git_diff_delta*, const git_diff_hunk*, const git_diff_line *line, void *payload) {
    auto *p = static_cast<DiffPayload*>(payload);
    switch (line->origin) {
        case GIT_DIFF_LINE_CONTEXT:
            p->flush();
            p->last_old = line->old_lineno;
            break;
        case GIT_DIFF_LINE_DELETION:
            p->start_change(line->old_lineno, line->old_lineno - 1);
            ++p->current.old_count;
            p->last_old = line->old_lineno;
            break;
        case GIT_DIFF_LINE_ADDITION:
            p->start_change(0, p->last_old);
            p->current.added.push_back(strip_eol(line));
            break;
        default:
            /* EOFNL and metadata pseudo-lines do not affect the ed script. */
            break;
    }
    return 0;
}

bool contains_nul(std::string_view s) {
    return std::memchr(s.data(), '\0', s.size()) != nullptr;
}

bool read_file(const std::string &path, std::string &out) {
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
    } else {
        out.clear();
    }
    return true;
}

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
        skip = 1;
    }
    std::vector<NormLine> norm = normalizer.run(input);
    std::vector<std::string> keys;
    if (norm.size() > skip) keys.reserve(norm.size() - skip);
    for (std::size_t i = skip; i < norm.size(); ++i) keys.push_back(std::move(norm[i].key));
    return keys;
}

class Index {
public:
    void load(const std::string &path) {
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
        if (std::memcmp(p, kIndexMagic, 4) != 0) die("not a phpatcher index (bad magic)");
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

    const std::vector<Posting> *find(std::string_view key) const {
        auto it = lines_.find(key);
        return it == lines_.end() ? nullptr : &it->second;
    }
    std::string_view file(std::uint32_t id) const { return files_.at(id); }
    bool normalized() const { return normalized_; }
private:
    std::string blob_;
    std::vector<std::string_view> files_;
    std::unordered_map<std::string_view, std::vector<Posting>> lines_;
    bool normalized_ = false;
};

class Corpus {
public:
    Corpus(std::string root, Normalizer *normalizer) : root_(std::move(root)), normalizer_(normalizer) {}

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
        const std::string path = root_ + "/" + std::string(idx.file(file_id));
        if (read_file(path, lf.content)) {
            lf.ok = true;
            lf.lines = split_lines(lf.content);
            if (normalizer_) {
                std::vector<NormLine> norm = normalizer_->run(lf.content);
                lf.keys.reserve(norm.size());
                lf.key_views.reserve(norm.size());
                for (NormLine &nl : norm) lf.keys.push_back(std::move(nl.key));
                for (const std::string &k : lf.keys) lf.key_views.push_back(k);
            }
        }
        return cache_.emplace(file_id, std::move(lf)).first->second;
    }

    std::string root_;
    Normalizer *normalizer_;
    std::unordered_map<std::uint32_t, Loaded> cache_;
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
            default: out.push_back(c); break;
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
        if (!cl) return false;
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

bool emit_ref(std::ostream &out, Corpus &corpus, const Index &idx, const Options &opt,
              std::uint32_t file_id, std::uint32_t begin, std::uint32_t end,
              bool trim_pad = false, std::string_view lpad = {}, std::string_view rpad = {}) {
    const std::string guard = make_guard(corpus, idx, opt, file_id, begin, end);
    if (guard.empty()) return false;
    const std::string path = quote_ed(idx.file(file_id));
    if (trim_pad) {
        out << "r " << path << " " << begin << " " << end << " "
            << quote_ed(lpad) << " " << quote_ed(rpad) << " # " << guard << "\n";
    } else {
        out << "r " << path << " " << begin << " " << end << " # " << guard << "\n";
    }
    return true;
}

std::string join_added(const std::vector<std::string> &lines) {
    std::string out;
    for (const std::string &l : lines) {
        out += l;
        out += '\n';
    }
    return out;
}

void emit_added_plain(std::ostream &out, const std::vector<std::string> &added) {
    for (const std::string &line : added) out << line << "\n";
}

void emit_added_corpus(std::ostream &out, const std::vector<std::string> &added,
                       const Options &opt, const Index &idx, Corpus &corpus,
                       Normalizer *normalizer) {
    if (added.empty()) return;

    std::vector<std::string_view> block;
    block.reserve(added.size());
    for (const std::string &l : added) block.push_back(l);

    std::vector<std::string> key_storage;
    std::vector<std::string_view> block_keys;
    if (idx.normalized()) {
        const std::string content = join_added(added);
        key_storage = normalize_block_keys(*normalizer, content);
        block_keys.reserve(key_storage.size());
        for (const std::string &k : key_storage) block_keys.push_back(k);
    } else {
        block_keys.reserve(block.size());
        for (std::string_view line : block) block_keys.push_back(phpatcher::match::trim(line));
    }

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

    std::size_t pos = 0, run_idx = 0;
    while (pos < block.size()) {
        if (run_idx < runs.size() && runs[run_idx].block_begin == pos) {
            const Run &r = runs[run_idx++];
            if (!idx.normalized()) {
                if (!emit_ref(out, corpus, idx, opt, r.file_id, r.corpus_begin, r.corpus_end, false)) {
                    for (std::size_t b = r.block_begin; b < r.block_end; ++b) out << block[b] << "\n";
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
                        if (!emit_ref(out, corpus, idx, opt, r.file_id, c0, c - 1, false)) {
                            for (std::size_t k = b0; k < b; ++k) out << block[k] << "\n";
                        }
                        continue;
                    }

                    std::string_view lpad, rpad;
                    if (cl != nullptr && trim_pad_for(*cl, block[b], lpad, rpad) &&
                        emit_ref(out, corpus, idx, opt, r.file_id, c, c, true, lpad, rpad)) {
                        /* emitted */
                    } else {
                        out << block[b] << "\n";
                    }
                    ++b; ++c;
                }
            }
            pos = r.block_end;
        } else {
            out << block[pos] << "\n";
            ++pos;
        }
    }
}

/* Emit a "# newfile:" section: the whole content of an added file, de-duplicated
 * against the corpus exactly like added lines in a change block, so a created
 * file can reference corpus runs instead of re-printing them. */
void emit_newfile(std::ostream &out, const std::string &path,
                  const std::vector<std::string> &lines, const Options &opt,
                  const Index *idx, Corpus *corpus, Normalizer *normalizer) {
    out << "# newfile: " << path << "\n";
    if (opt.corpus && idx && corpus) emit_added_corpus(out, lines, opt, *idx, *corpus, normalizer);
    else emit_added_plain(out, lines);
    out << ".\n";
}

void emit_change(std::ostream &out, const Change &c, const Options &opt,
                 const Index *idx, Corpus *corpus, Normalizer *normalizer) {
    if (c.old_count == 0) {
        out << c.append_after << "a\n";
    } else {
        const std::int64_t old_end = c.old_start + c.old_count - 1;
        if (c.added.empty()) {
            if (c.old_count == 1) out << c.old_start << "d\n";
            else out << c.old_start << "," << old_end << "d\n";
            return;
        }
        if (c.old_count == 1) out << c.old_start << "c\n";
        else out << c.old_start << "," << old_end << "c\n";
    }

    if (opt.corpus && idx && corpus) emit_added_corpus(out, c.added, opt, *idx, *corpus, normalizer);
    else emit_added_plain(out, c.added);
    out << ".\n";
}

git_tree *tree_for_rev(git_repository *repo, const std::string &rev) {
    ObjectPtr obj;
    int rc = git_revparse_single(obj.out(), repo, rev.c_str());
    if (rc != 0) die_git("resolve revision '" + rev + "'", rc);

    ObjectPtr peeled;
    rc = git_object_peel(peeled.out(), obj.get(), GIT_OBJECT_COMMIT);
    if (rc != 0) die_git("peel revision '" + rev + "' to commit", rc);

    auto *commit = reinterpret_cast<git_commit*>(peeled.release());
    CommitPtr commit_holder(commit);
    git_tree *tree = nullptr;
    rc = git_commit_tree(&tree, commit_holder.get());
    if (rc != 0) die_git("read tree for '" + rev + "'", rc);
    return tree;
}

void load_blob(git_repository *repo, const git_oid *oid, BlobPtr &blob) {
    int rc = git_blob_lookup(blob.out(), repo, oid);
    if (rc != 0) die_git("read blob", rc);
}

std::vector<Change> diff_buffers(const std::string &path,
                                 std::string_view old_buf,
                                 std::string_view new_buf) {
    git_diff_options opts;
    git_diff_options_init(&opts, GIT_DIFF_OPTIONS_VERSION);
    opts.context_lines = 3;

    DiffPayload payload;
    const int rc = git_diff_buffers(
        old_buf.data(), old_buf.size(), path.c_str(),
        new_buf.data(), new_buf.size(), path.c_str(),
        &opts, nullptr, nullptr, hunk_cb, line_cb, &payload);
    if (rc != 0) die_git("diff buffers for " + path, rc);
    payload.flush();
    return payload.changes;
}

}  // namespace

int main(int argc, char **argv) {
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) die("missing value for " + name);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else if (a == "-b" || a == "--base") opt.base = next(a);
        else if (a == "--to") opt.target = next(a);
        else if (a == "-o" || a == "--output") opt.output = next(a);
        else if (a == "-c" || a == "--corpus") opt.corpus = true;
        else if (a == "--index") opt.index = next(a);
        else if (a == "--corpus-root") opt.corpus_root = next(a);
        else if (a == "-H" || a == "--hash") opt.hash = true;
        else if (a == "--php") opt.php = next(a);
        else if (a == "--normalizer") opt.normalizer = next(a);
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
        else if (!a.empty() && a[0] == '-') die("unknown option: " + a);
        else opt.paths.push_back(a);
    }

    Index index;
    Normalizer normalizer;
    std::unique_ptr<Corpus> corpus;
    if (opt.corpus) {
        if (opt.index.empty()) die("--index is required with --corpus");
        if (opt.corpus_root.empty()) die("--corpus-root is required with --corpus");
        index.load(opt.index);
        if (index.normalized()) {
            if (opt.normalizer.empty()) {
                std::string self(argv[0]);
                const std::size_t slash = self.find_last_of('/');
                opt.normalizer = (slash == std::string::npos)
                    ? "normalize.php"
                    : self.substr(0, slash + 1) + "normalize.php";
            }
            normalizer.start(opt.php, opt.normalizer);
        }
        corpus.reset(new Corpus(opt.corpus_root, index.normalized() ? &normalizer : nullptr));
    }

    git_libgit2_init();

    RepoPtr repo;
    int rc = git_repository_open_ext(repo.out(), ".", 0, nullptr);
    if (rc != 0) die_git("open repository", rc);

    TreePtr base_tree(tree_for_rev(repo.get(), opt.base));
    TreePtr target_tree;
    if (!opt.target.empty()) {
        target_tree.reset(tree_for_rev(repo.get(), opt.target));
    }

    git_diff_options diff_opts;
    git_diff_options_init(&diff_opts, GIT_DIFF_OPTIONS_VERSION);
    diff_opts.flags = GIT_DIFF_IGNORE_SUBMODULES;
    std::vector<char*> pathspec;
    pathspec.reserve(opt.paths.size());
    for (std::string &p : opt.paths) pathspec.push_back(p.data());
    if (!pathspec.empty()) {
        diff_opts.pathspec.strings = pathspec.data();
        diff_opts.pathspec.count = pathspec.size();
    }

    DiffPtr diff;
    if (target_tree.get()) {
        rc = git_diff_tree_to_tree(diff.out(), repo.get(), base_tree.get(), target_tree.get(), &diff_opts);
    } else {
        rc = git_diff_tree_to_workdir_with_index(diff.out(), repo.get(), base_tree.get(), &diff_opts);
    }
    if (rc != 0) die_git("create diff", rc);

    std::ofstream fout;
    std::ostream *out = &std::cout;
    if (!opt.output.empty()) {
        fout.open(opt.output, std::ios::binary | std::ios::trunc);
        if (!fout) die("cannot write output: " + opt.output);
        out = &fout;
    }

    *out << "# phpatcher-ed v1\n";
    *out << "# Generated by tools/phpatcher-changes from changes vs " << opt.base;
    if (!opt.target.empty()) *out << " to " << opt.target;
    *out << ".\n";

    const std::size_t nd = git_diff_num_deltas(diff.get());
    std::size_t emitted = 0;
    const char *workdir = git_repository_workdir(repo.get());

    for (std::size_t i = 0; i < nd; ++i) {
        const git_diff_delta *d = git_diff_get_delta(diff.get(), i);
        if (!d) continue;
        const bool added = d->status == GIT_DELTA_ADDED;
        const bool modified = d->status == GIT_DELTA_MODIFIED;
        if (!added && !modified) continue;  /* deleted/renamed/etc: not encodable */
        if ((d->flags & GIT_DIFF_FLAG_BINARY) != 0) continue;

        const char *path_c = d->new_file.path ? d->new_file.path : d->old_file.path;
        if (!path_c) continue;
        const std::string path(path_c);

        /* Fetch the new-side content (shared by added and modified files). */
        std::string new_storage;
        std::string_view new_buf;
        BlobPtr new_blob;
        if (target_tree.get()) {
            load_blob(repo.get(), &d->new_file.id, new_blob);
            if (git_blob_is_binary(new_blob.get())) continue;
            new_buf = std::string_view(
                static_cast<const char*>(git_blob_rawcontent(new_blob.get())),
                git_blob_rawsize(new_blob.get()));
        } else {
            if (workdir == nullptr) die("repository has no working directory");
            if (!read_file(std::string(workdir) + path, new_storage)) continue;
            if (contains_nul(new_storage)) continue;
            new_buf = new_storage;
        }

        if (added) {
            /* A new file has no on-disk original on the target: emit its whole
             * content as a "# newfile:" section phpatcher creates in memory. */
            std::vector<std::string> lines;
            for (std::string_view sv : split_lines(new_buf)) lines.emplace_back(sv);
            emit_newfile(*out, path, lines, opt, opt.corpus ? &index : nullptr,
                         opt.corpus ? corpus.get() : nullptr,
                         index.normalized() ? &normalizer : nullptr);
            ++emitted;
            continue;
        }

        BlobPtr old_blob;
        load_blob(repo.get(), &d->old_file.id, old_blob);
        if (git_blob_is_binary(old_blob.get())) continue;
        const std::string_view old_buf(
            static_cast<const char*>(git_blob_rawcontent(old_blob.get())),
            git_blob_rawsize(old_blob.get()));

        std::vector<Change> changes = diff_buffers(path, old_buf, new_buf);
        if (changes.empty()) continue;

        *out << "# file: " << path << "\n";
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            emit_change(*out, *it, opt, opt.corpus ? &index : nullptr,
                        opt.corpus ? corpus.get() : nullptr,
                        index.normalized() ? &normalizer : nullptr);
        }
        ++emitted;
    }

    if (emitted == 0) die("Nothing to patch: no applicable changes found.");

    git_libgit2_shutdown();
    return 0;
}
