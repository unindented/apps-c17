#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_posix.h"
#include "runtime/walk.h"
#include "testing/test_support.h"

// These tests run the walk on the POSIX host, for the failures that only a real filesystem gives.
// `test_walk.c` runs the rest of the walk on the in-memory host.

/**
 * @brief Joins a relative path onto a fixture directory, keeping a terminator after it.
 *
 * The terminator lets a test hand `data` straight to a system call that sets up the fixture, while
 * the host calls under test read the counted string.
 *
 * @param dir           Terminated fixture directory. Must not be `NULL`.
 * @param relative_path Terminated path below `dir`. Must not be `NULL`.
 * @param arena         Arena that owns the joined path. Must not be `NULL`.
 * @return The joined path, its `data` terminated.
 */
static struct Str fixture_path(const char* dir, const char* relative_path, struct Arena* arena) {
  const struct Str path = path_join(str_from_cstr(dir), str_from_cstr(relative_path), arena);
  return (struct Str){str_cstr(path, arena), path.len};
}

/**
 * @brief Returns the scratch arena every filesystem call in this file runs with.
 *
 * One reservation serves every call. A scratch arena passes by value, so whatever a call allocates
 * in it is dead once the call returns, and the next call may reuse the same bytes. No test holds a
 * pointer into it, and no test passes it as a long-lived arena.
 *
 * @return An arena over the shared scratch block.
 */
static struct Arena scratch_arena(void) {
  static struct Arena scratch;
  if (scratch.beg == NULL) {
    scratch = test_arena();
  }
  return scratch;
}

/**
 * @brief Lists files under `root_dir` matching one case-sensitive suffix.
 *
 * Adapts `walk_files_with_suffixes` on the POSIX host to the single-suffix shape the tests here
 * need.
 *
 * @param paths    Path list that receives the matching paths. Must not be `NULL`.
 * @param root_dir Directory tree to walk.
 * @param suffix   Filename suffix to match, such as `.md`.
 * @param perm     Arena that owns the listed paths. Must not be `NULL`.
 * @param log      Log that receives a failure, led by the operation `op`. Must not be `NULL`.
 * @return `0` on success, or `-1` on a directory or entry failure.
 */
static int list_files(struct PathList* paths,
                      struct Str root_dir,
                      struct Str suffix,
                      struct Arena* perm,
                      struct Log* log) {
  return walk_files_with_suffixes(paths, root_dir, S(""), &suffix, 1, false, os_posix(), perm,
                                  scratch_arena(), S("op"), log);
}

// An entry that exists but cannot be inspected fails the walk and names that entry, rather than
// being dropped while the walk still reports success. A directory that is readable but not
// searchable is the reachable way to produce it: `readdir` lists the entry, and `stat` on the entry
// then fails with `EACCES`.
static void test_walk_rejects_unstatable_entry(void) {
  struct Log log = test_log();
  // Root bypasses the permission bits, so the sealed directory would be walked like any other and
  // the assertions below would fail for a reason that says nothing about the code under test.
  if (geteuid() == 0) {
    return;
  }

  char root_dir[] = "/tmp/apps-fs-list-unstatable.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str sealed_dir = fixture_path(root_dir, "sealed", &arena);
  struct Str sealed_md = fixture_path(root_dir, "sealed/a.md", &arena);
  TEST_CHECK(
      os_write(os_posix(), sealed_md, (struct OsSource){.bytes = S("hidden")}, S("op"), &log) == 0);
  TEST_CHECK(chmod(sealed_dir.data, 0400) == 0);

  struct PathList paths = {0};
  const int rc = list_files(&paths, str_from_cstr(root_dir), S(".md"), &arena, &log);
  // Restore the mode before checking anything: neither the cleanup below nor the harness can remove
  // a directory it is not allowed to search.
  (void)chmod(sealed_dir.data, 0700);

  TEST_CHECK(rc == -1);
  TEST_CHECK(paths.count == 0);
  TEST_CHECK(test_log_is(&log, "op: cannot inspect entry: %s ('%.*s')\n", strerror(EACCES),
                         STR_FMT(sealed_md)));
  remove_fixture_tree(root_dir);
}

