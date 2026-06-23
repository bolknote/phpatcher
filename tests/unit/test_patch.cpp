/*
 * Standalone unit tests for the phpatcher patch core (no PHP required).
 * Build & run via: tests/unit/run.sh  (or `make` in this directory).
 */
#include "../../patch.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <unistd.h>

using phpatcher::FilePatch;
using phpatcher::PatchSet;

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

std::string g_tmpdir;

std::string make_tmpdir() {
    char tmpl[] = "/tmp/phpatcher_testXXXXXX";
    char *d = mkdtemp(tmpl);
    if (!d) {
        std::perror("mkdtemp");
        std::exit(2);
    }
    return std::string(d);
}

void write_file(const std::string &path, const std::string &content) {
    std::ofstream f(path, std::ios::binary);
    f << content;
}

/* Apply a diff (relative to g_tmpdir) to `original` written at `relpath`. */
bool run_patch(const std::string &relpath, const std::string &original,
               const std::string &diff, std::string &out, std::string &err) {
    const std::string abs = g_tmpdir + "/" + relpath;
    write_file(abs, original);

    PatchSet ps;
    std::string perr;
    if (!ps.parse(diff, g_tmpdir, perr)) {
        err = "parse failed: " + perr;
        return false;
    }
    const std::string key = PatchSet::canonicalize(relpath, g_tmpdir);
    const FilePatch *fp = ps.find(key);
    if (!fp) {
        err = "patch not found for key " + key;
        return false;
    }
    return PatchSet::apply(*fp, original, out, err);
}

}  // namespace

static void test_parse_index() {
    const std::string diff =
        "diff --git a/foo.php b/foo.php\n"
        "index 1111111..2222222 100644\n"
        "--- a/foo.php\n"
        "+++ b/foo.php\n"
        "@@ -1,1 +1,1 @@\n"
        "-echo 1;\n"
        "+echo 2;\n";
    write_file(g_tmpdir + "/foo.php", "echo 1;\n");

    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(1));
    const std::string key = PatchSet::canonicalize("foo.php", g_tmpdir);
    CHECK(ps.find(key) != nullptr);
    CHECK(ps.find("/nonexistent/path.php") == nullptr);
}

static void test_replace_line() {
    std::string out, err;
    const std::string diff =
        "--- a/a.php\n+++ b/a.php\n"
        "@@ -1,3 +1,3 @@\n"
        " line1\n"
        "-line2\n"
        "+LINE2\n"
        " line3\n";
    CHECK(run_patch("a.php", "line1\nline2\nline3\n", diff, out, err));
    CHECK_EQ(out, std::string("line1\nLINE2\nline3\n"));
}

static void test_insertion_at_start() {
    std::string out, err;
    const std::string diff =
        "--- a/b.php\n+++ b/b.php\n"
        "@@ -0,0 +1,1 @@\n"
        "+<?php\n";
    CHECK(run_patch("b.php", "echo 1;\n", diff, out, err));
    CHECK_EQ(out, std::string("<?php\necho 1;\n"));
}

static void test_pure_deletion() {
    std::string out, err;
    const std::string diff =
        "--- a/c.php\n+++ b/c.php\n"
        "@@ -1,3 +1,2 @@\n"
        " keep1\n"
        "-remove\n"
        " keep2\n";
    CHECK(run_patch("c.php", "keep1\nremove\nkeep2\n", diff, out, err));
    CHECK_EQ(out, std::string("keep1\nkeep2\n"));
}

static void test_append_at_eof() {
    std::string out, err;
    const std::string diff =
        "--- a/d.php\n+++ b/d.php\n"
        "@@ -2,1 +2,2 @@\n"
        " second\n"
        "+third\n";
    CHECK(run_patch("d.php", "first\nsecond\n", diff, out, err));
    CHECK_EQ(out, std::string("first\nsecond\nthird\n"));
}

static void test_multi_hunk() {
    std::string out, err;
    const std::string diff =
        "--- a/e.php\n+++ b/e.php\n"
        "@@ -1,2 +1,2 @@\n"
        "-a\n"
        "+A\n"
        " b\n"
        "@@ -4,2 +4,2 @@\n"
        " d\n"
        "-e\n"
        "+E\n";
    CHECK(run_patch("e.php", "a\nb\nc\nd\ne\n", diff, out, err));
    CHECK_EQ(out, std::string("A\nb\nc\nd\nE\n"));
}

