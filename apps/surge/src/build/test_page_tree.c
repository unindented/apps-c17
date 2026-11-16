#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/page_tree.h"
#include "build/readme.h"
#include "build/template_value.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "domain/site_config.h"
#include "domain/surge_version.h"
#include "testing/test_support.h"

/**
 * A repo `r` described as `Tools`, whose branch `main` holds `LICENSE`, `README.md` and `lib/`,
 * which holds `README.md`.
 */
struct TreeFixture {
  /** The repo's table. */
  struct SurgeRepo repo_config;

  /**
   * The configuration of the site `Code`, described as `Repositories`, by `Ada`, at
   * `https://git.example.org`.
   */
  struct SiteConfig site_config;

  /** The branch. */
  struct Ref branch;

  /** The branch's newest commit. */
  struct CommitSummary commit;

  /** The root's entries, in tree order. */
  struct TreeEntry root_entries[3];

  /** The entries of `lib/`. */
  struct TreeEntry lib_entries[1];

  /** The root, then `lib/`. */
  struct TreeDir dirs[2];

  /** The tree. */
  struct BranchTree tree;

  /** The repo. */
  struct RepoModel repo;
};

/**
 * @brief Fills the repo `fixture` describes.
 *
 * @param fixture Fixture to fill. Must not be `NULL`.
 */
static void tree_fixture_init(struct TreeFixture* fixture) {
  *fixture = (struct TreeFixture){
      .repo_config = {.name = S("r")},
      .site_config = {.title = S("Code"),
                      .description = S("Repositories"),
                      .author = S("Ada"),
                      .base_url = S("https://git.example.org")},
      .branch = {.name = S("refs/heads/main"),
                 .short_name = S("main"),
                 .kind = REF_KIND_BRANCH,
                 .commit_oid = S("1111111111111111111111111111111111111111")},
      .commit = {.oid = S("1111111111111111111111111111111111111111"), .subject = S("Start")},
      .root_entries =
          {
              {.name = S("lib"), .path = S("lib"), .kind = TREE_ENTRY_DIR, .is_linked = true},
              {.name = S("LICENSE"),
               .path = S("LICENSE"),
               .kind = TREE_ENTRY_FILE,
               .is_linked = true},
              {.name = S("README.md"),
               .path = S("README.md"),
               .kind = TREE_ENTRY_FILE,
               .is_linked = true},
          },
      .lib_entries =
          {
              {.name = S("README.md"),
               .path = S("lib/README.md"),
               .kind = TREE_ENTRY_FILE,
               .is_linked = true},
          },
  };
  fixture->dirs[0] = (struct TreeDir){.entries = fixture->root_entries, .entry_count = 3};
  fixture->dirs[1] =
      (struct TreeDir){.path = S("lib"), .entries = fixture->lib_entries, .entry_count = 1};
  fixture->tree =
      (struct BranchTree){.branch = &fixture->branch, .dirs = fixture->dirs, .dir_count = 2};
  fixture->repo = (struct RepoModel){
      .config = &fixture->repo_config,
      .description = S("Tools"),
      .refset = {.refs = &fixture->branch, .count = 1},
      .ref_commits = &fixture->commit,
      .default_branch = S("main"),
      .default_ref = &fixture->branch,
  };
}

/**
 * @brief Reports whether the page's document `index` has the kind `kind` and the `is_main` that
 *        `is_main` says.
 *
 * @param page    The page's data tree. Must not be `NULL`.
 * @param index   Index of the document.
 * @param kind    Terminated kind name. Must not be `NULL`.
 * @param is_main Whether the document should be the main one.
 * @return `true` when both match.
 */
static bool is_document(const struct TemplateValue* page,
                        ptrdiff_t index,
                        const char* kind,
                        bool is_main) {
  const struct TemplateValue* documents = template_value_find(page, S("documents"));
  if (documents == NULL || index >= documents->item_count) {
    return false;
  }
  const struct TemplateValue* document = documents->items[index];
  return str_eq(template_value_find(document, S("kind"))->text, str_from_cstr(kind)) &&
         template_value_is_truthy(template_value_find(document, S("is_main"))) == is_main;
}

/**
 * @brief Returns the text of the field `name` of the map `name_of_map` of `page`, or of `page`
 *        itself when `name_of_map` is `NULL`.
 *
 * @param page        The page's data tree. Must not be `NULL`.
 * @param name_of_map Terminated name of a map of `page`, or `NULL`.
 * @param name        Terminated name of the field. Must not be `NULL`.
 * @return The text.
 */
static struct Str text_of(const struct TemplateValue* page,
                          const char* name_of_map,
                          const char* name) {
  const struct TemplateValue* map =
      name_of_map != NULL ? template_value_find(page, str_from_cstr(name_of_map)) : page;
  return template_value_find(map, str_from_cstr(name))->text;
}

