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

static void test_rejects_unified_diff() {
    PatchSet ps;
    std::string error;
    const bool ok = ps.parse("--- a/file.php\n+++ b/file.php\n@@ -1 +1 @@\n-old\n+new\n", ".", error);
    assert(!ok);
    assert(error.find("phpatcher-ed") != std::string::npos);
}

static void test_append_change_delete_insert() {
    std::string out;
    apply_one(bundle("4c\nFOUR\n.\n3d\n2a\nTWO-HALF\n.\n1c\nONE\n.\n1i\nZERO\n.\nw\nq\n"),
              "one\ntwo\ndrop\nfour\n", out);
    assert(out == "ZERO\nONE\ntwo\nTWO-HALF\nFOUR\n");
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
    test_ed_native_r_reference_resolves();
    test_ed_native_r_reference_trim_pad_resolves();
    test_missing_resolver_fails();
    test_invalid_native_r_reference_stays_literal();
    test_duplicate_file_rejected();
    std::cout << "test_patch OK\n";
    return 0;
}
