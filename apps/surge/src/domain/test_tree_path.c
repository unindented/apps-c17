#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/tree_path.h"
#include "testing/test_support.h"

/**
 * @brief Resolves `path` against `dir_path` as `tree_path_resolve` does.
 *
 * @param dir_path Directory the path is relative to.
 * @param path     Path to resolve.
 * @param path_out Receives the resolved path. Must not be `NULL`.
 * @return Whether the path stays inside the tree.
 */
static bool resolve(struct Str dir_path, struct Str path, struct Str* path_out) {
  struct Arena arena = test_arena();
  return tree_path_resolve(dir_path, path, &arena, path_out);
}

// A relative path joins its directory, a `/`-rooted one starts at the root, and `.`, `..` and empty
// segments are applied.
static void test_resolve_applies_dot_segments(void) {
  struct Str path = {0};
  TEST_CHECK(resolve(S("docs"), S("guide.md"), &path) && test_text_is(path, "docs/guide.md"));
  TEST_CHECK(resolve(S("docs/a"), S("./../b//c/"), &path) && test_text_is(path, "docs/b/c"));
  TEST_CHECK(resolve(S("docs"), S("/lib/main.c"), &path) && test_text_is(path, "lib/main.c"));
  TEST_CHECK(resolve(S("docs"), S(".."), &path) && path.len == 0);
  TEST_CHECK(resolve(S(""), S("a%20b?c#d"), &path) && test_text_is(path, "%s", "a%20b?c#d"));
}

// The directory of a path is what comes before its last `/`, empty in the root.
static void test_dir_drops_the_last_segment(void) {
  TEST_CHECK(test_text_is(tree_path_dir(S("a/b/c")), "a/b"));
  TEST_CHECK(tree_path_dir(S("c")).len == 0);
}

// The lookups find the linked files, the symlinks and the directories of a tree, the root included.
static void test_find_looks_up_files_symlinks_and_directories(void) {
  const struct TreeEntry file = {.path = S("lib/main.c"), .kind = TREE_ENTRY_FILE};
  const struct TreeEntry link = {.path = S("lib/link"), .kind = TREE_ENTRY_SYMLINK};
  const struct TreeEntry* files[] = {&file};
  const struct TreeEntry* symlinks[] = {&link};
  struct Str dir_paths[] = {S("lib")};
  const struct BranchTree tree = {
      .files = files,
      .file_count = 1,
      .symlinks = symlinks,
      .symlink_count = 1,
      .dir_paths = dir_paths,
      .dir_path_count = 1,
  };
  TEST_CHECK(tree_path_find_file(&tree, S("lib/main.c")) == &file);
  TEST_CHECK(tree_path_find_file(&tree, S("lib/link")) == NULL);
  TEST_CHECK(tree_path_find_symlink(&tree, S("lib/link")) == &link);
  TEST_CHECK(tree_path_find_symlink(&tree, S("lib/main.c")) == NULL);
  TEST_CHECK(tree_path_has_dir(&tree, S("lib")) && tree_path_has_dir(&tree, S("")));
  TEST_CHECK(!tree_path_has_dir(&tree, S("lib/main.c")));
  const struct BranchTree empty = {0};
  TEST_CHECK(tree_path_find_file(&empty, S("a")) == NULL);
  TEST_CHECK(tree_path_find_symlink(&empty, S("a")) == NULL);
  TEST_CHECK(!tree_path_has_dir(&empty, S("a")) && tree_path_has_dir(&empty, S("")));
}

// A path that leaves the root does not resolve.
static void test_resolve_rejects_a_path_out_of_the_root(void) {
  struct Str path = {0};
  TEST_CHECK(!resolve(S("docs"), S("../../x"), &path));
  TEST_CHECK(!resolve(S(""), S("/.."), &path));
}

TEST_LIST = {
    {"resolve applies dot segments", test_resolve_applies_dot_segments},
    {"dir drops the last segment", test_dir_drops_the_last_segment},
    {"find looks up files symlinks and directories",
     test_find_looks_up_files_symlinks_and_directories},
    {"resolve rejects a path out of the root", test_resolve_rejects_a_path_out_of_the_root},
    {NULL, NULL},
};
