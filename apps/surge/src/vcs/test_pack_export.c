#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "os/os.h"
#include "os/os_posix.h"
#include "testing/test_support.h"
#include "vcs/pack_export.h"
#include "vcs/refset.h"
#include "vcs/repository.h"
#include "vcs/test_vcs_support.h"

/**
 * @brief Writes the clone pack of the fixture `name`.
 *
 * @param name       Terminated name of the fixture. Must not be `NULL`.
 * @param arena      Arena that owns the paths. Must not be `NULL`.
 * @param export_out Receives the export. Must not be `NULL`.
 * @return `0` when the pack was written, or `-1` otherwise.
 */
static int export_fixture(const char* name, struct Arena* arena, struct PackExport* export_out) {
  struct Repository repository = {0};
  struct Refset refset = {0};
  struct Log log = test_log();
  const bool is_written =
      open_fixture(&repository, name, arena) == 0 &&
      refset_load(&refset, &repository, arena, &log) == 0 &&
      pack_export_build(export_out, &repository, &refset, os_posix(), arena, &log) == 0;
  TEST_CHECK(test_log_is_empty(&log));
  repository_close(&repository);
  return is_written ? 0 : -1;
}

/**
 * @brief Reports whether `path` names nothing on the real host.
 *
 * @param path Path to inspect.
 * @return `true` when `os_stat` finds nothing there.
 */
static bool is_gone(struct Str path) {
  struct OsStat st;
  return os_stat(os_posix(), path, &st) != 0;
}

// The same refs make the same pack, by name and by size, and its removal leaves nothing behind.
static void test_build_writes_the_same_pack_twice(void) {
  struct Arena arena = test_arena();
  struct PackExport first = {0};
  struct PackExport second = {0};
  // Each pack is a fixture below `TMPDIR`, so a failed step still reaches the removal.
  if (TEST_CHECK(export_fixture("showcase", &arena, &first) == 0) &&
      TEST_CHECK(export_fixture("showcase", &arena, &second) == 0)) {
    TEST_CHECK(first.pack_id.len == 40 && str_eq(first.pack_id, second.pack_id));
    TEST_CHECK(first.pack_len > 0 && first.pack_len == second.pack_len);
    TEST_CHECK(first.idx_len > 0 && first.idx_len == second.idx_len);
    TEST_CHECK(!str_eq(first.dir_path, second.dir_path));
  }
  pack_export_remove(&first);
  pack_export_remove(&second);
  TEST_CHECK(is_gone(first.pack_path) && is_gone(first.idx_path) && is_gone(first.dir_path));
}

// A repository with no refs writes nothing, and its `objects/info/packs` is empty.
static void test_build_writes_nothing_for_an_empty_repository(void) {
  struct Arena arena = test_arena();
  struct PackExport export = {0};
  TEST_CHECK(export_fixture("empty", &arena, &export) == 0);
  TEST_CHECK(export.dir_path.len == 0 && export.pack_id.len == 0);
  TEST_CHECK(pack_export_format_packs(export.pack_id, &arena).len == 0);
  pack_export_remove(&export);
}

// `objects/info/packs` lists the pack and ends with a blank line, as git writes it.
static void test_format_packs_lists_the_pack_and_a_blank_line(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(test_text_is(pack_export_format_packs(S("abc"), &arena), "P pack-abc.pack\n\n"));
}

TEST_LIST = {
    {"build writes the same pack twice", test_build_writes_the_same_pack_twice},
    {"build writes nothing for an empty repository",
     test_build_writes_nothing_for_an_empty_repository},
    {"format packs lists the pack and a blank line",
     test_format_packs_lists_the_pack_and_a_blank_line},
    {NULL, NULL},
};
