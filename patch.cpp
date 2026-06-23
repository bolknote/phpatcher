/*
 * phpatcher - in-memory phpatcher-ed patching core.
 *
 * Deliberately free of any PHP/Zend dependency so it can be unit-tested in
 * isolation. The two entry points are PatchSet::parse() (build an index from a
 * patch file) and PatchSet::apply() (produce patched content for one file).
 *
 * Performance notes:
 *   - Parsing and application work over std::string_view slices of the caller's
 *     buffers; only the data that must outlive the call (ed input blocks, index
 *     keys, the final output) is materialised into owning std::strings.
 *   - The index is a flat array sorted by canonical path; lookups are O(log n)
 *     via binary search, and the contiguous storage is cache- and COW-friendly.
 */
#include "patch.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace phpatcher {
namespace {

namespace fs = std::filesystem;

using std::string_view;
constexpr std::size_t npos = string_view::npos;

/* Tokens recognised in the phpatcher-ed format. */
constexpr string_view kEdMagic = "# phpatcher-ed";  /* phpatcher-ed selector    */
constexpr string_view kEdFileHeader = "# file:";    /* per-file section marker  */
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

string_view trim_blanks(string_view s) {
    while (!s.empty() && is_blank(s.front())) {
        s.remove_prefix(1);
    }
    return rtrim(s);
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

/* True if the patch text is one of our phpatcher-ed bundles (its first non-blank
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

/* Parse a whole string_view as a non-negative decimal. Returns false unless the
 * entire token is digits and fits in int64. */
bool parse_decimal_full(string_view s, std::int64_t& out) {
    if (s.empty()) return false;
    std::int64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') return false;
        if (v > (INT64_MAX - (c - '0')) / 10) return false;  /* overflow */
        v = v * 10 + (c - '0');
    }
    out = v;
    return true;
}

bool parse_quoted(string_view s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        const char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= s.size()) return false;
            const char e = s[pos++];
            switch (e) {
                case '\\': out.push_back('\\'); break;
                case '"':  out.push_back('"'); break;
                case 't':  out.push_back('\t'); break;
                case 'r':  out.push_back('\r'); break;
                case 'n':  out.push_back('\n'); break;
                default:   return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

bool parse_guard_tokens(string_view meta, EdRef& ref) {
    bool have_guard = false;
    while (!meta.empty()) {
        const std::size_t sp = meta.find(' ');
        const string_view tok = meta.substr(0, sp);
        if (tok.substr(0, 2) == "s:") {
            if (!parse_decimal_full(tok.substr(2), ref.bytes)) return false;
            have_guard = true;
        } else if (tok.substr(0, 2) == "h:") {
            if (!ref_hash_parse(tok.substr(2), ref.hash)) return false;
            ref.has_hash = true;
            have_guard = true;
        } else if (!tok.empty()) {
            return false;  /* unknown token: not our directive */
        }
        if (sp == npos) break;
        meta.remove_prefix(sp + 1);
    }
    return have_guard;
}

void skip_blanks(string_view s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t')) ++pos;
}

bool parse_r_directive(string_view line, EdRef& ref) {
    if (line.substr(0, 2) != "r ") return false;
    std::size_t pos = 2;
    skip_blanks(line, pos);

    std::string path;
    if (pos < line.size() && line[pos] == '"') {
        if (!parse_quoted(line, pos, path)) return false;
    } else {
        const std::size_t start = pos;
        while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
        if (pos == start) return false;
        path = std::string(line.substr(start, pos - start));
    }

    skip_blanks(line, pos);
    std::int64_t begin = 0, end = 0;
    if (!read_uint(line, pos, begin)) return false;
    skip_blanks(line, pos);
    if (!read_uint(line, pos, end)) return false;
    if (begin < 1 || end < begin) return false;

    ref = EdRef{};
    ref.path = std::move(path);
    ref.begin = begin;
    ref.end = end;

    skip_blanks(line, pos);
    if (pos < line.size() && line[pos] == '"') {
        if (!parse_quoted(line, pos, ref.lpad)) return false;
        skip_blanks(line, pos);
        if (!parse_quoted(line, pos, ref.rpad)) return false;
        ref.transform = EdRef::TrimPad;
        skip_blanks(line, pos);
    }

    if (pos + 2 > line.size() || line.substr(pos, 2) != "# ") return false;
    return parse_guard_tokens(line.substr(pos + 2), ref);
}

/* Parse one phpatcher-ed input-block line into a piece. A line is treated as a
 * corpus reference only if it fully matches the native phpatcher form
 *
 *     r "PATH" A B ["lpad" "rpad"] # <guard>
 *
 * <guard> is space-separated key:value tokens, at least one of which must be
 * "s:<len>" (byte length) or "h:<32 hex>" (content hash). Anything that
 * deviates (including ordinary code that merely starts with "r ") is kept
 * verbatim as a literal line. */
EdPiece parse_ed_input_piece(string_view line) {
    const auto literal = [&]() { return EdPiece(std::string(line)); };

    EdRef native_ref;
    if (parse_r_directive(line, native_ref)) {
        return EdPiece(std::move(native_ref));
    }
    return literal();
}

/* Expand an ed input block into concrete line views. Literal pieces reference
 * the patch's own storage; resolved reference pieces are owned by `storage`
 * (a deque, so the views stay valid as more are appended). */
bool build_input(const std::vector<EdPiece>& input, const PatchSet::RefResolver& resolve,
                 std::deque<std::string>& storage, std::vector<string_view>& out,
                 std::string& error) {
    for (const EdPiece& piece : input) {
        if (const std::string* lit = std::get_if<std::string>(&piece)) {
            out.emplace_back(*lit);
            continue;
        }
        const EdRef& ref = std::get<EdRef>(piece);
        if (!resolve) {
            error = "patch references corpus lines but no resolver is available";
            return false;
        }
        std::vector<std::string> resolved;
        if (!resolve(ref, resolved, error)) {
            return false;
        }
        for (std::string& l : resolved) {
            if (ref.transform == EdRef::TrimPad) {
                std::string t = ref.lpad;
                const string_view core = trim_blanks(l);
                t.append(core.data(), core.size());
                t += ref.rpad;
                storage.push_back(std::move(t));
            } else {
                storage.push_back(std::move(l));
            }
            out.emplace_back(storage.back());
        }
    }
    return true;
}

/* Verify the commands address strictly descending, non-overlapping ranges in the
 * *original* file. phpatcher's format mandates this (its generators always emit
 * it) precisely so that applying the commands sequentially to the mutating buffer
 * is identical to addressing the original line numbers. Any other ordering is
 * order-dependent: applied verbatim it would silently produce different output
 * than intended, so we reject it (fail-closed) rather than guess.
 *
 * For each command we track the lowest original line affected so far (`barrier`)
 * and require the next command's highest addressed line to sit strictly below it. */
bool validate_command_order(const FilePatch& fp, std::string& error) {
    std::int64_t barrier = INT64_MAX;  /* lowest original line touched so far */
    for (const EdCommand& c : fp.commands) {
        std::int64_t addressed_top = 0;  /* highest original line it needs in place  */
        std::int64_t affected_low = 0;   /* lowest original line it consumes/shifts   */
        switch (c.kind) {
            case EdCommand::Delete:
            case EdCommand::Change:
                addressed_top = c.end;       /* needs lines c.start..c.end present */
                affected_low = c.start;      /* consumes from c.start upward       */
                break;
            case EdCommand::Append:          /* inserts after line c.start (c.start may be 0) */
                addressed_top = c.start;     /* anchor line must be in place       */
                affected_low = c.start + 1;  /* shifts everything after the anchor */
                break;
            case EdCommand::Insert:          /* inserts before line c.start */
                addressed_top = c.start;     /* anchor line must be in place       */
                affected_low = c.start;      /* shifts the anchor and below-after  */
                break;
        }
        if (addressed_top >= barrier) {
            error = "ed commands must be in descending, non-overlapping line order";
            return false;
        }
        if (affected_low < barrier) {
            barrier = affected_low;
        }
    }
    return true;
}

/* Apply a phpatcher-ed FilePatch to `original`. Corpus references are expanded via
 * `resolve`; an ed script without references does not need one. */
bool apply_ed_script(const FilePatch& fp, string_view original, std::string& out,
                     std::string& error, const PatchSet::RefResolver& resolve) {
    if (!validate_command_order(fp, error)) {
        return false;
    }

    bool ends_nl = false;
    std::vector<string_view> lines = split_lines(original, ends_nl);
    if (original.empty()) {
        ends_nl = true;  /* a file built from nothing should be newline-terminated */
    }

    std::deque<std::string> storage;  /* owns resolved reference lines */

    const auto out_of_range = [&](const char* what) {
        error = what;
        return false;
    };

    for (const EdCommand& c : fp.commands) {
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
                std::vector<string_view> ins;
                if (!build_input(c.input, resolve, storage, ins, error)) return false;
                auto at = lines.erase(lines.begin() + (c.start - 1), lines.begin() + c.end);
                lines.insert(at, ins.begin(), ins.end());
                break;
            }

            case EdCommand::Append: {
                if (c.start < 0 || c.start > size) {
                    return out_of_range("ed append out of range");
                }
                std::vector<string_view> ins;
                if (!build_input(c.input, resolve, storage, ins, error)) return false;
                lines.insert(lines.begin() + c.start, ins.begin(), ins.end());
                break;
            }

            case EdCommand::Insert: {
                if (c.start < 1 || c.start > size + 1) {
                    return out_of_range("ed insert out of range");
                }
                std::vector<string_view> ins;
                if (!build_input(c.input, resolve, storage, ins, error)) return false;
                lines.insert(lines.begin() + (c.start - 1), ins.begin(), ins.end());
                break;
            }
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

    if (diff_text.empty()) {
        return true;
    }
    if (!looks_like_ed_bundle(diff_text)) {
        error = "unsupported patch format: expected '# phpatcher-ed'";
        return false;
    }
    return parse_ed_bundle(diff_text, base_dir, error);
}

bool PatchSet::parse_ed_bundle(const std::string& diff_text, const std::string& base_dir,
                               std::string& error) {
    bool ends_nl = false;
    const std::vector<string_view> lines = split_lines(diff_text, ends_nl);

    FilePatch current;
    std::string current_key;  /* canonical path of `current`, the index key */
    bool have_current = false;

    EdCommand pending;
    bool in_input = false;

    const auto flush_current = [&]() {
        if (have_current && !current.commands.empty()) {
            index_.emplace_back(std::move(current_key), std::move(current));
        }
        current = FilePatch();
        current_key.clear();
        have_current = false;
    };

    for (const string_view line : lines) {
        /* Inside an a/c/i text block: collect lines until a lone "." */
        if (in_input) {
            if (line.size() == 1 && line[0] == kEdInputTerminator) {
                current.commands.push_back(std::move(pending));
                pending = EdCommand();
                in_input = false;
            } else {
                pending.input.push_back(parse_ed_input_piece(line));
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
            current.commands.push_back(std::move(cmd));
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
                     std::string& out, std::string& error, const RefResolver& resolve) {
    return apply_ed_script(fp, original, out, error, resolve);
}

}  // namespace phpatcher