static void test_no_newline_at_eof() {
    std::string out, err;
    /* Original has no trailing newline; patched also has none. */
    const std::string diff =
        "--- a/f.php\n+++ b/f.php\n"
        "@@ -1,1 +1,1 @@\n"
        "-old\n"
        "\\ No newline at end of file\n"
        "+new\n"
        "\\ No newline at end of file\n";
    CHECK(run_patch("f.php", "old", diff, out, err));
    CHECK_EQ(out, std::string("new"));
}

static void test_context_mismatch_fails() {
    std::string out, err;
    const std::string diff =
        "--- a/g.php\n+++ b/g.php\n"
        "@@ -1,2 +1,2 @@\n"
        " expected\n"
        "-old\n"
        "+new\n";
    /* File content does not match the context line -> must fail. */
    CHECK(!run_patch("g.php", "different\nold\n", diff, out, err));
}

static void test_hunk_count_omitted() {
    std::string out, err;
    /* "@@ -1 +1 @@" form (counts omitted, default 1). */
    const std::string diff =
        "--- a/h.php\n+++ b/h.php\n"
        "@@ -1 +1 @@\n"
        "-x\n"
        "+y\n";
    CHECK(run_patch("h.php", "x\n", diff, out, err));
    CHECK_EQ(out, std::string("y\n"));
}

static void test_multiple_files() {
    write_file(g_tmpdir + "/m1.php", "1\n");
    write_file(g_tmpdir + "/m2.php", "2\n");
    const std::string diff =
        "diff --git a/m1.php b/m1.php\n"
        "--- a/m1.php\n+++ b/m1.php\n"
        "@@ -1 +1 @@\n-1\n+one\n"
        "diff --git a/m2.php b/m2.php\n"
        "--- a/m2.php\n+++ b/m2.php\n"
        "@@ -1 +1 @@\n-2\n+two\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(2));

    std::string out;
    const FilePatch *f1 = ps.find(PatchSet::canonicalize("m1.php", g_tmpdir));
    const FilePatch *f2 = ps.find(PatchSet::canonicalize("m2.php", g_tmpdir));
    CHECK(f1 && f2);
    CHECK(PatchSet::apply(*f1, "1\n", out, err));
    CHECK_EQ(out, std::string("one\n"));
    CHECK(PatchSet::apply(*f2, "2\n", out, err));
    CHECK_EQ(out, std::string("two\n"));
}

static void test_ed_change_line() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: a.php\n"
        "2c\n"
        "LINE2\n"
        ".\n";
    CHECK(run_patch("a.php", "line1\nline2\nline3\n", diff, out, err));
    CHECK_EQ(out, std::string("line1\nLINE2\nline3\n"));
}

static void test_ed_append() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: b.php\n"
        "1a\n"
        "inserted\n"
        ".\n";
    CHECK(run_patch("b.php", "x\ny\n", diff, out, err));
    CHECK_EQ(out, std::string("x\ninserted\ny\n"));
}

static void test_ed_append_at_start() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: b0.php\n"
        "0a\n"
        "<?php\n"
        ".\n";
    CHECK(run_patch("b0.php", "echo 1;\n", diff, out, err));
    CHECK_EQ(out, std::string("<?php\necho 1;\n"));
}

static void test_ed_delete() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: c.php\n"
        "2d\n";
    CHECK(run_patch("c.php", "a\nb\nc\n", diff, out, err));
    CHECK_EQ(out, std::string("a\nc\n"));
}

static void test_ed_range_change() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: r.php\n"
        "2,3c\n"
        "X\n"
        "Y\n"
        "Z\n"
        ".\n";
    CHECK(run_patch("r.php", "a\nb\nc\nd\n", diff, out, err));
    CHECK_EQ(out, std::string("a\nX\nY\nZ\nd\n"));
}

static void test_ed_reverse_order_multi() {
    /* diff -e emits commands highest-line-first so they apply in sequence. */
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: e.php\n"
        "5c\n"
        "E\n"
        ".\n"
        "2c\n"
        "B\n"
        ".\n";
    CHECK(run_patch("e.php", "a\nb\nc\nd\ne\n", diff, out, err));
    CHECK_EQ(out, std::string("a\nB\nc\nd\nE\n"));
}