/**
 * @brief Returns the `link_url` of the row of a symlink `lib/link` to `target`, whose blob is `len`
 *        bytes, on a page whose way back to the output root is `../`. The fixture's tree also holds
 *        the symlink `lib/inner`.
 *
 * @param target Terminated target text. Must not be `NULL`.
 * @param len    Size of the symlink's blob, longer than `target` for a target cut short.
 * @return The link.
 */
static struct Str symlink_link_url(const char* target, ptrdiff_t len) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  // The linked files and directories, and the symlinks, as the scan sorts them.
  const struct TreeEntry* files[] = {&fixture.root_entries[1], &fixture.root_entries[2],
                                     &fixture.lib_entries[0]};
  struct Str dir_paths[] = {S("lib")};
  const struct TreeEntry inner = {
      .name = S("inner"), .path = S("lib/inner"), .kind = TREE_ENTRY_SYMLINK, .is_linked = true};
  const struct TreeEntry* symlinks[] = {&inner};
  fixture.tree.files = files;
  fixture.tree.file_count = 3;
  fixture.tree.dir_paths = dir_paths;
  fixture.tree.dir_path_count = 1;
  fixture.tree.symlinks = symlinks;
  fixture.tree.symlink_count = 1;
  struct TreeEntry link = {
      .name = S("link"),
      .path = S("lib/link"),
      .kind = TREE_ENTRY_SYMLINK,
      .len = len,
      .link_target = str_from_cstr(target),
      .is_linked = true,
  };
  const struct TreeDir dir = {.path = S("lib"), .entries = &link, .entry_count = 1};
  struct Arena arena = test_arena();
  const struct TemplateValue* entries =
      page_tree_entries(&fixture.site_config, &fixture.repo, &fixture.tree, &dir, S("../"), &arena);
  TEST_CHECK(template_value_find(entries->items[0], S("url"))->text.len == 0);
  return template_value_find(entries->items[0], S("link_url"))->text;
}

// The home shows the default branch's root: the README is its main document, rendered, the branch's
// newest commit, and the repo's description, with links from `<r>/index.html`.
static void test_value_makes_the_readme_main_on_the_home(void) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TreeDocument documents[] = {
      {.kind = DOCUMENT_README,
       .entry = &fixture.root_entries[2],
       .rendered_html = S("<p>Read me.</p>\n")},
      {.kind = DOCUMENT_LICENSE,
       .entry = &fixture.root_entries[1],
       .rendered_html = S("<p>MIT</p>\n")},
  };
  const struct TemplateValue* home =
      page_tree_value(&fixture.site_config, &fixture.repo, &fixture.tree, &fixture.dirs[0], true,
                      documents, 2, &arena);
  TEST_CHECK(is_document(home, 0, "readme", true));
  TEST_CHECK(is_document(home, 1, "license", false));
  const struct TemplateValue* readme = template_value_find(home, S("documents"))->items[0];
  TEST_CHECK(
      test_text_is(template_value_find(readme, S("rendered_html"))->text, "<p>Read me.</p>\n"));
  TEST_CHECK(test_text_is(text_of(home, NULL, "path_to_root"), "../"));
  TEST_CHECK(test_text_is(text_of(home, "repo", "description"), "Tools"));
  TEST_CHECK(test_text_is(text_of(home, "commit", "subject"), "Start"));
  TEST_CHECK(template_value_is_truthy(template_value_find(home, S("is_home"))));
  TEST_CHECK(template_value_is_truthy(template_value_find(home, S("is_root"))));
  TEST_CHECK(!template_value_is_truthy(template_value_find(home, S("is_empty"))));
}

// Every page starts with the `site` map, which holds the site keys of the configuration, and with
// `generator` beside it. The golden test's feeds show `site.author` end to end.
static void test_value_holds_the_site_keys(void) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TemplateValue* page = page_tree_value(
      &fixture.site_config, &fixture.repo, &fixture.tree, &fixture.dirs[0], false, NULL, 0, &arena);
  TEST_CHECK(test_text_is(text_of(page, "site", "title"), "Code"));
  TEST_CHECK(test_text_is(text_of(page, "site", "description"), "Repositories"));
  TEST_CHECK(test_text_is(text_of(page, "site", "author"), "Ada"));
  TEST_CHECK(test_text_is(text_of(page, "site", "base_url"), "https://git.example.org"));
  TEST_CHECK(str_eq(text_of(page, NULL, "generator"), surge_generator_string()));
  TEST_CHECK(template_value_find(template_value_find(page, S("site")), S("generator")) == NULL);
}

// Without a README, the first document is the main one. Each document has a flag for each kind,
// true for its own.
static void test_value_makes_the_first_document_main_without_a_readme(void) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TreeDocument documents[] = {
      {.kind = DOCUMENT_LICENSE,
       .entry = &fixture.root_entries[1],
       .rendered_html = S("<p>MIT</p>\n")},
  };
  const struct TemplateValue* page =
      page_tree_value(&fixture.site_config, &fixture.repo, &fixture.tree, &fixture.dirs[0], false,
                      documents, 1, &arena);
  TEST_CHECK(is_document(page, 0, "license", true));
  const struct TemplateValue* license = template_value_find(page, S("documents"))->items[0];
  TEST_CHECK(template_value_is_truthy(template_value_find(license, S("is_license"))));
  TEST_CHECK(!template_value_is_truthy(template_value_find(license, S("is_readme"))));
  TEST_CHECK(!template_value_is_truthy(template_value_find(page, S("is_home"))));
  TEST_CHECK(test_text_is(text_of(page, NULL, "path_to_root"), "../../../"));
}

