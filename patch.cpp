/*
 * phpatcher - in-memory unified-diff / ed-script patching core.
 *
 * Deliberately free of any PHP/Zend dependency so it can be unit-tested in
 * isolation. The two entry points are PatchSet::parse() (build an index from a
 * patch file) and PatchSet::apply() (produce patched content for one file).
 *
 * Performance notes:
 *   - Parsing and application work over std::string_view slices of the caller's
 *     buffers; only the data that must outlive the call (hunk/ed bodies, index
 *     keys, the final output) is materialised into owning std::strings.
 *   - The index is a flat array sorted by canonical path; lookups are O(log n)
 *     via binary search, and the contiguous storage is cache- and COW-friendly.
 */
#include "patch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace phpatcher {
namespace {

namespace fs = std::filesystem;

using std::string_view;
constexpr std::size_t npos = string_view::npos;

/* Tokens recognised in the two supported patch formats. */
constexpr string_view kEdMagic = "# phpatcher-ed";  /* ed-bundle selector       */
constexpr string_view kEdFileHeader = "# file:";    /* per-file section marker  */
constexpr string_view kOldFileHeader = "--- ";      /* unified-diff old path    */
constexpr string_view kNewFileHeader = "+++ ";      /* unified-diff new path    */
constexpr string_view kHunkHeader = "@@";           /* unified-diff hunk header */
constexpr string_view kDevNull = "/dev/null";       /* "no such file" sentinel  */
constexpr char kEdInputTerminator = '.';            /* ends an a/c/i input block*/

bool is_digit(char c) { return c >= '0' && c <= '9'; }

bool is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/* Split `content` into lines (newline stripped), as views into `content`.
 * `ends_with_nl` reports whether the input terminated with '\n', which must be
 * preserved when the content is reconstructed. */
std::vector<string_view> split_lines(string_view content, bool& ends_with_nl) {
    std::vector<string_view> lines;
    ends_with_nl = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < content.size(); ++i) {
        if (content[i] == '\n') {
            lines.push_back(content.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < content.size()) {
        lines.push_back(content.substr(start));
    } else if (!content.empty()) {
        ends_with_nl = true;
    }
    return lines;
}

/* Concatenate `lines` with '\n' separators, optionally terminating the result
 * with a trailing newline. */
void join_lines(const std::vector<string_view>& lines, bool trailing_newline, std::string& out) {
    std::size_t total = 0;
    for (const string_view line : lines) {
        total += line.size() + 1;  /* +1 reserves room for each separator/EOL */
    }
    out.clear();
    out.reserve(total);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out.append(lines[i].data(), lines[i].size());
        if (i + 1 < lines.size() || trailing_newline) {
            out += '\n';
        }
    }
}

string_view rtrim(string_view s) {
    while (!s.empty() && is_blank(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

/* Read a non-negative decimal integer at `s[pos]`, advancing `pos` past it.
 * Returns false when no digit is present or the value would overflow int64. */
bool read_uint(string_view s, std::size_t& pos, std::int64_t& out) {
    if (pos >= s.size() || !is_digit(s[pos])) {
        return false;
    }
    std::int64_t value = 0;
    while (pos < s.size() && is_digit(s[pos])) {
        if (value > (INT64_MAX - 9) / 10) {
            return false;  /* reject rather than wrap (signed overflow is UB) */
        }
        value = value * 10 + (s[pos] - '0');
        ++pos;
    }
    out = value;
    return true;
}

/* Strip a unified-diff path field of its trailing tab-separated timestamp
 * (git/diff sometimes append "\t<date>") and its a// b/ prefix. */
std::string clean_diff_path(string_view raw) {
    const std::size_t tab = raw.find('\t');
    if (tab != npos) {
        raw = raw.substr(0, tab);
    }
    while (!raw.empty() && (raw.back() == '\r' || raw.back() == ' ')) {
        raw.remove_suffix(1);
    }
    if (raw == kDevNull) {
        return std::string(raw);
    }
    if (raw.size() >= 2 && (raw[0] == 'a' || raw[0] == 'b') && raw[1] == '/') {
        raw.remove_prefix(2);
    }
    return std::string(raw);
}

/* Parse "@@ -l[,s] +l[,s] @@". Omitted counts default to 1. */
bool parse_hunk_header(string_view line, Hunk& hunk) {
    if (line.substr(0, kHunkHeader.size()) != kHunkHeader) {
        return false;
    }
    const std::size_t minus = line.find('-');
    const std::size_t plus = line.find('+');
    if (minus == npos || plus == npos || plus < minus) {
        return false;
    }

    /* A range is "<sign>start[,count]"; `marker` points at the sign. */
    auto read_range = [&](std::size_t marker, std::int64_t& start, std::int64_t& count) -> bool {
        std::size_t pos = marker + 1;
        if (!read_uint(line, pos, start)) {
            return false;
        }
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            return read_uint(line, pos, count);
        }
        count = 1;
        return true;
    };

    return read_range(minus, hunk.orig_start, hunk.orig_count) &&
           read_range(plus, hunk.new_start, hunk.new_count);
}

/* True if the patch text is one of our ed-script bundles (its first non-blank
 * line starts with the magic token). */
bool looks_like_ed_bundle(string_view text) {
    for (std::size_t i = 0; i <= text.size();) {
        const std::size_t eol = text.find('\n', i);
        const string_view line = text.substr(i, (eol == npos ? text.size() : eol) - i);
        const std::size_t s = line.find_first_not_of(" \t\r");
        if (s != npos) {
            return line.substr(s, kEdMagic.size()) == kEdMagic;
        }
        if (eol == npos) {
            break;
        }
        i = eol + 1;
    }
    return false;
}

/* Parse an ed command header such as "12d", "5,9c", "0a", "3i". Fills `cmd`
 * and reports via `needs_input` whether an a/c/i text block follows. */
bool parse_ed_command(string_view line, EdCommand& cmd, bool& needs_input) {
    std::size_t pos = 0;
    std::int64_t first = 0;
    if (!read_uint(line, pos, first)) {
        return false;
    }
    std::int64_t second = first;
    if (pos < line.size() && line[pos] == ',') {
        ++pos;
        if (!read_uint(line, pos, second)) {
            return false;
        }
    }
    if (pos >= line.size()) {
        return false;
    }
    const char op = line[pos++];
    if (rtrim(line.substr(pos)) != string_view()) {
        return false;  /* only trailing whitespace may follow the command */
    }

    switch (op) {
        case 'd':
            cmd = {EdCommand::Delete, first, second, {}};
            needs_input = false;
            return true;
        case 'c':
            cmd = {EdCommand::Change, first, second, {}};
            needs_input = true;
            return true;
        case 'a':
            cmd = {EdCommand::Append, first, first, {}};
            needs_input = true;
            return true;
        case 'i':
            cmd = {EdCommand::Insert, first, first, {}};
            needs_input = true;
            return true;
        default:
            return false;
    }
}

/* Apply an ed-script FilePatch to `original`. */
bool apply_ed_script(const FilePatch& fp, string_view original,
                     std::string& out, std::string& error) {
    bool ends_nl = false;
    std::vector<string_view> lines = split_lines(original, ends_nl);
    if (original.empty()) {
        ends_nl = true;  /* a file built from nothing should be newline-terminated */
    }

    const auto out_of_range = [&](const char* what) {
        error = what;
        return false;
    };

    for (const EdCommand& c : fp.ed()) {
        const auto size = static_cast<std::int64_t>(lines.size());
        switch (c.kind) {
            case EdCommand::Delete:
                if (c.start < 1 || c.end < c.start || c.end > size) {
                    return out_of_range("ed delete out of range");
                }
                lines.erase(lines.begin() + (c.start - 1), lines.begin() + c.end);
                break;

            case EdCommand::Change: {
                if (c.start < 1 || c.end < c.start || c.end > size) {
                    return out_of_range("ed change out of range");
                }
                auto at = lines.erase(lines.begin() + (c.start - 1), lines.begin() + c.end);
                lines.insert(at, c.lines.begin(), c.lines.end());
                break;
            }

            case EdCommand::Append:
                if (c.start < 0 || c.start > size) {
                    return out_of_range("ed append out of range");
                }
                lines.insert(lines.begin() + c.start, c.lines.begin(), c.lines.end());
                break;

            case EdCommand::Insert:
                if (c.start < 1 || c.start > size + 1) {
                    return out_of_range("ed insert out of range");
                }
                lines.insert(lines.begin() + (c.start - 1), c.lines.begin(), c.lines.end());
                break;
        }
    }

    join_lines(lines, ends_nl, out);
    return true;
}

}  // namespace

std::string PatchSet::basename_of(string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return std::string(slash == npos ? path : path.substr(slash + 1));
}

std::string PatchSet::canonicalize(const std::string& path, const std::string& base_dir) {
    fs::path p(path);
    if (p.is_relative() && !base_dir.empty()) {
        p = fs::path(base_dir) / p;
    }

    /* Prefer the real path (resolves symlinks and .., matches what the engine
     * uses for include bookkeeping). realpath() with a null buffer allocates a
     * result of the right size, sidestepping PATH_MAX entirely. */
    if (char *resolved = ::realpath(p.c_str(), nullptr)) {
        std::string result(resolved);
        std::free(resolved);
        return result;
    }

    /* The file does not exist yet (or is otherwise unresolvable): fall back to a
     * lexical normalization so we still produce a stable, absolute key. */
    std::error_code ec;
    fs::path normalized = p.lexically_normal();
    if (normalized.is_relative()) {
        const fs::path abs = fs::absolute(normalized, ec);
        if (!ec) {
            normalized = abs.lexically_normal();
        }
    }
    return normalized.string();
}

bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return false;
    }
    /* Size up front when the stream is seekable, so the whole file lands in a
     * single allocation; fall back to streaming for pipes and the like. */
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size > 0) {
        out.resize(static_cast<std::size_t>(size));
        f.seekg(0);
        f.read(out.data(), size);
        if (f.bad()) {
            return false;
        }
        out.resize(static_cast<std::size_t>(f.gcount()));
        return true;
    }
    f.clear();
    f.seekg(0);
    std::ostringstream ss;
    ss << f.rdbuf();
    if (f.bad()) {
        return false;
    }
    out = ss.str();
    return true;
}

