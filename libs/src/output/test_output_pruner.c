#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_pruner.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

/** Directory every prune of the fixture empties of stale entries. */
#define OUTPUT_DIR S("public")

/**
 * @brief Creates a host whose `output_dir` is `public`, holding the outputs `public/index.html` and
 *        `public/notes/index.html`, recorded in `manifest`.
 *
 * The inputs `content`, `templates` and `static` each hold one file, all outside `public`.
 *
 * @param manifest Empty manifest that receives the two outputs. Must not be `NULL`.
 * @param arena    Arena that owns the manifest's entries. Must not be `NULL`.
 * @return The host.
 */
static struct Os* init_pruner_fixture(struct Manifest* manifest, struct Arena* arena) {
  struct Os* os = os_memory();
  os_memory_write(os, "content/notes/hello.md", "source");
  os_memory_write(os, "templates/content.html", "template");
  os_memory_write(os, "static/a.css", "static");
  os_memory_write(os, "public/index.html", "root");
  os_memory_write(os, "public/notes/index.html", "notes");
  TEST_CHECK(manifest_add(manifest, S("public/index.html"), S("content/index.md"), arena, NULL) ==
             0);
  TEST_CHECK(manifest_add(manifest, S("public/notes/index.html"), S("content/notes/hello.md"),
                          arena, NULL) == 0);
  return os;
}

/**
 * @brief Prunes the fixture's `output_dir` through a writer that removes, and checks the result and
 *        the number removed.
 *
 * @param manifest      Outputs of the build. Must not be `NULL`.
 * @param os            Host the outputs are on. Must not be `NULL`.
 * @param expected_rc   Result the prune must return.
 * @param removed_count Number of entries the prune must report removed.
 * @param log           Log that receives the failures. Must not be `NULL`.
 */
static void check_prune(struct Manifest* manifest,
                        struct Os* os,
                        int expected_rc,
                        ptrdiff_t removed_count,
                        struct Log* log) {
  struct OutputWriter writer = {.manifest = manifest, .os = os};
  ptrdiff_t removed_count_out = -1;
  TEST_CHECK(output_pruner_prune(&writer, OUTPUT_DIR, os, test_arena(), &removed_count_out, log) ==
             expected_rc);
  TEST_CHECK(removed_count_out == removed_count);
  TEST_MSG("removed: %td", removed_count_out);
}

/**
 * @brief Reports whether `paths` holds `path`.
 *
 * @param paths Paths to search. Must not be `NULL`.
 * @param path  Terminated path to look for. Must not be `NULL`.
 * @return `true` when one of `paths` is `path`.
 */
static bool is_recorded(const struct PathList* paths, const char* path) {
  for (ptrdiff_t i = 0; i < paths->count; i++) {
    if (str_eq(paths->items[i], str_from_cstr(path))) {
      return true;
    }
  }
  return false;
}

// Every entry the build did not write is removed, a stale directory tree with everything in it, and
// every output and the directories they are in stay. A second prune finds nothing to remove.
static void test_removes_stale_entries_and_keeps_outputs(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "public/old.html", "old");
  os_memory_write(os, "public/notes/old.html", "old");
  os_memory_write(os, "public/old-section/deeper/notes/a.html", "old");
  os_memory_write(os, "public/.hidden", "hand");
  os_memory_mkdir(os, "public/empty");

  struct Log log = test_log();
  // The two old pages, the hidden file, the empty directory, and the old section tree's page and
  // three directories.
  check_prune(&manifest, os, 0, 8, &log);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(os, "public/index.html", "root"));
  TEST_CHECK(os_memory_is_file(os, "public/notes/index.html", "notes"));
  TEST_CHECK(os_memory_files(os, "public").count == 2);
  TEST_CHECK(!os_memory_has_path(os, "public/old-section"));
  TEST_CHECK(!os_memory_has_path(os, "public/empty"));
  TEST_CHECK(os_memory_has_path(os, "public"));

  check_prune(&manifest, os, 0, 0, &log);
  TEST_CHECK(test_log_is_empty(&log));
}

// A build with no outputs empties `output_dir` but keeps the directory itself.
static void test_empties_output_dir_without_outputs(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  struct Manifest empty_manifest = {0};

  struct Log log = test_log();
  check_prune(&empty_manifest, os, 0, 3, &log);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_files(os, "public").count == 0);
  TEST_CHECK(os_memory_has_path(os, "public"));
}

// An `output_dir` that does not exist, as a dry run of a first build leaves it, has nothing stale
// in it, so the prune removes nothing and reports nothing.
static void test_accepts_missing_output_dir(void) {
  struct Os* os = os_memory();
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  TEST_CHECK(manifest_add(&manifest, S("public/index.html"), S("content/index.md"), &arena, NULL) ==
             0);

  struct Log log = test_log();
  check_prune(&manifest, os, 0, 0, &log);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(os, "public"));
}