static void test_ed_no_leaked_original() {
    /* The ed bundle must not contain the original/removed text at all. */
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: leak.php\n"
        "1c\n"
        "new secret\n"
        ".\n";
    CHECK(diff.find("old secret") == std::string::npos);
    std::string out, err;
    CHECK(run_patch("leak.php", "old secret\n", diff, out, err));
    CHECK_EQ(out, std::string("new secret\n"));
}

static void test_ed_multi_file() {
    write_file(g_tmpdir + "/em1.php", "1\n");
    write_file(g_tmpdir + "/em2.php", "2\n");
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: em1.php\n"
        "1c\n"
        "one\n"
        ".\n"
        "# file: em2.php\n"
        "1c\n"
        "two\n"
        ".\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(2));

    std::string out;
    const FilePatch *f1 = ps.find(PatchSet::canonicalize("em1.php", g_tmpdir));
    const FilePatch *f2 = ps.find(PatchSet::canonicalize("em2.php", g_tmpdir));
    CHECK(f1 && f2);
    CHECK(f1->is_ed() && f2->is_ed());
    CHECK(PatchSet::apply(*f1, "1\n", out, err));
    CHECK_EQ(out, std::string("one\n"));
    CHECK(PatchSet::apply(*f2, "2\n", out, err));
    CHECK_EQ(out, std::string("two\n"));
}

static void test_ed_bad_command_fails() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: bad.php\n"
        "totally not a command\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

/* Regression: a removed/added line whose content itself begins with "-- " or
 * "++ " (so the diff line reads "--- ..."/"+++ ...") must be treated as hunk
 * body, never as a new file header. */
static void test_body_line_looks_like_header() {
    std::string out, err;
    const std::string diff =
        "--- a/hdr.php\n+++ b/hdr.php\n"
        "@@ -1,3 +1,3 @@\n"
        " header\n"
        "--- old dashes\n"
        "+++ new pluses\n"
        " footer\n";
    CHECK(run_patch("hdr.php", "header\n-- old dashes\nfooter\n", diff, out, err));
    CHECK_EQ(out, std::string("header\n++ new pluses\nfooter\n"));
}

/* A "@@"-looking added line inside the body must also stay in the hunk. */
static void test_body_line_looks_like_hunk_marker() {
    std::string out, err;
    const std::string diff =
        "--- a/atat.php\n+++ b/atat.php\n"
        "@@ -1,1 +1,2 @@\n"
        " keep\n"
        "+@@ not a header\n";
    CHECK(run_patch("atat.php", "keep\n", diff, out, err));
    CHECK_EQ(out, std::string("keep\n@@ not a header\n"));
}

static void test_empty_context_line_in_hunk() {
    std::string out, err;
    /* The middle context line is blank (represented as an empty line). */
    const std::string diff =
        "--- a/blank.php\n+++ b/blank.php\n"
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "\n"
        "-c\n"
        "+C\n";
    CHECK(run_patch("blank.php", "a\n\nc\n", diff, out, err));
    CHECK_EQ(out, std::string("a\n\nC\n"));
}

static void test_overlapping_hunks_fail() {
    const std::string diff =
        "--- a/ov.php\n+++ b/ov.php\n"
        "@@ -2,1 +2,1 @@\n-b\n+B\n"
        "@@ -1,1 +1,1 @@\n-a\n+A\n";
    std::string out, err;
    CHECK(!run_patch("ov.php", "a\nb\nc\n", diff, out, err));
}

static void test_hunk_beyond_eof_fail() {
    const std::string diff =
        "--- a/be.php\n+++ b/be.php\n"
        "@@ -10,1 +10,1 @@\n-x\n+y\n";
    std::string out, err;
    CHECK(!run_patch("be.php", "a\n", diff, out, err));
}

static void test_removal_mismatch_fail() {
    const std::string diff =
        "--- a/rm.php\n+++ b/rm.php\n"
        "@@ -1,1 +1,1 @@\n-nope\n+y\n";
    std::string out, err;
    CHECK(!run_patch("rm.php", "actual\n", diff, out, err));
}

