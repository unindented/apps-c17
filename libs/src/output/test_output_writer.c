#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/output_writer.h"
#include "testing/test_support.h"

/**
 * @brief Initializes a writer on a new host whose manifest names `public/a.html` and
 *        `public/b.html`.
 *
 * @param writer     Writer to initialize. Must not be `NULL`.
 * @param manifest   Empty manifest that receives the two paths. Must not be `NULL`.
 * @param is_dry_run Whether the writer withholds its changes.
 * @param arena      Arena that owns the manifest's entries and the removed paths. Must not be
 *                   `NULL`.
 */
static void init_writer(struct OutputWriter* writer,
                        struct Manifest* manifest,
                        bool is_dry_run,
                        struct Arena* arena) {
  TEST_CHECK(manifest_add(manifest, S("public/a.html"), S("a"), arena, NULL) == 0);
  TEST_CHECK(manifest_add(manifest, S("public/b.html"), S("b"), arena, NULL) == 0);
  *writer = (struct OutputWriter){
      .manifest = manifest, .os = os_memory(), .is_dry_run = is_dry_run, .perm = arena};
}

/**
 * @brief Reports whether `name` is one of the names `userdata` lists, as
 *        `OutputWriterIsUnchangedFn`.
 *
 * @param name        Name of the source below the root.
 * @param source_stat Stat of the source, unused.
 * @param userdata    `NULL`-terminated array of the names of the unchanged sources.
 * @return `true` when `name` is listed.
 */
static bool is_listed(struct Str name, const struct OsStat* source_stat, void* userdata) {
  (void)source_stat;
  for (const char* const* listed = userdata; *listed != NULL; listed++) {
    if (str_eq(name, str_from_cstr(*listed))) {
      return true;
    }
  }
  return false;
}

// A write writes its bytes and marks its path written, and a keep marks its path kept and leaves
// the file as it is. A path neither touched stays pending.
static void test_marks_writes_and_keeps(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/b.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "new"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_PENDING);

  TEST_CHECK(output_writer_keep(&writer, S("public/b.html"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/b.html", "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_KEPT);
}

// A copy whose source the build cache vouches for, and whose output is a file of the source's size,
// is kept. Every other copy is written from its source: one the cache does not vouch for, and one
// whose output has another size.
static void test_copy_files_keeps_current_copies(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "static/a.html", "aaa");
  os_memory_write(writer.os, "static/b.html", "bbb");
  os_memory_write(writer.os, "public/a.html", "old");
  os_memory_write(writer.os, "public/b.html", "longer");
  struct PathList sources = {0};
  path_list_push(&sources, S("static/a.html"), &arena);
  path_list_push(&sources, S("static/b.html"), &arena);
  const struct OsStat stats[] = {{.len = 3}, {.len = 3}};
  const char* unchanged[] = {"a.html", "b.html", NULL};

  struct Log log = test_log();
  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, is_listed, unchanged, S("static"),
                                      S("public"), test_arena(), S("failed to copy file"),
                                      &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "old"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(os_memory_is_file(writer.os, "public/b.html", "bbb"));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_WRITTEN);

  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, NULL, NULL, S("static"),
                                      S("public"), test_arena(), S("failed to copy file"),
                                      &log) == 0);
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "aaa"));
}

// A copy keeps its path below the root of its sources, nested directories included, below the
// output directory.
static void test_copy_files_keeps_the_path_below_the_root(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  TEST_CHECK(manifest_add(&manifest, S("public/nested/a.svg"), S("a"), &arena, NULL) == 0);
  struct OutputWriter writer = {.manifest = &manifest, .os = os_memory(), .perm = &arena};
  os_memory_write(writer.os, "other/nested/a.svg", "<svg/>");
  struct PathList sources = {0};
  path_list_push(&sources, S("other/nested/a.svg"), &arena);
  const struct OsStat stats[] = {{.len = 6}};
  struct Log log = test_log();
  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, NULL, NULL, S("other"), S("public"),
                                      test_arena(), S("failed to copy file"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_file(writer.os, "public/nested/a.svg", "<svg/>"));
}

// A dry run of the copies marks a stale copy written and a current one kept, and changes neither.
static void test_copy_files_dry_run_changes_nothing(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "static/a.html", "aaa");
  os_memory_write(writer.os, "static/b.html", "bbb");
  os_memory_write(writer.os, "public/a.html", "old");
  os_memory_write(writer.os, "public/b.html", "old");
  struct PathList sources = {0};
  path_list_push(&sources, S("static/a.html"), &arena);
  path_list_push(&sources, S("static/b.html"), &arena);
  const struct OsStat stats[] = {{.len = 3}, {.len = 3}};
  const char* unchanged[] = {"a.html", NULL};

  struct Log log = test_log();
  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, is_listed, unchanged, S("static"),
                                      S("public"), test_arena(), S("failed to copy file"),
                                      &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_KEPT);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/b.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(os_memory_is_file(writer.os, "public/a.html", "old"));
  TEST_CHECK(os_memory_is_file(writer.os, "public/b.html", "old"));
}