// A subdirectory's page has its own documents, and no commit, which only the root's page shows.
static void test_value_shows_no_commit_below_the_root(void) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  struct Arena arena = test_arena();
  const struct TreeDocument documents[] = {
      {.kind = DOCUMENT_README,
       .entry = &fixture.lib_entries[0],
       .rendered_html = S("<p>Lib.</p>\n")},
  };
  const struct TemplateValue* page =
      page_tree_value(&fixture.site_config, &fixture.repo, &fixture.tree, &fixture.dirs[1], false,
                      documents, 1, &arena);
  TEST_CHECK(is_document(page, 0, "readme", true));
  TEST_CHECK(test_text_is(text_of(page, NULL, "path"), "lib"));
  TEST_CHECK(!template_value_is_truthy(template_value_find(page, S("is_root"))));
  TEST_CHECK(template_value_find(page, S("commit"))->kind == TEMPLATE_VALUE_FALSE);
}

// The home of a repo with no commit is empty, with no entries, documents or commit.
static void test_value_makes_the_home_of_an_empty_repo(void) {
  struct TreeFixture fixture;
  tree_fixture_init(&fixture);
  fixture.repo.default_ref = NULL;
  struct Arena arena = test_arena();
  const struct TemplateValue* home =
      page_tree_value(&fixture.site_config, &fixture.repo, NULL, NULL, true, NULL, 0, &arena);
  TEST_CHECK(template_value_is_truthy(template_value_find(home, S("is_empty"))));
  TEST_CHECK(template_value_find(home, S("tree_entries"))->item_count == 0);
  TEST_CHECK(template_value_find(home, S("documents"))->item_count == 0);
  TEST_CHECK(template_value_find(home, S("commit"))->kind == TEMPLATE_VALUE_FALSE);
  TEST_CHECK(test_text_is(text_of(home, "ref", "name"), "main"));
}

// A symlink links the blob page of a file it names and the tree page of a directory, resolved
// against its own directory, and has no `url` of its own.
static void test_entries_link_a_symlink_to_its_target(void) {
  TEST_CHECK(
      test_text_is(symlink_link_url("README.md", 9), "../r/blob/main/lib/README.md/index.html"));
  TEST_CHECK(test_text_is(symlink_link_url("../LICENSE", 10), "../r/blob/main/LICENSE/index.html"));
  TEST_CHECK(test_text_is(symlink_link_url("./../lib/", 9), "../r/tree/main/lib/index.html"));
  TEST_CHECK(test_text_is(symlink_link_url("..", 2), "../r/tree/main/index.html"));
}

// A symlink to a symlink links the tree page of the directory that holds the second one, one hop
// only.
static void test_entries_link_a_symlink_to_a_symlink_to_its_directory(void) {
  TEST_CHECK(test_text_is(symlink_link_url("inner", 5), "../r/tree/main/lib/index.html"));
}

// A symlink out of the tree, to an absolute path, to a path the branch does not have, or whose
// target was cut short has no link. Its target is a raw path, with no escape, query or `.md` added.
static void test_entries_link_no_symlink_out_of_the_tree(void) {
  TEST_CHECK(symlink_link_url("../../outside", 13).len == 0);
  TEST_CHECK(symlink_link_url("/etc/passwd", 11).len == 0);
  TEST_CHECK(symlink_link_url("missing.c", 9).len == 0);
  TEST_CHECK(symlink_link_url("README.md", 4096 + 9).len == 0);
  TEST_CHECK(symlink_link_url("READM%45.md", 11).len == 0);
  TEST_CHECK(symlink_link_url("README.md?plain=1", 17).len == 0);
  TEST_CHECK(symlink_link_url("README", 6).len == 0);
}

TEST_LIST = {
    {"value makes the readme main on the home", test_value_makes_the_readme_main_on_the_home},
    {"value holds the site keys", test_value_holds_the_site_keys},
    {"value makes the first document main without a readme",
     test_value_makes_the_first_document_main_without_a_readme},
    {"value shows no commit below the root", test_value_shows_no_commit_below_the_root},
    {"value makes the home of an empty repo", test_value_makes_the_home_of_an_empty_repo},
    {"entries link a symlink to its target", test_entries_link_a_symlink_to_its_target},
    {"entries link a symlink to a symlink to its directory",
     test_entries_link_a_symlink_to_a_symlink_to_its_directory},
    {"entries link no symlink out of the tree", test_entries_link_no_symlink_out_of_the_tree},
    {NULL, NULL},
};