static void test_hunk_before_file_header_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "@@ -1,1 +1,1 @@\n-a\n+b\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_malformed_hunk_header_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "--- a/mh.php\n+++ b/mh.php\n"
        "@@ this is not a hunk header @@\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_devnull_paths_fail() {
    PatchSet ps;
    std::string err;
    /* Both sides /dev/null: no usable target file path. */
    const std::string diff =
        "--- /dev/null\n+++ /dev/null\n"
        "@@ -0,0 +1,1 @@\n+x\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_clean_path_with_timestamp() {
    /* git/diff sometimes append a tab-separated timestamp after the path. */
    write_file(g_tmpdir + "/ts.php", "1\n");
    const std::string diff =
        "--- a/ts.php\t2020-01-01 00:00:00.000000000 +0000\n"
        "+++ b/ts.php\t2020-01-02 00:00:00.000000000 +0000\n"
        "@@ -1 +1 @@\n-1\n+one\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK(ps.find(PatchSet::canonicalize("ts.php", g_tmpdir)) != nullptr);
}

static void test_empty_diff_noop() {
    PatchSet ps;
    std::string err;
    CHECK(ps.parse("", g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(0));
}

static void test_ed_insert() {
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ins.php\n"
        "2i\n"
        "X\n"
        ".\n";
    CHECK(run_patch("ins.php", "a\nb\nc\n", diff, out, err));
    CHECK_EQ(out, std::string("a\nX\nb\nc\n"));
}

static void test_ed_build_from_empty() {
    /* Appending to an empty original yields a newline-terminated file. */
    std::string out, err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: fromempty.php\n"
        "0a\n"
        "<?php\n"
        ".\n";
    CHECK(run_patch("fromempty.php", "", diff, out, err));
    CHECK_EQ(out, std::string("<?php\n"));
}

static void test_ed_out_of_range_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: oor.php\n"
        "9d\n";
    write_file(g_tmpdir + "/oor.php", "a\nb\n");
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("oor.php", g_tmpdir));
    CHECK(fp != nullptr);
    std::string out;
    CHECK(!PatchSet::apply(*fp, "a\nb\n", out, err));
}

static void test_ed_change_out_of_range_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: cor.php\n"
        "5,9c\n"
        "X\n"
        ".\n";
    write_file(g_tmpdir + "/cor.php", "a\nb\n");
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("cor.php", g_tmpdir));
    CHECK(fp != nullptr);
    std::string out;
    CHECK(!PatchSet::apply(*fp, "a\nb\n", out, err));
}

static void test_ed_unterminated_input_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: unterm.php\n"
        "1a\n"
        "no terminator follows\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_empty_file_header_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file:    \n"
        "1d\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_command_before_file_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "1d\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_overflow_command_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: of.php\n"
        "999999999999999999999999999999d\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_bundle_with_leading_comments() {
    /* Blank lines and comments may precede the magic line. */
    std::string out, err;
    const std::string diff =
        "\n"
        "   \n"
        "# phpatcher-ed v1\n"
        "# a comment\n"
        "# file: lead.php\n"
        "w\n"
        "1c\n"
        "ONE\n"
        ".\n"
        "q\n";
    CHECK(run_patch("lead.php", "one\n", diff, out, err));
    CHECK_EQ(out, std::string("ONE\n"));
}

static void test_read_file() {
    const std::string path = g_tmpdir + "/rf.txt";
    write_file(path, "hello\nworld\n");
    std::string out;
    CHECK(phpatcher::read_file(path, out));
    CHECK_EQ(out, std::string("hello\nworld\n"));
    std::string missing;
    CHECK(!phpatcher::read_file(g_tmpdir + "/does-not-exist", missing));
}

static void test_canonicalize_nonexistent_relative() {
    /* A path that does not exist still yields a stable absolute key. */
    const std::string key = PatchSet::canonicalize("no/such/file.php", g_tmpdir);
    CHECK(!key.empty());
    CHECK(key[0] == '/');
    /* With an empty base_dir a relative, non-existent path is made absolute
     * against the process cwd. */
    const std::string key2 = PatchSet::canonicalize("no/such/file.php", std::string());
    CHECK(!key2.empty());
    CHECK(key2[0] == '/');
}

