#include "../../patch.hpp"

#include <cassert>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using phpatcher::EdRef;
using phpatcher::PatchSet;

static std::string bundle(std::string_view body) {
    return "# phpatcher-ed\n# file: file.php\n" + std::string(body);
}

static std::string r_directive(std::string_view path, int begin, int end,
                               std::string_view guard = "s:0") {
    return "r \"" + std::string(path) + "\" " + std::to_string(begin) + " " +
           std::to_string(end) + " # " + std::string(guard);
}

static std::string r_directive_pad(std::string_view path, int line,
                                   std::string_view lpad, std::string_view rpad,
                                   std::string_view guard = "s:0") {
    return "r \"" + std::string(path) + "\" " + std::to_string(line) + " " +
           std::to_string(line) + " \"" + std::string(lpad) + "\" \"" +
           std::string(rpad) + "\" # " + std::string(guard);
}

static bool parse_one(const std::string& text, PatchSet& ps, std::string& error) {
    return ps.parse(text, ".", error);
}

static void apply_one(const std::string& patch, const std::string& original,
                      std::string& out) {
    PatchSet ps;
    std::string error;
    assert(parse_one(patch, ps, error));
    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);
    assert(PatchSet::apply(*fp, original, out, error));
}

/* True if the patch parses but apply() rejects it; `error` is the apply error. */
static bool apply_rejects(const std::string& patch, const std::string& original,
                          std::string& error) {
    PatchSet ps;
    std::string parse_error;
    assert(parse_one(patch, ps, parse_error));
    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);
    std::string out;
    return !PatchSet::apply(*fp, original, out, error);
}

/* True if parse() itself rejects the text; `error` is the parse error. */
static bool parse_rejects(const std::string& text, std::string& error) {
    PatchSet ps;
    return !ps.parse(text, ".", error);
}

static void test_rejects_unified_diff() {
    PatchSet ps;
    std::string error;
    const bool ok = ps.parse("--- a/file.php\n+++ b/file.php\n@@ -1 +1 @@\n-old\n+new\n", ".", error);
    assert(!ok);
    assert(error.find("phpatcher-ed") != std::string::npos);
}

static void test_append_change_delete_insert() {
    /* Exercise all four commands in one bundle, in the strictly descending,
     * non-overlapping order phpatcher's generators emit (and apply requires). */
    std::string out;
    apply_one(bundle("5c\nFIVE\n.\n4d\n2a\nTWO-HALF\n.\n1i\nZERO\n.\nw\nq\n"),
              "one\ntwo\nthree\nfour\nfive\n", out);
    assert(out == "ZERO\none\ntwo\nTWO-HALF\nthree\nFIVE\n");
}

static void test_rejects_out_of_order_commands() {
    /* Ascending order (1c then 3c): rejected even though these two happen not to
     * overlap, because the format mandates descending order. */
    std::string error;
    assert(apply_rejects(bundle("1c\nA\n.\n3c\nB\n.\n"), "l1\nl2\nl3\n", error));
    assert(error.find("descending") != std::string::npos);
}

static void test_rejects_overlapping_commands() {
    /* Descending by start, but the ranges overlap on line 5. */
    std::string error;
    assert(apply_rejects(bundle("5,6c\nX\n.\n4,5d\n"), "a\nb\nc\nd\ne\nf\n", error));
    assert(error.find("descending") != std::string::npos);
}

static void test_delete_out_of_range() {
    std::string error;
    assert(apply_rejects(bundle("5d\n"), "a\nb\n", error));
    assert(error.find("delete out of range") != std::string::npos);
}

static void test_change_out_of_range() {
    std::string error;
    assert(apply_rejects(bundle("5c\nX\n.\n"), "a\nb\n", error));
    assert(error.find("change out of range") != std::string::npos);
}

static void test_append_out_of_range() {
    std::string error;
    assert(apply_rejects(bundle("5a\nX\n.\n"), "a\nb\n", error));
    assert(error.find("append out of range") != std::string::npos);
}

static void test_insert_out_of_range() {
    std::string error;
    assert(apply_rejects(bundle("5i\nX\n.\n"), "a\nb\n", error));
    assert(error.find("insert out of range") != std::string::npos);
}

static void test_parse_invalid_command() {
    std::string error;
    assert(parse_rejects(bundle("not-a-command\n"), error));
    assert(error.find("invalid ed command") != std::string::npos);
}

static void test_parse_command_before_header() {
    std::string error;
    assert(parse_rejects("# phpatcher-ed\n1d\n", error));
    assert(error.find("before any '# file:'") != std::string::npos);
}