// An entry the listing could not inspect is reported when the sorted walk reaches it, on the real
// filesystem as on the in-memory host: of two such siblings, the first in byte order, and an error
// inside an earlier subdirectory before a later such sibling. A symlink through a regular file is
// such an entry, and needs no permission change.
static void test_walk_reports_uninspectable_entries_in_order(void) {
  static const struct {
    const char* links[2][2];
    const char* reported;
  } cases[] = {
      {{{"../file.md/y", "site/b.md"}, {"../file.md/y", "site/a.md"}}, "site/a.md"},
      {{{"../file.md/y", "site/b.md"}, {"../../file.md/y", "site/a/z.md"}}, "site/a/z.md"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i].reported);
    char root_dir[] = "/tmp/apps-fs-list-order.XXXXXX";
    if (init_fixture_dir(root_dir) != 0) {
      return;
    }
    struct Arena arena = test_arena();
    struct Log log = test_log();
    const int fd = open(fixture_path(root_dir, "file.md", &arena).data, O_WRONLY | O_CREAT, 0600);
    TEST_CHECK(fd >= 0 && close(fd) == 0);
    TEST_CHECK(mkdir(fixture_path(root_dir, "site", &arena).data, 0700) == 0);
    TEST_CHECK(mkdir(fixture_path(root_dir, "site/a", &arena).data, 0700) == 0);
    for (ptrdiff_t j = 0; j < 2; j++) {
      const struct Str link = fixture_path(root_dir, cases[i].links[j][1], &arena);
      TEST_CHECK(symlink(cases[i].links[j][0], link.data) == 0);
    }
    struct PathList paths = {0};
    const struct Str site = fixture_path(root_dir, "site", &arena);
    TEST_CHECK(list_files(&paths, site, S(".md"), &arena, &log) == -1);
    const struct Str reported = fixture_path(root_dir, cases[i].reported, &arena);
    TEST_CHECK(test_log_is(&log, "op: cannot inspect entry: %s ('%.*s')\n", strerror(ENOTDIR),
                           STR_FMT(reported)));
    remove_fixture_tree(root_dir);
  }
}

// A tree deeper than the open-file limit is walked, because `os_list` closes each directory before
// the walk descends into its subdirectories rather than holding one stream open per level.
static void test_walk_descends_deeper_than_open_file_limit(void) {
  struct Log log = test_log();
  enum { TREE_DEPTH = 64, OPEN_FILE_LIMIT = 32 };
  char root_dir[] = "/tmp/apps-fs-list-deep.XXXXXX";
  if (init_fixture_dir(root_dir) != 0) {
    return;
  }

  struct Arena arena = test_arena();
  struct Str leaf_dir = fixture_path(root_dir, "d", &arena);
  for (size_t i = 1; i < TREE_DEPTH; i++) {
    leaf_dir = fixture_path(leaf_dir.data, "d", &arena);
  }
  const struct Str page = fixture_path(leaf_dir.data, "page.md", &arena);
  TEST_CHECK(os_write(os_posix(), page, (struct OsSource){.bytes = S("x")}, S("op"), &log) == 0);

  struct rlimit limit;
  struct rlimit lowered;
  struct PathList paths = {0};
  int rc = -1;
  if (!TEST_CHECK(getrlimit(RLIMIT_NOFILE, &limit) == 0)) {
    goto cleanup;
  }
  lowered = (struct rlimit){.rlim_cur = OPEN_FILE_LIMIT, .rlim_max = limit.rlim_max};
  if (!TEST_CHECK(setrlimit(RLIMIT_NOFILE, &lowered) == 0)) {
    goto cleanup;
  }
  rc = list_files(&paths, str_from_cstr(root_dir), S(".md"), &arena, &log);
  // Restore the limit before asserting, so a failure cannot starve later tests of descriptors.
  (void)setrlimit(RLIMIT_NOFILE, &limit);

  TEST_CHECK(rc == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(paths.count == 1 && str_eq(paths.items[0], page));

cleanup:
  remove_fixture_tree(root_dir);
}

TEST_LIST = {
    {"walk rejects unstatable entry", test_walk_rejects_unstatable_entry},
    {"walk reports uninspectable entries in order",
     test_walk_reports_uninspectable_entries_in_order},
    {"walk descends deeper than open file limit", test_walk_descends_deeper_than_open_file_limit},
    {NULL, NULL},
};
