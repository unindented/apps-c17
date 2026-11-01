#include <acutest.h>
#include <git2.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "testing/test_support.h"
#include "vcs/filetree.h"
#include "vcs/refset.h"
#include "vcs/repository.h"
#include "vcs/test_vcs_support.h"

/**
 * @brief Loads the tree of `main` of the fixture `name`.
 *
 * @param name     Terminated name of the fixture. Must not be `NULL`.
 * @param arena    Arena that owns the tree. Must not be `NULL`.
 * @param tree_out Receives the tree. Must not be `NULL`.
 * @param log      Log that receives the warnings. Must not be `NULL`.
 * @return `0` when the tree loaded, or `-1` otherwise.
 */
static int load_main(const char* name,
                     struct Arena* arena,
                     struct BranchTree* tree_out,
                     struct Log* log) {
  struct Repository repository = {0};
  struct Refset refset = {0};
  bool is_loaded = open_fixture(&repository, name, arena) == 0 &&
                   refset_load(&refset, &repository, arena, log) == 0;
  const struct Ref* branch = is_loaded ? refset_find_branch(&refset, S("main")) : NULL;
  is_loaded = branch != NULL && filetree_load(tree_out, &repository, branch, arena, log) == 0;
  repository_close(&repository);
  return is_loaded ? 0 : -1;
}

/**
 * @brief Finds the entry named `name` in `dir`.
 *
 * @param dir  Directory to search. Must not be `NULL`.
 * @param name Name of the entry.
 * @return The entry, or `NULL` when the directory has none of that name.
 */
static const struct TreeEntry* find_entry(const struct TreeDir* dir, struct Str name) {
  for (ptrdiff_t i = 0; i < dir->entry_count; i++) {
    if (str_eq(dir->entries[i].name, name)) {
      return &dir->entries[i];
    }
  }
  return NULL;
}

/**
 * @brief Makes a bare repository at `r.git` below `root_dir` with one commit, whose tree holds a
 *        symlink `exact` to `exact_target` and a symlink `long` to `long_target`. libgit2 must be
 *        set up.
 *
 * @param root_dir     Terminated path of the fixture root. Must not be `NULL`.
 * @param exact_target Target of `exact`.
 * @param long_target  Target of `long`.
 * @param arena        Arena that owns the commit's id. Must not be `NULL`.
 * @param branch_out   Receives a branch `main` that names the commit. Must not be `NULL`.
 * @return `0` on success, or `-1` after recording a test-plumbing failure.
 */
static int write_symlink_repo(const char* root_dir,
                              struct Str exact_target,
                              struct Str long_target,
                              struct Arena* arena,
                              struct Ref* branch_out) {
  const struct Str path = path_join(str_from_cstr(root_dir), S("r.git"), arena);
  git_repository* git = NULL;
  git_treebuilder* builder = NULL;
  git_tree* tree = NULL;
  git_signature* signature = NULL;
  git_oid exact_oid;
  git_oid long_oid;
  git_oid tree_oid;
  git_oid commit_oid;
  const bool is_written =
      TEST_CHECK(git_repository_init(&git, str_cstr(path, arena), 1) == 0) &&
      TEST_CHECK(git_blob_create_from_buffer(&exact_oid, git, exact_target.data,
                                             (size_t)exact_target.len) == 0) &&
      TEST_CHECK(git_blob_create_from_buffer(&long_oid, git, long_target.data,
                                             (size_t)long_target.len) == 0) &&
      TEST_CHECK(git_treebuilder_new(&builder, git, NULL) == 0) &&
      TEST_CHECK(git_treebuilder_insert(NULL, builder, "exact", &exact_oid, GIT_FILEMODE_LINK) ==
                 0) &&
      TEST_CHECK(git_treebuilder_insert(NULL, builder, "long", &long_oid, GIT_FILEMODE_LINK) ==
                 0) &&
      TEST_CHECK(git_treebuilder_write(&tree_oid, builder) == 0) &&
      TEST_CHECK(git_tree_lookup(&tree, git, &tree_oid) == 0) &&
      TEST_CHECK(git_signature_new(&signature, "A", "a@example.org", 0, 0) == 0) &&
      TEST_CHECK(git_commit_create(&commit_oid, git, NULL, signature, signature, NULL, "m", tree, 0,
                                   NULL) == 0);
  git_signature_free(signature);
  git_tree_free(tree);
  git_treebuilder_free(builder);
  git_repository_free(git);
  if (!is_written) {
    return -1;
  }
  *branch_out = (struct Ref){
      .name = S("refs/heads/main"),
      .short_name = S("main"),
      .kind = REF_KIND_BRANCH,
      .commit_oid = repository_oid_text(&commit_oid, arena),
  };
  return 0;
}

