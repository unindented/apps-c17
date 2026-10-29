#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "testing/test_support.h"
#include "vcs/changeset.h"
#include "vcs/repository.h"
#include "vcs/test_vcs_support.h"

/**
 * @brief Diffs a commit of a fixture repository.
 *
 * @param name          Terminated name of the fixture, such as `showcase`. Must not be `NULL`.
 * @param commit_oid    Terminated hex id of the commit. Must not be `NULL`.
 * @param limits        The limits. Must not be `NULL`.
 * @param arena         Arena that owns the changes. Must not be `NULL`.
 * @param changeset_out Receives the changes. Must not be `NULL`.
 * @return `0` when the commit was diffed, or `-1` otherwise.
 */
static int load_commit(const char* name,
                       const char* commit_oid,
                       const struct SurgeLimits* limits,
                       struct Arena* arena,
                       struct Changeset* changeset_out) {
  struct Repository repository = {0};
  struct Log log = test_log();
  const bool is_loaded = open_fixture(&repository, name, arena) == 0 &&
                         changeset_load(changeset_out, &repository, str_from_cstr(commit_oid),
                                        limits, arena, &log) == 0;
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
  return is_loaded ? 0 : -1;
}

/**
 * @brief Returns limits that cap nothing in the fixture.
 *
 * @return The limits.
 */
static struct SurgeLimits wide_limits(void) {
  return (struct SurgeLimits){
      .blob_len_max = 1024 * 1024,
      .diff_file_count_max = 100,
      .diff_line_count_max = 10000,
      .diff_len_max = 1024 * 1024,
  };
}

// A root commit adds every file, in the changed-files tree's order, with its lines.
static void test_load_diffs_a_root_commit_against_nothing(void) {
  struct Arena arena = test_arena();
  const struct SurgeLimits limits = wide_limits();
  struct Changeset changeset = {0};
  TEST_ASSERT(load_commit("showcase", "7ed6f9f821dad694bd213cde09fcf1558e505dee", &limits, &arena,
                          &changeset) == 0);
  TEST_ASSERT(changeset.file_count == 2);
  TEST_CHECK(test_text_is(changeset.files[0].new_path, "LICENSE"));
  TEST_CHECK(test_text_is(changeset.files[1].new_path, "README.md"));
  TEST_CHECK(changeset.files[1].status == CHANGE_ADDED && changeset.files[1].old_path.len == 0);
  TEST_CHECK(changeset.files[1].hunk_count == 1 && changeset.files[1].additions == 3);
  TEST_CHECK(changeset.files[1].lines[0].origin == '+' &&
             changeset.files[1].lines[0].new_line == 1);
  TEST_CHECK(test_text_is(changeset.files[1].lines[0].text, "# Showcase"));
  TEST_CHECK(!changeset.is_truncated && changeset.deletions == 0);
}

// Directories come before files at each level, binary files show no lines, and an exact move is a
// rename.
static void test_load_orders_files_and_finds_binaries_and_renames(void) {
  struct Arena arena = test_arena();
  const struct SurgeLimits limits = wide_limits();
  struct Changeset images = {0};
  struct Changeset rename = {0};
  TEST_ASSERT(load_commit("showcase", "8d028c5c6a8323cf091b48aaf86542b633a9986f", &limits, &arena,
                          &images) == 0);
  TEST_ASSERT(load_commit("showcase", "f5ee58f15ba34190e53faee44824e024b57bf1c4", &limits, &arena,
                          &rename) == 0);
  TEST_ASSERT(images.file_count == 4);
  const char* paths[] = {"media/dot.png", "media/logo.svg", "README.md", "data.bin"};
  for (ptrdiff_t i = 0; i < 4; i++) {
    TEST_CHECK(test_text_is(images.files[i].new_path, "%s", paths[i]));
  }
  TEST_CHECK(images.files[0].is_binary && images.files[3].is_binary && !images.files[1].is_binary);
  TEST_CHECK(images.files[2].status == CHANGE_MODIFIED && images.files[2].deletions == 1);
  TEST_ASSERT(rename.file_count == 1);
  TEST_CHECK(rename.files[0].status == CHANGE_RENAMED && rename.files[0].line_count == 0);
  TEST_CHECK(test_text_is(rename.files[0].old_path, "docs/guide.md"));
  TEST_CHECK(test_text_is(rename.files[0].new_path, "docs/manual.md"));
  TEST_CHECK(rename.files[0].similarity == 100);
}