static void test_hunk_header_no_minus_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "--- a/n.php\n+++ b/n.php\n@@ +1,1 @@\n+x\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_hunk_header_garbage_numbers_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "--- a/n2.php\n+++ b/n2.php\n@@ -x +y @@\n+x\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_bad_second_number_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff =
        "# phpatcher-ed v1\n# file: b2.php\n1,x c\nX\n.\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_no_op_char_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "# phpatcher-ed v1\n# file: noop.php\n12\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_trailing_junk_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "# phpatcher-ed v1\n# file: tj.php\n1d extra\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_unknown_op_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "# phpatcher-ed v1\n# file: uo.php\n1x\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_ed_append_out_of_range_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "# phpatcher-ed v1\n# file: aor.php\n5a\nX\n.\n";
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("aor.php", g_tmpdir));
    CHECK(fp != nullptr);
    std::string out;
    CHECK(!PatchSet::apply(*fp, "a\nb\n", out, err));
}

static void test_ed_insert_out_of_range_fail() {
    PatchSet ps;
    std::string err;
    const std::string diff = "# phpatcher-ed v1\n# file: ior.php\n9i\nX\n.\n";
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("ior.php", g_tmpdir));
    CHECK(fp != nullptr);
    std::string out;
    CHECK(!PatchSet::apply(*fp, "a\nb\n", out, err));
}

/* File deletion: the new side is /dev/null, so the old path is the target. */
static void test_delete_side_uses_old_path() {
    write_file(g_tmpdir + "/del.php", "a\nb\n");
    const std::string diff =
        "--- a/del.php\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-a\n-b\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("del.php", g_tmpdir));
    CHECK(fp != nullptr);
    std::string out;
    CHECK(PatchSet::apply(*fp, "a\nb\n", out, err));
    CHECK_EQ(out, std::string(""));
}

/* A hunk that declares more body lines than are actually present: the parser
 * must stop at the next header rather than over-consume. */
static void test_short_hunk_then_next_file() {
    write_file(g_tmpdir + "/sh1.php", "a\n");
    write_file(g_tmpdir + "/sh2.php", "b\n");
    const std::string diff =
        "--- a/sh1.php\n+++ b/sh1.php\n@@ -1,1 +1,1 @@\n-a\n+A\n"
        "--- a/sh2.php\n+++ b/sh2.php\n@@ -1,1 +1,1 @@\n-b\n+B\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(2));
}

/* apply() must work on a hand-built FilePatch assembled through the typed API
 * (default-constructed FilePatch is a unified-diff patch). */
static void test_apply_hand_built_hunk() {
    FilePatch fp;  /* defaults to the unified-diff (hunks) representation */
    phpatcher::Hunk h;
    h.orig_start = 1;
    h.orig_count = 1;
    h.new_start = 1;
    h.new_count = 1;
    h.lines.push_back({phpatcher::LineKind::Context, "a"});
    fp.hunks().push_back(std::move(h));
    std::string out, err;
    CHECK(PatchSet::apply(fp, "a\n", out, err));
    CHECK_EQ(out, std::string("a\n"));
}

static void test_clean_path_trailing_space() {
    /* Trailing spaces on a path field (no tab) must be trimmed. */
    write_file(g_tmpdir + "/sp.php", "1\n");
    const std::string diff =
        "--- a/sp.php  \n+++ b/sp.php  \n@@ -1 +1 @@\n-1\n+one\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK(ps.find(PatchSet::canonicalize("sp.php", g_tmpdir)) != nullptr);
}

static void test_read_empty_file() {
    const std::string path = g_tmpdir + "/empty.txt";
    write_file(path, "");
    std::string out = "stale";
    CHECK(phpatcher::read_file(path, out));
    CHECK_EQ(out, std::string(""));
}

static void test_short_hunk_underrun() {
    /* A hunk that declares more body lines than are present must end at the
     * next non-body line rather than swallow it. */
    write_file(g_tmpdir + "/ur.php", "a\n");
    const std::string diff =
        "--- a/ur.php\n+++ b/ur.php\n@@ -1,3 +1,3 @@\n-a\n+A\n"
        "diff --git a/ur.php b/ur.php\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    CHECK_EQ(ps.file_count(), static_cast<std::size_t>(1));
}

static void test_basename_of() {
    CHECK_EQ(PatchSet::basename_of("/a/b/c.php"), std::string("c.php"));
    CHECK_EQ(PatchSet::basename_of("c.php"), std::string("c.php"));
    CHECK_EQ(PatchSet::basename_of("/a/b/"), std::string(""));
    CHECK_EQ(PatchSet::basename_of(""), std::string(""));
}

/* ---- corpus references (ed "!sed" directives) ---------------------------- */

/* A resolver backed by an in-memory corpus, mirroring the real glue: it
 * validates the range and verifies whichever guard the reference carries — the
 * byte length (s:) or the content hash (h:). */
