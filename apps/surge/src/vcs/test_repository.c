#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE

#include <acutest.h>
#include <git2.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "testing/test_support.h"
#include "vcs/repository.h"
#include "vcs/test_vcs_support.h"

/**
 * @brief Reports whether libgit2's search path for one config level is empty.
 *
 * @param level Config level to inspect, such as `GIT_CONFIG_LEVEL_GLOBAL`.
 * @return `true` when the level's search path is the empty string.
 */
static bool is_search_path_empty(int level) {
  git_buf path = GIT_BUF_INIT;
  const bool is_read = git_libgit2_opts(GIT_OPT_GET_SEARCH_PATH, level, &path) == 0;
  const bool is_empty = is_read && path.size == 0;
  git_buf_dispose(&path);
  return is_empty;
}

// Setup empties every config search path, turns owner validation off and caps the mapped packs.
static void test_setup_sets_process_options(void) {
  struct Log log = test_log();
  TEST_CHECK(repository_setup(&log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_search_path_empty(GIT_CONFIG_LEVEL_SYSTEM));
  TEST_CHECK(is_search_path_empty(GIT_CONFIG_LEVEL_XDG));
  TEST_CHECK(is_search_path_empty(GIT_CONFIG_LEVEL_GLOBAL));
  TEST_CHECK(is_search_path_empty(GIT_CONFIG_LEVEL_PROGRAMDATA));
  int is_validated = -1;
  TEST_CHECK(git_libgit2_opts(GIT_OPT_GET_OWNER_VALIDATION, &is_validated) == 0);
  TEST_CHECK(is_validated == 0);
  size_t file_limit = 0;
  TEST_CHECK(git_libgit2_opts(GIT_OPT_GET_MWINDOW_FILE_LIMIT, &file_limit) == 0);
  TEST_CHECK(file_limit == (size_t)REPOSITORY_MWINDOW_FILE_LIMIT);
}

// The vendored libgit2 is built with thread support, which the workers rely on.
static void test_vendored_libgit2_has_threads(void) {
  TEST_CHECK((git_libgit2_features() & GIT_FEATURE_THREADS) != 0);
}

// A blob's bytes are copied out, with whether git judges them binary.
static void test_read_blob_copies_bytes_and_binary_flag(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "showcase", &arena) == 0);
  struct Log log = test_log();
  struct Str license = {0};
  struct Str data = {0};
  bool is_license_binary = true;
  bool is_data_binary = false;
  TEST_CHECK(repository_read_blob(&repository, S("a6ab47e0ef92cddafd622e118f319ee5e428babb"),
                                  &arena, &license, &is_license_binary, &log) == 0);
  TEST_CHECK(repository_read_blob(&repository, S("8a4bc17dc856987197ba938b28e917b286eeac4f"),
                                  &arena, &data, &is_data_binary, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(license.len == 124 && str_startswith(license, S("MIT License\n")));
  TEST_CHECK(!is_license_binary && is_data_binary && data.len == 12);
  repository_close(&repository);
}

// An id the repository does not hold relays libgit2's message, naming the repo.
static void test_read_blob_rejects_a_missing_object(void) {
  struct Arena arena = test_arena();
  struct Repository repository = {0};
  TEST_ASSERT(open_fixture(&repository, "showcase", &arena) == 0);
  struct Log log = test_log();
  struct Str data = {0};
  bool is_binary = false;
  TEST_CHECK(repository_read_blob(&repository, S("0123456789012345678901234567890123456789"),
                                  &arena, &data, &is_binary, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read blob: object not found - no match for id "
                         "(0123456789012345678901234567890123456789) (in repo 'showcase')\n"));
  repository_close(&repository);
}

/**
 * @brief Makes an empty repository at `main` below `root_dir`, and a linked worktree of it at `wt`,
 *        written by hand as `git worktree add` writes one.
 *
 * @param root_dir      Terminated path of the fixture root, resolved. Must not be `NULL`.
 * @param main_git_dir  Git directory of `main`.
 * @param worktree_path Path of `wt`.
 * @param arena         Arena for the texts of the files. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int write_worktree_fixture(const char* root_dir,
                                  struct Str main_git_dir,
                                  struct Str worktree_path,
                                  struct Arena* arena) {
  const struct Str main_path = path_join(str_from_cstr(root_dir), S("main"), arena);
  git_repository* git = NULL;
  if (!TEST_CHECK(git_repository_init(&git, str_cstr(main_path, arena), 0) == 0)) {
    return -1;
  }
  git_repository_free(git);
  struct Buf gitdir = {.arena = arena};
  buf_printf(&gitdir, "%.*s/.git\n", STR_FMT(worktree_path));
  struct Buf link = {.arena = arena};
  buf_printf(&link, "gitdir: %.*s/worktrees/wt\n", STR_FMT(main_git_dir));
  return TEST_CHECK(
             write_fixture_file(root_dir, "main/.git/worktrees/wt/HEAD", "ref: refs/heads/wt\n") ==
                 0 &&
             write_fixture_file(root_dir, "main/.git/worktrees/wt/commondir", "../..\n") == 0 &&
             write_fixture_file(root_dir, "main/.git/worktrees/wt/gitdir",
                                str_cstr(buf_str(&gitdir), arena)) == 0 &&
             write_fixture_file(root_dir, "wt/.git", str_cstr(buf_str(&link), arena)) == 0)
             ? 0
             : -1;
}

// A linked worktree opens with its main repository's git directory, which holds the objects, the
// refs and the description, and with its own work tree.
static void test_open_gives_a_worktree_the_common_git_dir(void) {
  char root_template[] = "/tmp/surge-repository.XXXXXX";
  if (init_fixture_dir(root_template) != 0) {
    return;
  }
  // libgit2 resolves the directories it reports, and `/tmp` can be a symlink.
  char root_dir[PATH_MAX] = {0};
  const bool is_resolved = TEST_CHECK(realpath(root_template, root_dir) != NULL);
  struct Log log = test_log();
  struct Arena arena = test_arena();
  const struct Str main_path = path_join(str_from_cstr(root_dir), S("main"), &arena);
  const struct Str main_git_dir = path_join(main_path, S(".git"), &arena);
  const struct Str worktree_path = path_join(str_from_cstr(root_dir), S("wt"), &arena);
  struct Repository main = {0};
  struct Repository worktree = {0};
  if (!is_resolved || !TEST_CHECK(repository_setup(&log) == 0) ||
      write_worktree_fixture(root_dir, main_git_dir, worktree_path, &arena) != 0) {
    goto cleanup;
  }
  if (!TEST_CHECK(repository_open(&main, main_path, S("main"), &arena, &log) == 0) ||
      !TEST_CHECK(repository_open(&worktree, worktree_path, S("wt"), &arena, &log) == 0)) {
    goto cleanup;
  }
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(main.git_dir, main_git_dir));
  TEST_CHECK(str_eq(main.work_dir, main_path));
  TEST_CHECK(str_eq(worktree.git_dir, main_git_dir));
  TEST_CHECK(str_eq(worktree.work_dir, worktree_path));
cleanup:
  repository_close(&worktree);
  repository_close(&main);
  remove_fixture_tree(root_template);
}

TEST_LIST = {
    {"setup sets process options", test_setup_sets_process_options},
    {"vendored libgit2 has threads", test_vendored_libgit2_has_threads},
    {"open gives a worktree the common git dir", test_open_gives_a_worktree_the_common_git_dir},
    {"read blob copies bytes and binary flag", test_read_blob_copies_bytes_and_binary_flag},
    {"read blob rejects a missing object", test_read_blob_rejects_a_missing_object},
    {NULL, NULL},
};