// A move that also edits the file is a rename too, as git finds it, with how alike the sides are
// and the diff of the edit.
static void test_load_finds_a_rename_that_edits_the_file(void) {
  struct Arena arena = test_arena();
  const struct SurgeLimits limits = wide_limits();
  struct Changeset changeset = {0};
  TEST_ASSERT(load_commit("changes", "6abce5d1f2acea455f90f499bc8a09487749ba19", &limits, &arena,
                          &changeset) == 0);
  TEST_ASSERT(changeset.file_count == 1);
  const struct ChangedFile* file = &changeset.files[0];
  TEST_CHECK(file->status == CHANGE_RENAMED);
  TEST_CHECK(test_text_is(file->old_path, "two-hunks.txt"));
  TEST_CHECK(test_text_is(file->new_path, "moved.txt"));
  TEST_CHECK(file->similarity >= 50 && file->similarity < 100);
  TEST_MSG("similarity: %td", file->similarity);
  TEST_CHECK(file->additions == 1 && file->deletions == 1);
}

// The line cap cuts the file that passes it short, and the file cap leaves later files unshown.
static void test_load_applies_the_caps_in_order(void) {
  struct Arena arena = test_arena();
  struct SurgeLimits limits = wide_limits();
  limits.diff_line_count_max = 50;
  struct Changeset table = {0};
  TEST_ASSERT(load_commit("showcase", "aa02d29453a240bab99ef9357b50480a5ada392e", &limits, &arena,
                          &table) == 0);
  TEST_ASSERT(table.file_count == 1);
  TEST_CHECK(table.files[0].is_shown && table.files[0].is_truncated);
  TEST_CHECK(table.files[0].line_count == 50 && table.additions == 50 && table.is_truncated);
  limits = wide_limits();
  limits.diff_file_count_max = 1;
  struct Changeset root = {0};
  TEST_ASSERT(load_commit("showcase", "7ed6f9f821dad694bd213cde09fcf1558e505dee", &limits, &arena,
                          &root) == 0);
  TEST_ASSERT(root.file_count == 2);
  TEST_CHECK(root.files[0].is_shown && !root.files[1].is_shown && root.files[1].line_count == 0);
  TEST_CHECK(root.is_truncated);
}

// The byte cap cuts a file short at the line that would pass it.
static void test_load_applies_the_byte_cap(void) {
  struct Arena arena = test_arena();
  struct SurgeLimits limits = wide_limits();
  // Each line of `lib/table.c` is 16 bytes with its newline, so 100 bytes hold six of them.
  limits.diff_len_max = 100;
  struct Changeset table = {0};
  TEST_ASSERT(load_commit("showcase", "aa02d29453a240bab99ef9357b50480a5ada392e", &limits, &arena,
                          &table) == 0);
  TEST_ASSERT(table.file_count == 1);
  TEST_CHECK(table.files[0].is_truncated && table.files[0].line_count == 6);
  TEST_CHECK(table.is_truncated);
}

// A file after the one the line cap cut short is listed without its diff.
static void test_load_leaves_files_after_the_line_cap_unshown(void) {
  struct Arena arena = test_arena();
  struct SurgeLimits limits = wide_limits();
  limits.diff_line_count_max = 1;
  struct Changeset root = {0};
  TEST_ASSERT(load_commit("showcase", "7ed6f9f821dad694bd213cde09fcf1558e505dee", &limits, &arena,
                          &root) == 0);
  TEST_ASSERT(root.file_count == 2);
  TEST_CHECK(root.files[0].is_shown && root.files[0].is_truncated);
  TEST_CHECK(root.files[0].line_count == 1 && root.files[0].hunk_count == 1);
  TEST_CHECK(!root.files[1].is_shown && root.files[1].line_count == 0);
}

// A file whose blob is over `blob_len_max` is too large, not binary, and shows no lines.
static void test_load_tells_large_files_from_binary_ones(void) {
  struct Arena arena = test_arena();
  struct SurgeLimits limits = wide_limits();
  limits.blob_len_max = 50;
  struct Changeset images = {0};
  TEST_ASSERT(load_commit("showcase", "8d028c5c6a8323cf091b48aaf86542b633a9986f", &limits, &arena,
                          &images) == 0);
  TEST_ASSERT(images.file_count == 4);
  TEST_CHECK(images.files[1].is_too_large && !images.files[1].is_binary);
  TEST_CHECK(images.files[1].line_count == 0);
  TEST_CHECK(images.files[3].is_binary && !images.files[3].is_too_large);
}