// A directory lists its directories first and then the rest, each group in byte order, and the
// directories are walked breadth first from the root.
static void test_load_lists_directories_first_in_byte_order(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct BranchTree tree = {0};
  TEST_ASSERT(load_main("showcase", &arena, &tree, &log) == 0);
  TEST_CHECK(log.warning_buf.len == 0);
  const char* names[] = {
      "docs",    "lib",       "media",    "names",       "vendor",         "CONTRIBUTING.md",
      "LICENSE", "README.md", "data.bin", "feature.txt", "link-to-readme", "notes.org"};
  const ptrdiff_t count = (ptrdiff_t)(sizeof(names) / sizeof(names[0]));
  TEST_ASSERT(tree.dir_count == 6 && tree.dirs[0].entry_count == count);
  for (ptrdiff_t i = 0; i < count; i++) {
    TEST_CHECK(test_text_is(tree.dirs[0].entries[i].name, "%s", names[i]));
  }
  const char* dir_paths[] = {"", "docs", "lib", "media", "names", "vendor"};
  for (ptrdiff_t i = 0; i < tree.dir_count; i++) {
    TEST_CHECK(test_text_is(tree.dirs[i].path, "%s", dir_paths[i]));
  }
}

// Each entry has its kind and size, a symlink its target, and a gitlink no size.
static void test_load_reads_kinds_sizes_and_link_targets(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct BranchTree tree = {0};
  TEST_ASSERT(load_main("showcase", &arena, &tree, &log) == 0);
  const struct TreeEntry* data = find_entry(&tree.dirs[0], S("data.bin"));
  const struct TreeEntry* link = find_entry(&tree.dirs[0], S("link-to-readme"));
  const struct TreeEntry* sub = find_entry(&tree.dirs[5], S("sub"));
  TEST_ASSERT(data != NULL && link != NULL && sub != NULL);
  TEST_CHECK(data->kind == TREE_ENTRY_FILE && data->len == 12 && data->is_linked);
  TEST_CHECK(link->kind == TREE_ENTRY_SYMLINK && test_text_is(link->link_target, "README.md"));
  TEST_CHECK(sub->kind == TREE_ENTRY_GITLINK && sub->len == 0);
  TEST_CHECK(test_text_is(sub->oid, "1111111111111111111111111111111111111111"));
}

// An executable file keeps its kind apart from a regular one.
static void test_load_reads_executable_files(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct BranchTree tree = {0};
  TEST_ASSERT(load_main("monorepo", &arena, &tree, &log) == 0);
  const struct TreeDir* alpha = NULL;
  for (ptrdiff_t i = 0; i < tree.dir_count; i++) {
    if (str_eq(tree.dirs[i].path, S("apps/alpha"))) {
      alpha = &tree.dirs[i];
    }
  }
  // cppcheck's cross-translation-unit check cannot see that a failed `TEST_ASSERT` returns.
  TEST_ASSERT(alpha != NULL);
  if (alpha == NULL) {
    return;
  }
  const struct TreeEntry* script = find_entry(alpha, S("build.sh"));
  const struct TreeEntry* source = find_entry(alpha, S("alpha.c"));
  TEST_ASSERT(script != NULL && source != NULL);
  TEST_CHECK(script->kind == TREE_ENTRY_EXECUTABLE && source->kind == TREE_ENTRY_FILE);
  TEST_CHECK(test_text_is(script->path, "apps/alpha/build.sh"));
}

// The paths a link resolves to are every linked file and every directory but the root, sorted.
static void test_load_sorts_the_paths_links_resolve_to(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct BranchTree tree = {0};
  TEST_ASSERT(load_main("showcase", &arena, &tree, &log) == 0);
  TEST_ASSERT(tree.dir_path_count == 5 && tree.file_count == 14);
  TEST_CHECK(test_text_is(tree.dir_paths[0], "docs") && test_text_is(tree.dir_paths[4], "vendor"));
  TEST_CHECK(test_text_is(tree.files[0]->path, "CONTRIBUTING.md"));
  for (ptrdiff_t i = 1; i < tree.file_count; i++) {
    TEST_CHECK(str_compare(tree.files[i - 1]->path, tree.files[i]->path) < 0);
  }
}

/**
 * @brief Returns `count` copies of `c`.
 *
 * @param c     Byte to repeat.
 * @param count Number of copies.
 * @param arena Arena that owns the text. Must not be `NULL`.
 * @return The text.
 */
static struct Str repeat(char c, ptrdiff_t count, struct Arena* arena) {
  char* text = arena_new(arena, count, char);
  memset(text, c, (size_t)count);
  return (struct Str){text, count};
}