bool PatchSet::finalize(std::string& error) {
    std::sort(index_.begin(), index_.end(),
              [](const Entry& a, const Entry& b) { return a.first < b.first; });

    for (std::size_t i = 1; i < index_.size(); ++i) {
        if (index_[i].first == index_[i - 1].first) {
            error = "duplicate patch section for " + index_[i].first;
            return false;
        }
    }

    basenames_.clear();
    basenames_.reserve(index_.size());
    for (const Entry& e : index_) {
        basenames_.push_back(basename_of(e.first));
    }
    std::sort(basenames_.begin(), basenames_.end());
    basenames_.erase(std::unique(basenames_.begin(), basenames_.end()), basenames_.end());
    return true;
}

void PatchSet::recanonicalize(const std::function<std::string(const std::string&)>& fn) {
    for (Entry& e : index_) {
        e.first = fn(e.first);
    }
    /* Re-canonicalization can only narrow distinct keys onto the same path in
     * pathological setups; tolerate it (first match wins) rather than error in a
     * void method, but keep the index sorted and the basename filter rebuilt. */
    std::sort(index_.begin(), index_.end(),
              [](const Entry& a, const Entry& b) { return a.first < b.first; });
    basenames_.clear();
    basenames_.reserve(index_.size());
    for (const Entry& e : index_) {
        basenames_.push_back(basename_of(e.first));
    }
    std::sort(basenames_.begin(), basenames_.end());
    basenames_.erase(std::unique(basenames_.begin(), basenames_.end()), basenames_.end());
}