// The row that says a side has no final newline carries no line number of its own.
static void test_load_numbers_no_newline_markers_nowhere(void) {
  struct Arena arena = test_arena();
  const struct SurgeLimits limits = wide_limits();
  struct Changeset names = {0};
  TEST_ASSERT(load_commit("showcase", "119edc43de569e78adf5af0a0608e005771ac3ca", &limits, &arena,
                          &names) == 0);
  TEST_ASSERT(names.file_count == 3);
  const struct ChangedFile* link = &names.files[2];
  TEST_ASSERT(test_text_is(link->new_path, "link-to-readme") && link->line_count == 2);
  TEST_CHECK(link->lines[0].origin == '+' && link->lines[0].new_line == 1);
  TEST_CHECK(link->lines[1].origin == '\\' && link->lines[1].old_line == 0 &&
             link->lines[1].new_line == 0);
}

// A deleted file has no new side and shows its lines as deletions, and a file that becomes a
// symlink is one type change, not a deletion and an addition.
static void test_load_reports_deletions_and_type_changes(void) {
  struct Arena arena = test_arena();
  const struct SurgeLimits limits = wide_limits();
  struct Changeset changeset = {0};
  TEST_ASSERT(load_commit("changes", "cd8013f50156d818625d47dc854e7c14e6c80100", &limits, &arena,
                          &changeset) == 0);
  TEST_ASSERT(changeset.file_count == 2);
  const struct ChangedFile* gone = &changeset.files[0];
  TEST_CHECK(gone->status == CHANGE_DELETED && test_text_is(gone->old_path, "gone.txt"));
  TEST_CHECK(gone->new_path.len == 0 && gone->new_oid.len == 0);
  TEST_CHECK(gone->deletions == 1 && gone->additions == 0);
  TEST_CHECK(gone->line_count == 1 && test_text_is(gone->lines[0].text, "This file goes away."));
  const struct ChangedFile* kind = &changeset.files[1];
  TEST_CHECK(kind->status == CHANGE_TYPE_CHANGED);
  TEST_CHECK(test_text_is(kind->old_path, "kind.txt") && test_text_is(kind->new_path, "kind.txt"));
}

// Each hunk of a file keeps its own header and lines, and one that the line cap reaches at its
// first line is dropped, not listed without lines.
static void test_load_drops_a_hunk_the_line_cap_reaches_at_its_first_line(void) {
  struct Arena arena = test_arena();
  struct SurgeLimits limits = wide_limits();
  struct Changeset whole = {0};
  TEST_ASSERT(load_commit("changes", "10a01232accd45ce1a52a9524aea0e998eb8b4c5", &limits, &arena,
                          &whole) == 0);
  TEST_ASSERT(whole.file_count == 1 && whole.files[0].hunk_count == 2);
  const struct ChangedFile* file = &whole.files[0];
  TEST_CHECK(test_text_is(file->hunks[0].header, "@@ -1,4 +1,4 @@"));
  TEST_CHECK(test_text_is(file->hunks[1].header, "@@ -17,4 +17,4 @@ line 16"));
  TEST_CHECK(file->hunks[1].line_index == 5 && file->hunks[1].line_count == 5);
  TEST_CHECK(test_text_is(file->lines[file->hunks[1].line_index].text, "line 17"));
  TEST_CHECK(!whole.is_truncated);
  limits.diff_line_count_max = 5;
  struct Changeset capped = {0};
  TEST_ASSERT(load_commit("changes", "10a01232accd45ce1a52a9524aea0e998eb8b4c5", &limits, &arena,
                          &capped) == 0);
  TEST_ASSERT(capped.file_count == 1);
  TEST_CHECK(capped.files[0].hunk_count == 1 && capped.files[0].line_count == 5);
  TEST_CHECK(capped.files[0].is_truncated && capped.is_truncated);
}

TEST_LIST = {
    {"load diffs a root commit against nothing", test_load_diffs_a_root_commit_against_nothing},
    {"load orders files and finds binaries and renames",
     test_load_orders_files_and_finds_binaries_and_renames},
    {"load finds a rename that edits the file", test_load_finds_a_rename_that_edits_the_file},
    {"load applies the caps in order", test_load_applies_the_caps_in_order},
    {"load applies the byte cap", test_load_applies_the_byte_cap},
    {"load leaves files after the line cap unshown",
     test_load_leaves_files_after_the_line_cap_unshown},
    {"load tells large files from binary ones", test_load_tells_large_files_from_binary_ones},
    {"load numbers no newline markers nowhere", test_load_numbers_no_newline_markers_nowhere},
    {"load reports deletions and type changes", test_load_reports_deletions_and_type_changes},
    {"load drops a hunk the line cap reaches at its first line",
     test_load_drops_a_hunk_the_line_cap_reaches_at_its_first_line},
    {NULL, NULL},
};