// A dry run's writer records every entry the prune would remove, a stale directory with what is in
// it, counts each one as removed, and leaves all of them in place.
static void test_dry_run_records_stale_entries_and_keeps_them(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "public/old.html", "old");
  os_memory_write(os, "public/old-section/page.html", "old");
  struct OutputWriter writer = {
      .manifest = &manifest, .os = os, .is_dry_run = true, .perm = &arena};

  struct Log log = test_log();
  ptrdiff_t removed_count = -1;
  TEST_CHECK(output_pruner_prune(&writer, OUTPUT_DIR, os, test_arena(), &removed_count, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(removed_count == 3);
  TEST_CHECK(writer.removed_paths.count == 3);
  TEST_CHECK(is_recorded(&writer.removed_paths, "public/old.html"));
  TEST_CHECK(is_recorded(&writer.removed_paths, "public/old-section/page.html"));
  TEST_CHECK(is_recorded(&writer.removed_paths, "public/old-section"));
  TEST_CHECK(os_memory_is_file(os, "public/old.html", "old"));
  TEST_CHECK(os_memory_is_file(os, "public/old-section/page.html", "old"));
}

// A symlink is removed as a link and never followed, so what it points to outside `output_dir`
// stays. A symlink an output is written through is kept, and so is the output behind it. Removing a
// hard link to a source leaves the source in place.
static void test_removes_links_without_touching_their_targets(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "elsewhere/keep.txt", "keep");
  os_memory_write(os, "linked/page.html", "linked");
  os_memory_symlink(os, "../elsewhere", "public/dir-link");
  os_memory_symlink(os, "../elsewhere/keep.txt", "public/file-link");
  os_memory_symlink(os, "../linked", "public/out");
  os_memory_link(os, "content/notes/hello.md", "public/notes/hello.md");
  TEST_CHECK(
      manifest_add(&manifest, S("public/out/page.html"), S("content/page.md"), &arena, NULL) == 0);

  struct Log log = test_log();
  check_prune(&manifest, os, 0, 3, &log);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(!os_memory_has_path(os, "public/dir-link"));
  TEST_CHECK(!os_memory_has_path(os, "public/file-link"));
  TEST_CHECK(!os_memory_has_path(os, "public/notes/hello.md"));
  TEST_CHECK(os_memory_is_file(os, "elsewhere/keep.txt", "keep"));
  TEST_CHECK(os_memory_is_file(os, "content/notes/hello.md", "source"));
  TEST_CHECK(os_memory_is_file(os, "public/out/page.html", "linked"));
}

// A removal that fails is reported once, the directory it leaves non-empty is neither removed nor
// reported, and every other stale entry is still removed.
static void test_reports_failed_removal_once(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "public/stale/a.html", "old");
  os_memory_write(os, "public/stale/b.html", "old");
  os_memory_write(os, "public/old.html", "old");
  os_memory_fail(os, OS_MEMORY_OP_REMOVE, "public/stale/a.html", EACCES);

  struct Log log = test_log();
  check_prune(&manifest, os, -1, 2, &log);
  TEST_CHECK(test_log_is(&log, "failed to remove stale output: %s ('public/stale/a.html')\n",
                         strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/stale/a.html"));
  TEST_CHECK(!os_memory_has_path(os, "public/stale/b.html"));
  TEST_CHECK(!os_memory_has_path(os, "public/old.html"));
}

// A directory that cannot be listed is reported once and left in place with everything in it, and
// the rest of the tree is still pruned.
static void test_reports_unlistable_dir_once(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "public/sealed/a.html", "old");
  os_memory_write(os, "public/old.html", "old");
  os_memory_fail(os, OS_MEMORY_OP_LIST, "public/sealed", EACCES);

  struct Log log = test_log();
  check_prune(&manifest, os, -1, 1, &log);
  TEST_CHECK(test_log_is(
      &log, "failed to list stale outputs: cannot open directory: %s ('public/sealed')\n",
      strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/sealed/a.html"));
  TEST_CHECK(!os_memory_has_path(os, "public/old.html"));
}

// An entry that cannot be inspected is reported once and left in place, with the directory that
// holds it, and the rest of the tree is still pruned.
static void test_reports_uninspectable_entry_once(void) {
  struct Manifest manifest = {0};
  struct Arena arena = test_arena();
  struct Os* os = init_pruner_fixture(&manifest, &arena);
  os_memory_write(os, "public/odd/b.html", "old");
  os_memory_write(os, "public/old.html", "old");
  os_memory_fail(os, OS_MEMORY_OP_STAT, "public/odd/b.html", EACCES);

  struct Log log = test_log();
  check_prune(&manifest, os, -1, 1, &log);
  TEST_CHECK(test_log_is(
      &log, "failed to list stale outputs: cannot inspect entry: %s ('public/odd/b.html')\n",
      strerror(EACCES)));
  TEST_CHECK(os_memory_has_path(os, "public/odd/b.html"));
  TEST_CHECK(!os_memory_has_path(os, "public/old.html"));
}

TEST_LIST = {
    {"removes stale entries and keeps outputs", test_removes_stale_entries_and_keeps_outputs},
    {"empties output dir without outputs", test_empties_output_dir_without_outputs},
    {"accepts missing output dir", test_accepts_missing_output_dir},
    {"dry run records stale entries and keeps them",
     test_dry_run_records_stale_entries_and_keeps_them},
    {"removes links without touching their targets",
     test_removes_links_without_touching_their_targets},
    {"reports failed removal once", test_reports_failed_removal_once},
    {"reports unlistable dir once", test_reports_unlistable_dir_once},
    {"reports uninspectable entry once", test_reports_uninspectable_entry_once},
    {NULL, NULL},
};