// A name a page cannot use, names that collide after case folding, and a path over a limit each
// keep their row without pages, with one warning each, and the other entries keep theirs.
static void test_load_unlinks_unusable_names_with_warnings(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct BranchTree tree = {0};
  TEST_ASSERT(load_main("hostile", &arena, &tree, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  // The fixture's long name is 252 `l`s and `.txt`, and its deep path five directories of 250-byte
  // names, whose fourth's tree page is the first over the limit.
  const struct Str long_name = str_concat(repeat('l', 252, &arena), S(".txt"), &arena);
  struct Buf deep_path = {.arena = &arena};
  const char letters[] = {'a', 'a', 'b', 'c'};
  for (ptrdiff_t i = 0; i < 4; i++) {
    buf_append(&deep_path, i > 0 ? S("/") : S(""));
    buf_append(&deep_path, repeat(letters[i], 250, &arena));
  }
  const struct Str deep = buf_str(&deep_path);
  const char* unusable[] = {"..",           ".GIT",         "back\\slash",
                            "bad\\xFFutf8", "ctl\\x01name", "index.html"};
  struct Buf expected = {.arena = &arena};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(unusable) / sizeof(unusable[0])); i++) {
    buf_printf(&expected,
               "warning: tree entry name cannot be a path, so it has no pages: '%s' (in repo "
               "'hostile', branch 'main')\n",
               unusable[i]);
  }
  buf_printf(&expected,
             "warning: output path for tree path '%.*s' exceeds max filename length (%d bytes) at "
             "%d bytes, so it has no pages: '%.*s' (in repo 'hostile', branch 'main')\n",
             STR_FMT(long_name), FILENAME_LEN_MAX, FILENAME_LEN_MAX + 1, STR_FMT(long_name));
  buf_append(&expected, S("warning: tree paths collide after case folding, so none of them has "
                          "pages: 'include/xt_CONNMARK.h', 'include/xt_connmark.h' (in repo "
                          "'hostile', branch 'main')\n"));
  buf_printf(&expected,
             "warning: output path for tree path '%.*s' exceeds max output path length (%d bytes) "
             "at %td bytes, so it has no pages: 'hostile/tree/main/%.*s/index.html' (in repo "
             "'hostile', branch 'main')\n",
             STR_FMT(deep), OUTPUT_PATH_RELATIVE_LEN_MAX, deep.len + 29, STR_FMT(deep));
  const struct Str warnings = buf_str(&log.warning_buf);
  const struct Str expected_text = buf_str(&expected);
  TEST_CHECK(test_text_is(warnings, "%.*s", STR_FMT(expected_text)));
  const struct TreeEntry* ok = find_entry(&tree.dirs[0], S("ok.txt"));
  const struct TreeEntry* dot_git = find_entry(&tree.dirs[0], S(".GIT"));
  const struct TreeEntry* index_dir = find_entry(&tree.dirs[0], S("index.html"));
  TEST_ASSERT(ok != NULL && dot_git != NULL && index_dir != NULL);
  TEST_CHECK(ok->is_linked && !dot_git->is_linked && !index_dir->is_linked);
  // The root, `include` and the first three deep directories have pages.
  TEST_CHECK(tree.dir_count == 5 && tree.file_count == 2);
}

// A symlink target of up to 4096 bytes is kept whole, and a longer one is cut before the UTF-8
// sequence the cut would split, with a warning that names the limit. The limit is file-local to
// `filetree.c`, so it is spelled out here, and changing it there must update this test.
static void test_load_cuts_long_link_targets_with_a_warning(void) {
  char root_template[] = "/tmp/surge-filetree.XXXXXX";
  if (init_fixture_dir(root_template) != 0) {
    return;
  }
  struct Arena arena = test_arena();
  struct Log log = test_log();
  // The cut at 4096 bytes would fall between the two bytes of `é`.
  const struct Str exact_target = repeat('a', 4096, &arena);
  const struct Str long_target = str_concat(repeat('a', 4095, &arena),
                                            S("\xC3\xA9"
                                              "b"),
                                            &arena);
  struct Ref branch = {0};
  struct Repository repository = {0};
  struct BranchTree tree = {0};
  const struct Str path = path_join(str_from_cstr(root_template), S("r.git"), &arena);
  const bool is_loaded =
      TEST_CHECK(repository_setup(&log) == 0) &&
      write_symlink_repo(root_template, exact_target, long_target, &arena, &branch) == 0 &&
      TEST_CHECK(repository_open(&repository, path, S("r"), &arena, &log) == 0) &&
      TEST_CHECK(filetree_load(&tree, &repository, &branch, &arena, &log) == 0);
  repository_close(&repository);
  remove_fixture_tree(root_template);
  TEST_ASSERT(is_loaded && tree.dir_count == 1);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(test_text_is(buf_str(&log.warning_buf),
                          "warning: symlink target exceeds max symlink target length (4096 bytes) "
                          "at 4098 bytes, so its row shows it cut short: 'long' (in repo 'r', "
                          "branch 'main')\n"));
  const struct TreeEntry* exact = find_entry(&tree.dirs[0], S("exact"));
  const struct TreeEntry* cut = find_entry(&tree.dirs[0], S("long"));
  TEST_ASSERT(exact != NULL && cut != NULL);
  TEST_CHECK(str_eq(exact->link_target, exact_target));
  TEST_CHECK(cut->len == 4098 && str_eq(cut->link_target, str_head(long_target, 4095)));
}

TEST_LIST = {
    {"load lists directories first in byte order", test_load_lists_directories_first_in_byte_order},
    {"load reads kinds sizes and link targets", test_load_reads_kinds_sizes_and_link_targets},
    {"load reads executable files", test_load_reads_executable_files},
    {"load sorts the paths links resolve to", test_load_sorts_the_paths_links_resolve_to},
    {"load unlinks unusable names with warnings", test_load_unlinks_unusable_names_with_warnings},
    {"load cuts long link targets with a warning", test_load_cuts_long_link_targets_with_a_warning},
    {NULL, NULL},
};