static void test_parse_empty_file_header() {
    std::string error;
    assert(parse_rejects("# phpatcher-ed\n# file:   \n1d\n", error));
    assert(error.find("empty '# file:'") != std::string::npos);
}

static void test_parse_unterminated_input() {
    std::string error;
    assert(parse_rejects(bundle("0a\nsome line\n"), error));
    assert(error.find("unterminated") != std::string::npos);
}

static void test_ed_native_r_reference_resolves() {
    PatchSet ps;
    std::string error;
    const std::string patch = bundle("0a\n" + r_directive("corpus.php", 2, 3) + "\n.\nw\nq\n");
    assert(parse_one(patch, ps, error));

    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);

    std::string out;
    bool saw_ref = false;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string& err) {
        assert(ref.path == "corpus.php");
        assert(ref.begin == 2);
        assert(ref.end == 3);
        assert(ref.transform == EdRef::Transform::Exact);
        assert(ref.bytes == 0);
        assert(!ref.has_hash);
        lines = {"two", "three"};
        saw_ref = true;
        err.clear();
        return true;
    };

    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(saw_ref);
    assert(out == "two\nthree\n");
}

static void test_ed_native_r_reference_trim_pad_resolves() {
    PatchSet ps;
    std::string error;
    const std::string patch = bundle("0a\n" + r_directive_pad("corpus.php", 7, "\t", " // copied", "s:18") + "\n.\nw\nq\n");
    assert(parse_one(patch, ps, error));

    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);

    std::string out;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string&) {
        assert(ref.transform == EdRef::Transform::TrimPad);
        assert(ref.lpad == "\t");
        assert(ref.rpad == " // copied");
        assert(ref.bytes == 18);
        lines = {"    while (true)"};
        return true;
    };

    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(out == "\twhile (true) // copied\n");
}

static void test_missing_resolver_fails() {
    PatchSet ps;
    std::string error;
    const std::string patch = bundle("0a\n" + r_directive("corpus.php", 1, 1, "s:5") + "\n.\nw\nq\n");
    assert(parse_one(patch, ps, error));

    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);
    std::string out;
    assert(!PatchSet::apply(*fp, "", out, error));
    assert(error.find("no resolver") != std::string::npos);
}

static void test_invalid_native_r_reference_stays_literal() {
    std::string out;
    apply_one(bundle("0a\nr \"corpus.php\" 1 2 # missing-guard\n.\nw\nq\n"), "", out);
    assert(out == "r \"corpus.php\" 1 2 # missing-guard\n");
}

static void test_preserves_no_trailing_newline() {
    /* A file without a trailing newline must stay that way after a patch. */
    std::string out;
    apply_one(bundle("2c\nB\n.\n"), "a\nb", out);
    assert(out == "a\nB");
}

static void test_preserves_crlf_on_untouched_lines() {
    /* CRLF line endings on lines the patch does not rewrite are kept verbatim;
     * only the inserted literal uses a bare LF (the patch author's bytes). */
    std::string out;
    apply_one(bundle("1a\nINS\n.\n"), "a\r\nb\r\n", out);
    assert(out == "a\r\nINS\nb\r\n");
}

static void test_r_unquoted_path_resolves() {
    PatchSet ps;
    std::string error;
    assert(parse_one(bundle("0a\nr corpus.php 2 3 # s:0\n.\n"), ps, error));
    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);

    std::string out;
    bool saw = false;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string&) {
        assert(ref.path == "corpus.php");
        assert(ref.begin == 2);
        assert(ref.end == 3);
        lines = {"two", "three"};
        saw = true;
        return true;
    };
    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(saw);
    assert(out == "two\nthree\n");
}

static void test_r_escapes_decoded() {
    /* Patch text (runtime): r "a\"b.php" 5 5 "\t\\" "x\ny" # s:0
     * which decodes to path a"b.php, lpad <tab><backslash>, rpad x<newline>y. */
    PatchSet ps;
    std::string error;
    const std::string patch =
        bundle("0a\nr \"a\\\"b.php\" 5 5 \"\\t\\\\\" \"x\\ny\" # s:0\n.\n");
    assert(parse_one(patch, ps, error));
    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);

    std::string out;
    bool saw = false;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string&) {
        assert(ref.path == std::string("a\"b.php"));
        assert(ref.lpad == std::string("\t\\"));
        assert(ref.rpad == std::string("x\ny"));
        assert(ref.transform == EdRef::Transform::TrimPad);
        lines = {"core"};
        saw = true;
        return true;
    };
    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(saw);
}