static phpatcher::PatchSet::RefResolver make_test_resolver(
    const std::unordered_map<std::string, std::vector<std::string>> &corpus) {
    return [&corpus](const phpatcher::EdRef &ref, std::vector<std::string> &out,
                     std::string &error) -> bool {
        auto it = corpus.find(ref.path);
        if (it == corpus.end()) { error = "no such file"; return false; }
        const std::vector<std::string> &lines = it->second;
        if (ref.begin < 1 || ref.end < ref.begin ||
            ref.end > static_cast<std::int64_t>(lines.size())) {
            error = "range out of bounds";
            return false;
        }
        std::string bytes;
        std::int64_t total = 0;
        out.clear();
        for (std::int64_t ln = ref.begin; ln <= ref.end; ++ln) {
            const std::string &l = lines[static_cast<std::size_t>(ln - 1)];
            total += static_cast<std::int64_t>(l.size()) + 1;
            if (ref.has_hash) { bytes += l; bytes += '\n'; }
            out.push_back(l);
        }
        if (ref.bytes >= 0 && total != ref.bytes) { error = "length mismatch"; out.clear(); return false; }
        if (ref.has_hash && phpatcher::ref_hash(bytes) != ref.hash) {
            error = "hash mismatch";
            out.clear();
            return false;
        }
        return true;
    };
}

/* Build a "!sed -n 'a,b p;b q' path  # s:LEN" directive (length guard, default)
 * for the given exact bytes. */
static std::string ref_directive(const std::string &path, int a, int b, const std::string &bytes) {
    return "!sed -n '" + std::to_string(a) + "," + std::to_string(b) + " p;" +
           std::to_string(b) + " q' " + path + "  # s:" + std::to_string(bytes.size());
}

/* Build a "!sed -n 'a,b p;b q' path  # h:HASH" directive (hash guard). */
static std::string ref_directive_hash(const std::string &path, int a, int b,
                                      const std::string &bytes) {
    return "!sed -n '" + std::to_string(a) + "," + std::to_string(b) + " p;" +
           std::to_string(b) + " q' " + path +
           "  # h:" + phpatcher::ref_hash_hex(phpatcher::ref_hash(bytes));
}

static std::string r_directive_hash(const std::string &path, int a, int b,
                                    const std::string &bytes) {
    return "r \"" + path + "\" " + std::to_string(a) + " " + std::to_string(b) +
           " # h:" + phpatcher::ref_hash_hex(phpatcher::ref_hash(bytes));
}

static std::string r_directive_trim_pad_hash(const std::string &path, int a, int b,
                                             const std::string &lpad,
                                             const std::string &rpad,
                                             const std::string &bytes) {
    return "r \"" + path + "\" " + std::to_string(a) + " " + std::to_string(b) +
           " \"" + lpad + "\" \"" + rpad + "\" # h:" +
           phpatcher::ref_hash_hex(phpatcher::ref_hash(bytes));
}

static bool run_patch_ref(const std::string &relpath, const std::string &original,
                          const std::string &diff, const phpatcher::PatchSet::RefResolver &resolve,
                          std::string &out, std::string &err) {
    const std::string abs = g_tmpdir + "/" + relpath;
    write_file(abs, original);
    PatchSet ps;
    std::string perr;
    if (!ps.parse(diff, g_tmpdir, perr)) { err = "parse failed: " + perr; return false; }
    const FilePatch *fp = ps.find(PatchSet::canonicalize(relpath, g_tmpdir));
    if (!fp) { err = "patch not found"; return false; }
    return PatchSet::apply(*fp, original, out, err, resolve);
}

static void test_ed_reference_resolves() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref1.php\n"
        "0a\n" +
        ref_directive("src/x.php", 1, 3, "A\nB\nC\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("ref1.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("A\nB\nC\norig\n"));
}

static void test_ed_reference_mixed_with_literal() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref2.php\n"
        "0a\n" +
        ref_directive("src/x.php", 1, 2, "A\nB\n") + "\n"
        "LITERAL\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("ref2.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("A\nB\nLITERAL\norig\n"));
}

static void test_ed_reference_length_mismatch_fails() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    /* s: encodes a length that does not match the corpus content. */
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref3.php\n"
        "0a\n" +
        ref_directive("src/x.php", 1, 2, "WRONGER\nBYTES\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(!run_patch_ref("ref3.php", "orig\n", diff, make_test_resolver(corpus), out, err));
}