bool PatchSet::parse(const std::string& diff_text, const std::string& base_dir, std::string& error) {
    index_.clear();
    basenames_.clear();

    if (looks_like_ed_bundle(diff_text)) {
        return parse_ed_bundle(diff_text, base_dir, error);
    }

    bool ends_nl = false;
    const std::vector<string_view> lines = split_lines(diff_text, ends_nl);

    FilePatch current;
    std::string current_key;   /* canonical path of `current`, the index key  */
    bool have_current = false;
    std::string pending_old;   /* path from the last "--- " line */
    std::string pending_new;   /* path from the last "+++ " line */
    Hunk* active_hunk = nullptr;
    std::int64_t need_orig = 0; /* original-side body lines still expected by active_hunk */
    std::int64_t need_new = 0;  /* new-side body lines still expected by active_hunk      */

    const auto flush_current = [&]() {
        if (have_current && !current.hunks().empty()) {
            index_.emplace_back(std::move(current_key), std::move(current));
        }
        current = FilePatch();
        current_key.clear();
        have_current = false;
        active_hunk = nullptr;
    };

    const auto begin_file = [&]() -> bool {
        /* Target the patched (new) path when present, else the old path. */
        string_view chosen;
        if (!pending_new.empty() && pending_new != kDevNull) {
            chosen = pending_new;
        } else if (!pending_old.empty() && pending_old != kDevNull) {
            chosen = pending_old;
        } else {
            error = "diff hunk without a usable target file path";
            return false;
        }
        flush_current();
        current_key = canonicalize(std::string(chosen), base_dir);
        have_current = true;
        return true;
    };

    for (std::size_t i = 0; i < lines.size(); ++i) {
        const string_view line = lines[i];

        /* While a hunk still expects body lines, consume them verbatim. This is
         * essential for correctness: a removed/added line whose *content* begins
         * with "--- ", "+++ " or "@@" must not be mistaken for a header. The
         * line counts in the hunk header tell us exactly where the body ends. */
        if (active_hunk != nullptr && (need_orig > 0 || need_new > 0)) {
            const char marker = line.empty() ? ' ' : line[0];
            /* Body content is the line without its marker byte (empty for an
             * empty context line, which a diff may emit as a wholly blank line). */
            const string_view content = line.size() <= 1 ? string_view() : line.substr(1);
            switch (marker) {
                case ' ':  /* context line (an empty line is empty context) */
                    active_hunk->lines.push_back({LineKind::Context, std::string(content)});
                    --need_orig;
                    --need_new;
                    break;
                case '-':  /* removed from the original */
                    active_hunk->lines.push_back({LineKind::Remove, std::string(content)});
                    --need_orig;
                    break;
                case '+':  /* added in the patched file */
                    active_hunk->lines.push_back({LineKind::Add, std::string(content)});
                    --need_new;
                    break;
                case '\\':  /* "\ No newline" note; consumes no original/new line */
                    active_hunk->lines.push_back({LineKind::NoNewline, std::string()});
                    break;
                default:
                    /* Fewer body lines than declared: end the hunk and revisit
                     * this line as a header or metadata. */
                    active_hunk = nullptr;
                    --i;
                    break;
            }
            continue;
        }

        /* A trailing "\ No newline at end of file" note belongs to the hunk we
         * have just finished consuming. */
        if (active_hunk != nullptr && !line.empty() && line[0] == '\\') {
            active_hunk->lines.push_back({LineKind::NoNewline, std::string()});
            continue;
        }
        active_hunk = nullptr;  /* the active hunk (if any) is now complete */

        if (line.substr(0, kOldFileHeader.size()) == kOldFileHeader) {
            pending_old = clean_diff_path(line.substr(kOldFileHeader.size()));
            pending_new.clear();
        } else if (line.substr(0, kNewFileHeader.size()) == kNewFileHeader) {
            pending_new = clean_diff_path(line.substr(kNewFileHeader.size()));
            if (!begin_file()) {
                return false;
            }
        } else if (line.substr(0, kHunkHeader.size()) == kHunkHeader) {
            if (!have_current) {
                error = "hunk header encountered before any file header";
                return false;
            }
            Hunk hunk;
            if (!parse_hunk_header(line, hunk)) {
                error = "malformed hunk header: " + std::string(line);
                return false;
            }
            current.hunks().push_back(std::move(hunk));
            active_hunk = &current.hunks().back();
            need_orig = active_hunk->orig_count;
            need_new = active_hunk->new_count;
        }
        /* Anything else outside a hunk is git metadata (diff --git, index,
         * mode, rename/copy, binary markers, ...) and is ignored. */
    }

    flush_current();
    return finalize(error);  /* also rejects a file targeted by two sections */
}