static void test_r_both_guards_parsed() {
    PatchSet ps;
    std::string error;
    const std::string patch =
        bundle("0a\nr \"corpus.php\" 1 1 # s:5 h:000000000000000000000000000000ab\n.\n");
    assert(parse_one(patch, ps, error));
    const auto* fp = ps.find(PatchSet::canonicalize("file.php", "."));
    assert(fp != nullptr);

    std::string out;
    bool saw = false;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string&) {
        assert(ref.bytes == 5);
        assert(ref.has_hash);
        lines = {"hello"};
        saw = true;
        return true;
    };
    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(saw);
}

static void test_newfile_creates_in_memory() {
    /* A "# newfile:" section declares a file built from nothing: creates=true and
     * apply() against an empty original yields the section's content verbatim. */
    PatchSet ps;
    std::string error;
    const std::string patch =
        "# phpatcher-ed\n# newfile: created.php\n<?php\necho 1;\n.\n";
    assert(ps.parse(patch, ".", error));
    const auto* fp = ps.find(PatchSet::canonicalize("created.php", "."));
    assert(fp != nullptr);
    assert(fp->creates);
    std::string out;
    assert(PatchSet::apply(*fp, "", out, error));
    assert(out == "<?php\necho 1;\n");
}

static void test_newfile_empty_is_empty_file() {
    PatchSet ps;
    std::string error;
    assert(ps.parse("# phpatcher-ed\n# newfile: empty.php\n.\n", ".", error));
    const auto* fp = ps.find(PatchSet::canonicalize("empty.php", "."));
    assert(fp != nullptr);
    assert(fp->creates);
    std::string out;
    assert(PatchSet::apply(*fp, "", out, error));
    assert(out.empty());
}

static void test_newfile_with_corpus_ref() {
    /* A created file can reference corpus runs just like a change block does. */
    PatchSet ps;
    std::string error;
    const std::string patch =
        "# phpatcher-ed\n# newfile: t.php\n<?php\n" + r_directive("corpus.php", 1, 2) + "\n.\n";
    assert(ps.parse(patch, ".", error));
    const auto* fp = ps.find(PatchSet::canonicalize("t.php", "."));
    assert(fp != nullptr);
    assert(fp->creates);

    std::string out;
    const auto resolver = [&](const EdRef& ref, std::vector<std::string>& lines, std::string&) {
        assert(ref.path == "corpus.php");
        lines = {"trait T {", "}"};
        return true;
    };
    assert(PatchSet::apply(*fp, "", out, error, resolver));
    assert(out == "<?php\ntrait T {\n}\n");
}

static void test_newfile_duplicate_target_rejected() {
    /* The same canonical path cannot be both edited and created. */
    PatchSet ps;
    std::string error;
    const bool ok = ps.parse(
        "# phpatcher-ed\n# file: dup.php\n0a\nx\n.\n# newfile: dup.php\ny\n.\n", ".", error);
    assert(!ok);
    assert(error.find("duplicate") != std::string::npos);
}

static void test_newfile_empty_header_rejected() {
    std::string error;
    assert(parse_rejects("# phpatcher-ed\n# newfile:\n.\n", error));
    assert(error.find("newfile") != std::string::npos);
}

static void test_duplicate_file_rejected() {
    PatchSet ps;
    std::string error;
    const bool ok = ps.parse("# phpatcher-ed\n# file: file.php\n0a\none\n.\n# file: file.php\n0a\ntwo\n.\n",
                             ".", error);
    assert(!ok);
    assert(error.find("duplicate") != std::string::npos);
}

int main() {
    test_rejects_unified_diff();
    test_append_change_delete_insert();
    test_rejects_out_of_order_commands();
    test_rejects_overlapping_commands();
    test_delete_out_of_range();
    test_change_out_of_range();
    test_append_out_of_range();
    test_insert_out_of_range();
    test_parse_invalid_command();
    test_parse_command_before_header();
    test_parse_empty_file_header();
    test_parse_unterminated_input();
    test_ed_native_r_reference_resolves();
    test_ed_native_r_reference_trim_pad_resolves();
    test_missing_resolver_fails();
    test_invalid_native_r_reference_stays_literal();
    test_preserves_no_trailing_newline();
    test_preserves_crlf_on_untouched_lines();
    test_r_unquoted_path_resolves();
    test_r_escapes_decoded();
    test_r_both_guards_parsed();
    test_newfile_creates_in_memory();
    test_newfile_empty_is_empty_file();
    test_newfile_with_corpus_ref();
    test_newfile_duplicate_target_rejected();
    test_newfile_empty_header_rejected();
    test_duplicate_file_rejected();
    std::cout << "test_patch OK\n";
    return 0;
}