static void test_ed_reference_hash_resolves() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref3h.php\n"
        "0a\n" +
        ref_directive_hash("src/x.php", 1, 3, "A\nB\nC\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("ref3h.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("A\nB\nC\norig\n"));
}

static void test_ed_native_r_reference_resolves() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: refr.php\n"
        "0a\n" +
        r_directive_hash("src/x.php", 1, 2, "A\nB\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("refr.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("A\nB\norig\n"));
}

static void test_ed_native_r_reference_trim_pad_resolves() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"    foo();   "}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: refpad.php\n"
        "0a\n" +
        r_directive_trim_pad_hash("src/x.php", 1, 1, "\t", " // moved",
                                  "    foo();   \n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("refpad.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("\tfoo(); // moved\norig\n"));
}

static void test_ed_reference_hash_mismatch_fails() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    /* Same byte length as the corpus run ("A\nB\n"), different content: a length
     * guard would pass here, but the hash guard catches it. */
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref3m.php\n"
        "0a\n" +
        ref_directive_hash("src/x.php", 1, 2, "X\nY\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(!run_patch_ref("ref3m.php", "orig\n", diff, make_test_resolver(corpus), out, err));
}

/* The sed early-exit ";B q" is optional: a directive without it still parses. */
static void test_ed_reference_without_q_resolves() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B", "C"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: refq.php\n"
        "0a\n"
        "!sed -n '1,3 p' src/x.php  # s:6\n"  /* "A\nB\nC\n" = 6 bytes, no ;q */
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("refq.php", "orig\n", diff, make_test_resolver(corpus), out, err));
    CHECK_EQ(out, std::string("A\nB\nC\norig\n"));
}

static void test_ed_reference_out_of_range_fails() {
    std::unordered_map<std::string, std::vector<std::string>> corpus = {
        {"src/x.php", {"A", "B"}}};
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref4.php\n"
        "0a\n" +
        ref_directive("src/x.php", 1, 5, "A\nB\n") + "\n"
        ".\n";
    std::string out, err;
    CHECK(!run_patch_ref("ref4.php", "orig\n", diff, make_test_resolver(corpus), out, err));
}

static void test_ed_reference_without_resolver_fails() {
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref5.php\n"
        "0a\n" +
        ref_directive("src/x.php", 1, 2, "A\nB\n") + "\n"
        ".\n";
    std::string out, err;
    /* A patch with a reference but no resolver must fail, not silently drop it. */
    CHECK(!run_patch_ref("ref5.php", "orig\n", diff, {}, out, err));
}

/* A line that merely starts with "!sed" but is not a complete directive must be
 * kept as a literal line (no resolver needed). */
static void test_ed_reference_literal_passthrough() {
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref6.php\n"
        "0a\n"
        "!sed is not a directive here\n"
        ".\n";
    std::string out, err;
    CHECK(run_patch_ref("ref6.php", "orig\n", diff, {}, out, err));
    CHECK_EQ(out, std::string("!sed is not a directive here\norig\n"));
}

/* The parser must decode the directive into a typed EdRef with the right
 * fields. */
static void test_ed_reference_parsed_fields() {
    const std::string diff =
        "# phpatcher-ed v1\n"
        "# file: ref7.php\n"
        "0a\n" +
        ref_directive("a/b/c.php", 12, 34, "x\n") + "\n"
        ".\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    const FilePatch *fp = ps.find(PatchSet::canonicalize("ref7.php", g_tmpdir));
    CHECK(fp != nullptr);
    if (fp && fp->is_ed() && fp->ed().size() == 1 && fp->ed()[0].input.size() == 1) {
        const phpatcher::EdPiece &piece = fp->ed()[0].input[0];
        CHECK(std::holds_alternative<phpatcher::EdRef>(piece));
        if (std::holds_alternative<phpatcher::EdRef>(piece)) {
            const phpatcher::EdRef &ref = std::get<phpatcher::EdRef>(piece);
            CHECK_EQ(ref.path, std::string("a/b/c.php"));
            CHECK_EQ(ref.begin, static_cast<std::int64_t>(12));
            CHECK_EQ(ref.end, static_cast<std::int64_t>(34));
            /* ref_directive emits a length (s:) guard, no hash. */
            CHECK_EQ(ref.bytes, static_cast<std::int64_t>(2));  /* "x\n" */
            CHECK(!ref.has_hash);
            CHECK_EQ(ref.transform, phpatcher::EdRef::Exact);
        }
    } else {
        CHECK(false);
    }
}