bool PatchSet::parse_ed_bundle(const std::string& diff_text, const std::string& base_dir,
                               std::string& error) {
    bool ends_nl = false;
    const std::vector<string_view> lines = split_lines(diff_text, ends_nl);

    FilePatch current;
    current.make_ed();
    std::string current_key;  /* canonical path of `current`, the index key */
    bool have_current = false;

    EdCommand pending;
    bool in_input = false;

    const auto flush_current = [&]() {
        if (have_current && !current.ed().empty()) {
            index_.emplace_back(std::move(current_key), std::move(current));
        }
        current = FilePatch();
        current.make_ed();
        current_key.clear();
        have_current = false;
    };

    for (const string_view line : lines) {
        /* Inside an a/c/i text block: collect lines until a lone "." */
        if (in_input) {
            if (line.size() == 1 && line[0] == kEdInputTerminator) {
                current.ed().push_back(std::move(pending));
                pending = EdCommand();
                in_input = false;
            } else {
                pending.lines.emplace_back(line);
            }
            continue;
        }

        if (line.substr(0, kEdFileHeader.size()) == kEdFileHeader) {
            const string_view path = rtrim(line.substr(kEdFileHeader.size()));
            const std::size_t s = path.find_first_not_of(" \t");
            if (s == npos) {
                error = "empty '# file:' header in ed bundle";
                return false;
            }
            flush_current();
            current_key = canonicalize(std::string(path.substr(s)), base_dir);
            have_current = true;
            continue;
        }

        /* Ignore comments, the magic line, blank separators, and the trailing
         * ed write/quit commands. */
        const std::size_t first = line.find_first_not_of(" \t\r");
        if (first == npos || line[first] == '#') {
            continue;
        }
        const string_view trimmed = rtrim(line.substr(first));
        if (trimmed == "w" || trimmed == "q") {
            continue;
        }

        if (!have_current) {
            error = "ed command before any '# file:' header";
            return false;
        }

        EdCommand cmd;
        bool needs_input = false;
        if (!parse_ed_command(line, cmd, needs_input)) {
            error = "invalid ed command: " + std::string(line);
            return false;
        }
        if (needs_input) {
            pending = std::move(cmd);
            in_input = true;
        } else {
            current.ed().push_back(std::move(cmd));
        }
    }

    if (in_input) {
        error = "unterminated ed input block (missing '.')";
        return false;
    }

    flush_current();
    return finalize(error);  /* also rejects a file targeted by two sections */
}

