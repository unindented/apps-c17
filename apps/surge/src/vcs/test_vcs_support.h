#ifndef SURGE_TEST_VCS_SUPPORT_H
#define SURGE_TEST_VCS_SUPPORT_H

#include <acutest.h>
#include <stdbool.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "testing/test_support.h"
#include "vcs/repository.h"

/**
 * @brief Sets libgit2 up and opens the fixture repository `name` that the CTest fixture
 *        `surge_fixture_repos` made below `SURGE_FIXTURE_REPOS_DIR`.
 *
 * @param repository Repository to open, zeroed. Must not be `NULL`.
 * @param name       Terminated name of the fixture, such as `showcase`. Must not be `NULL`.
 * @param arena      Arena that owns the paths. Must not be `NULL`.
 * @return `0` when the repository is open, or `-1` otherwise.
 */
static inline int open_fixture(struct Repository* repository,
                               const char* name,
                               struct Arena* arena) {
  const char* repos_dir = getenv("SURGE_FIXTURE_REPOS_DIR");
  TEST_ASSERT(repos_dir != NULL);
  if (repos_dir == NULL) {
    return -1;
  }
  struct Log log = test_log();
  const struct Str path = path_join(str_from_cstr(repos_dir), str_from_cstr(name), arena);
  const bool is_open = repository_setup(&log) == 0 &&
                       repository_open(repository, path, str_from_cstr(name), arena, &log) == 0;
  TEST_CHECK(test_log_is_empty(&log));
  return is_open ? 0 : -1;
}

#endif