static void test_duplicate_section_fails() {
    /* Two sections targeting the same file must be rejected at parse time. */
    PatchSet ps;
    std::string err;
    const std::string diff =
        "--- a/dup.php\n+++ b/dup.php\n@@ -1 +1 @@\n-1\n+one\n"
        "--- a/dup.php\n+++ b/dup.php\n@@ -1 +1 @@\n-1\n+uno\n";
    CHECK(!ps.parse(diff, g_tmpdir, err));
}

static void test_basename_index() {
    write_file(g_tmpdir + "/bn.php", "echo 1;\n");
    const std::string diff =
        "--- a/bn.php\n+++ b/bn.php\n@@ -1 +1 @@\n-echo 1;\n+echo 2;\n";
    PatchSet ps;
    std::string err;
    CHECK(ps.parse(diff, g_tmpdir, err));
    /* The patched file's basename must be indexed; unrelated names must not. */
    CHECK(ps.has_basename("bn.php"));
    CHECK(!ps.has_basename("other.php"));
}

int main() {
    g_tmpdir = make_tmpdir();

    test_parse_index();
    test_replace_line();
    test_insertion_at_start();
    test_pure_deletion();
    test_append_at_eof();
    test_multi_hunk();
    test_no_newline_at_eof();
    test_context_mismatch_fails();
    test_hunk_count_omitted();
    test_multiple_files();

    test_ed_change_line();
    test_ed_append();
    test_ed_append_at_start();
    test_ed_delete();
    test_ed_range_change();
    test_ed_reverse_order_multi();
    test_ed_no_leaked_original();
    test_ed_multi_file();
    test_ed_bad_command_fails();

    test_body_line_looks_like_header();
    test_body_line_looks_like_hunk_marker();
    test_empty_context_line_in_hunk();
    test_overlapping_hunks_fail();
    test_hunk_beyond_eof_fail();
    test_removal_mismatch_fail();
    test_hunk_before_file_header_fail();
    test_malformed_hunk_header_fail();
    test_devnull_paths_fail();
    test_clean_path_with_timestamp();
    test_empty_diff_noop();

    test_ed_insert();
    test_ed_build_from_empty();
    test_ed_out_of_range_fail();
    test_ed_change_out_of_range_fail();
    test_ed_unterminated_input_fail();
    test_ed_empty_file_header_fail();
    test_ed_command_before_file_fail();
    test_ed_overflow_command_fail();
    test_ed_bundle_with_leading_comments();

    test_read_file();
    test_canonicalize_nonexistent_relative();

    test_hunk_header_no_minus_fail();
    test_hunk_header_garbage_numbers_fail();
    test_ed_bad_second_number_fail();
    test_ed_no_op_char_fail();
    test_ed_trailing_junk_fail();
    test_ed_unknown_op_fail();
    test_ed_append_out_of_range_fail();
    test_ed_insert_out_of_range_fail();
    test_delete_side_uses_old_path();
    test_short_hunk_then_next_file();
    test_apply_hand_built_hunk();

    test_clean_path_trailing_space();
    test_read_empty_file();
    test_short_hunk_underrun();
    test_basename_of();
    test_duplicate_section_fails();
    test_basename_index();

    test_ed_reference_resolves();
    test_ed_reference_mixed_with_literal();
    test_ed_reference_length_mismatch_fails();
    test_ed_reference_hash_resolves();
    test_ed_native_r_reference_resolves();
    test_ed_native_r_reference_trim_pad_resolves();
    test_ed_reference_hash_mismatch_fails();
    test_ed_reference_without_q_resolves();
    test_ed_reference_out_of_range_fails();
    test_ed_reference_without_resolver_fails();
    test_ed_reference_literal_passthrough();
    test_ed_reference_parsed_fields();

    /* Best-effort cleanup. */
    std::string cmd = "rm -rf '" + g_tmpdir + "'";
    if (std::system(cmd.c_str()) != 0) {
        std::fprintf(stderr, "warning: cleanup of %s failed\n", g_tmpdir.c_str());
    }

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