const FilePatch* PatchSet::find(std::string_view canonical_path) const {
    const auto it = std::lower_bound(
        index_.begin(), index_.end(), canonical_path,
        [](const Entry& e, std::string_view key) { return string_view(e.first) < key; });
    if (it != index_.end() && string_view(it->first) == canonical_path) {
        return &it->second;
    }
    return nullptr;
}

bool PatchSet::has_basename(std::string_view basename) const {
    return std::binary_search(
        basenames_.begin(), basenames_.end(), basename,
        [](string_view a, string_view b) { return a < b; });
}

bool PatchSet::apply(const FilePatch& fp, const std::string& original,
                     std::string& out, std::string& error) {
    if (fp.is_ed()) {
        return apply_ed_script(fp, original, out, error);
    }

    bool orig_ends_nl = false;
    const std::vector<string_view> src = split_lines(original, orig_ends_nl);

    std::vector<string_view> result;
    result.reserve(src.size() + 16);

    std::size_t cursor = 0;        /* 0-based read position in `src`            */
    bool consumed_to_eof = false;  /* did the last hunk reach the end of `src`? */
    bool new_eof_no_nl = false;    /* should the rebuilt EOF omit its newline?  */

    for (const Hunk& hunk : fp.hunks()) {
        /* Where this hunk starts in the original (0-based). A pure insertion
         * (orig_count == 0) starts after `orig_start` lines. */
        const std::int64_t raw_start = (hunk.orig_count == 0) ? hunk.orig_start : hunk.orig_start - 1;
        const auto start_index = static_cast<std::size_t>(raw_start < 0 ? 0 : raw_start);

        if (start_index < cursor) {
            error = "overlapping or out-of-order hunks";
            return false;
        }
        if (start_index > src.size()) {
            error = "hunk starts beyond end of original file";
            return false;
        }

        for (; cursor < start_index; ++cursor) {  /* copy lines before the hunk */
            result.push_back(src[cursor]);
        }

        LineKind prev_kind = LineKind::Context;
        for (const DiffLine& dl : hunk.lines) {
            const string_view text(dl.text);
            switch (dl.kind) {
                case LineKind::Context:
                    if (cursor >= src.size() || src[cursor] != text) {
                        error = "context mismatch at original line " + std::to_string(cursor + 1);
                        return false;
                    }
                    result.push_back(src[cursor++]);
                    new_eof_no_nl = false;
                    break;
                case LineKind::Remove:
                    if (cursor >= src.size() || src[cursor] != text) {
                        error = "removal mismatch at original line " + std::to_string(cursor + 1);
                        return false;
                    }
                    ++cursor;
                    break;
                case LineKind::Add:
                    result.push_back(text);
                    new_eof_no_nl = false;
                    break;
                case LineKind::NoNewline:
                    /* The note refers to the side of the immediately preceding line. */
                    if (prev_kind == LineKind::Add || prev_kind == LineKind::Context) {
                        new_eof_no_nl = true;
                    }
                    break;
            }
            prev_kind = dl.kind;
        }

        consumed_to_eof = (cursor >= src.size());
    }

    for (; cursor < src.size(); ++cursor) {  /* copy lines after the last hunk */
        result.push_back(src[cursor]);
        consumed_to_eof = false;
    }

    const bool trailing_newline = consumed_to_eof ? !new_eof_no_nl : orig_ends_nl;
    join_lines(result, trailing_newline, out);
    return true;
}

}  // namespace phpatcher