// A failed copy names the operation and the cause, then the path that failed: the destination when
// it is an existing directory, and the source when it is gone. A directory is no current copy, even
// of a source of its size that the build cache vouches for.
static void test_copy_files_reports_a_failed_copy(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  // The source is empty, as the in-memory host's directory is, so only the type tells them apart.
  os_memory_write(writer.os, "static/a.html", "");
  os_memory_mkdir(writer.os, "public/a.html");
  struct PathList sources = {0};
  path_list_push(&sources, S("static/a.html"), &arena);
  const struct OsStat stats[] = {{.len = 0}};
  const char* unchanged[] = {"a.html", NULL};
  struct Log log = test_log();
  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, is_listed, unchanged, S("static"),
                                      S("public"), test_arena(), S("failed to copy file"),
                                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to copy file: %s ('public/a.html')\n", strerror(EISDIR)));

  sources = (struct PathList){0};
  path_list_push(&sources, S("static/b.html"), &arena);
  log = test_log();
  TEST_CHECK(output_writer_copy_files(&writer, &sources, stats, NULL, NULL, S("static"),
                                      S("public"), test_arena(), S("failed to copy file"),
                                      &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to copy file: %s ('static/b.html')\n", strerror(ENOENT)));
}

// A real build's writer makes each change: it creates directories and removes files and
// directories, recording nothing.
static void test_makes_changes(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);
  os_memory_write(writer.os, "public/old/page.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_mkdirs(&writer, S("public/new"), S("failed to create"), &log) == 0);
  TEST_CHECK(
      output_writer_remove(&writer, S("public/old/page.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_has_path(writer.os, "public/new"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/old"));
  TEST_CHECK(writer.removed_paths.count == 0);
}

// A dry run's writer marks a write without making it, creates no directory, and records each
// removal in order while leaving the path in place.
static void test_dry_run_withholds_changes(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  os_memory_write(writer.os, "public/old/page.html", "old");

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(output_writer_mkdirs(&writer, S("public/new"), S("failed to create"), &log) == 0);
  TEST_CHECK(
      output_writer_remove(&writer, S("public/old/page.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.html")) == MANIFEST_MARK_WRITTEN);
  TEST_CHECK(!os_memory_has_path(writer.os, "public/a.html"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/new"));
  TEST_CHECK(os_memory_is_file(writer.os, "public/old/page.html", "old"));
  TEST_CHECK(writer.removed_paths.count == 2);
  TEST_CHECK(str_eq(writer.removed_paths.items[0], S("public/old/page.html")));
  TEST_CHECK(str_eq(writer.removed_paths.items[1], S("public/old")));
}

// The report lists every output path in manifest order as `write` or `keep`, then every recorded
// removal as `remove`, sorted bytewise, each a mark, a tab and the path.
static void test_reports_marks_and_sorted_removals(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_writer_keep(&writer, S("public/b.html"), &log) == 0);
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){0},
                                 S("failed to write output"), &log) == 0);
  TEST_CHECK(output_writer_remove(&writer, S("public/z.html"), S("failed to remove"), &log) == 0);
  TEST_CHECK(output_writer_remove(&writer, S("public/old/x.html"), S("failed to remove"), &log) ==
             0);
  TEST_CHECK(output_writer_remove_dir(&writer, S("public/old"), S("failed to remove"), &log) == 0);

  TEST_CHECK(output_writer_report(&writer, test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(os_memory_is_printed(writer.os, 1,
                                  "write\tpublic/a.html\n"
                                  "keep\tpublic/b.html\n"
                                  "remove\tpublic/old\n"
                                  "remove\tpublic/old/x.html\n"
                                  "remove\tpublic/z.html\n"));
}

// A write or a keep of a path the manifest does not name fails with the operation and the path, and
// the write changes nothing.
static void test_rejects_path_outside_manifest(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, false, &arena);

  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/c.html"), (struct OsSource){.bytes = S("new")},
                                 S("failed to write output"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to write output: not a build output ('public/c.html')\n"));
  TEST_CHECK(!os_memory_has_path(writer.os, "public/c.html"));

  log = test_log();
  TEST_CHECK(output_writer_keep(&writer, S("public/c.html"), &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to keep output: not a build output ('public/c.html')\n"));
}

// An output path that is still pending after the writes was written without the writer, so the
// report names it and prints no listing, which would leave that write out.
static void test_report_rejects_pending_output(void) {
  struct Arena arena = test_arena();
  struct Manifest manifest = {0};
  struct OutputWriter writer;
  init_writer(&writer, &manifest, true, &arena);
  struct Log log = test_log();
  TEST_CHECK(output_writer_write(&writer, S("public/a.html"), (struct OsSource){0},
                                 S("failed to write output"), &log) == 0);

  TEST_CHECK(output_writer_report(&writer, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to report dry run: output was neither written nor kept ('public/b.html')\n"));
  TEST_CHECK(os_memory_is_printed(writer.os, 1, ""));
}

TEST_LIST = {
    {"marks writes and keeps", test_marks_writes_and_keeps},
    {"copy files keeps current copies", test_copy_files_keeps_current_copies},
    {"copy files keeps the path below the root", test_copy_files_keeps_the_path_below_the_root},
    {"copy files dry run changes nothing", test_copy_files_dry_run_changes_nothing},
    {"copy files reports a failed copy", test_copy_files_reports_a_failed_copy},
    {"makes changes", test_makes_changes},
    {"dry run withholds changes", test_dry_run_withholds_changes},
    {"reports marks and sorted removals", test_reports_marks_and_sorted_removals},
    {"rejects path outside manifest", test_rejects_path_outside_manifest},
    {"report rejects pending output", test_report_rejects_pending_output},
    {NULL, NULL},
};
