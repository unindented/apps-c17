#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "build/readme.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "testing/test_support.h"

/** Raw limit of the fixture branch, which `media/big.png` is over. */
enum { README_TEST_RAW_LEN_MAX = 100 };

/**
 * A branch whose tree holds `README.md`, `docs/guide.md`, `docs/notes.org`, `lib.md`, `lib/main.c`,
 * `media/big.png` and `media/dot.png`.
 */
struct ReadmeFixture {
  /** The branch. */
  struct Ref branch;

  /** The files, sorted by path. */
  struct TreeEntry files[7];

  /** Pointers to `files`, as the tree holds them. */
  const struct TreeEntry* file_pointers[7];

  /** Sorted directory paths. */
  struct Str dir_paths[3];

  /** The tree. */
  struct BranchTree tree;
};

/**
 * @brief Fills the branch tree `fixture` describes.
 *
 * @param fixture Fixture to fill. Must not be `NULL`.
 */
static void readme_fixture_init(struct ReadmeFixture* fixture) {
  *fixture = (struct ReadmeFixture){
      .branch = {.name = S("refs/heads/feature/x"), .short_name = S("feature/x")},
      .files =
          {
              {.path = S("README.md"), .len = 10},
              {.path = S("docs/guide.md"), .len = 10},
              {.path = S("docs/notes.org"), .len = 10},
              {.path = S("lib.md"), .len = 10},
              {.path = S("lib/main.c"), .len = 10},
              {.path = S("media/big.png"), .len = README_TEST_RAW_LEN_MAX + 1},
              {.path = S("media/dot.png"), .len = README_TEST_RAW_LEN_MAX},
          },
      .dir_paths = {S("docs"), S("lib"), S("media")},
  };
  for (ptrdiff_t i = 0; i < 7; i++) {
    fixture->file_pointers[i] = &fixture->files[i];
  }
  fixture->tree = (struct BranchTree){
      .branch = &fixture->branch,
      .files = fixture->file_pointers,
      .file_count = 7,
      .dir_paths = fixture->dir_paths,
      .dir_path_count = 3,
  };
}

/**
 * @brief Renders Markdown or Org text as the README of `docs/` in the fixture, on a page two levels
 *        below the output root.
 *
 * @param text                   Document text. Must not be `NULL`.
 * @param is_org                 Whether the text is Org.
 * @param missing_link_count_out Receives how many links named no path. Must not be `NULL`.
 * @return The HTML.
 */
static struct Str render(const char* text, bool is_org, ptrdiff_t* missing_link_count_out) {
  struct ReadmeFixture fixture;
  readme_fixture_init(&fixture);
  const struct DocumentPlace place = {
      .tree = &fixture.tree,
      .repo_name = S("r"),
      .path_to_root = S("../../"),
      .dir_path = S("docs"),
      .raw_len_max = README_TEST_RAW_LEN_MAX,
  };
  struct Arena perm = test_arena();
  return readme_render_markup(str_from_cstr(text), is_org, &place, &perm, test_arena(),
                              missing_link_count_out);
}

/**
 * @brief Returns a directory of files with the given names, each a regular, linked file of size 1.
 *
 * @param names Names of the files. Must not be `NULL`.
 * @param count Number of entries in `names`.
 * @param arena Arena that owns the directory. Must not be `NULL`.
 * @return The directory.
 */
static struct TreeDir make_dir(const char* const* names, ptrdiff_t count, struct Arena* arena) {
  struct TreeEntry* entries = arena_new(arena, count, struct TreeEntry);
  for (ptrdiff_t i = 0; i < count; i++) {
    entries[i] = (struct TreeEntry){
        .name = str_from_cstr(names[i]),
        .path = str_from_cstr(names[i]),
        .kind = TREE_ENTRY_FILE,
        .len = 1,
        .is_linked = true,
    };
  }
  return (struct TreeDir){.entries = entries, .entry_count = count};
}

// Markdown and Org files, in any case, render, and nothing else does.
static void test_is_markup_goes_by_extension(void) {
  TEST_CHECK(readme_is_markup(S("README.MD")) && readme_is_markup(S("a.markdown")));
  TEST_CHECK(readme_is_markup(S("notes.Org")) && readme_is_org(S("notes.Org")));
  TEST_CHECK(!readme_is_markup(S("a.txt")) && !readme_is_org(S("a.md")));
}

// The first file in byte order whose base name is the kind's, in any case, with no extension or a
// document's, is the document.
static void test_find_matches_base_names_in_any_case(void) {
  struct Arena arena = test_arena();
  const char* names[] = {"COPYING", "License.md", "notes.txt", "readme.rst", "readme.txt"};
  const struct TreeDir dir = make_dir(names, 5, &arena);
  const struct TreeEntry* license = readme_find(&dir, DOCUMENT_LICENSE, 100);
  const struct TreeEntry* readme = readme_find(&dir, DOCUMENT_README, 100);
  TEST_ASSERT(license != NULL && readme != NULL);
  TEST_CHECK(test_text_is(license->name, "COPYING"));
  TEST_CHECK(test_text_is(readme->name, "readme.txt"));
  TEST_CHECK(readme_find(&dir, DOCUMENT_SECURITY, 100) == NULL);
}

// A symlink, a file without pages, and a file over the limit are no document.
static void test_find_skips_links_unlinked_and_large_files(void) {
  struct Arena arena = test_arena();
  const char* names[] = {"README", "README.md", "README.org"};
  struct TreeDir dir = make_dir(names, 3, &arena);
  dir.entries[0].kind = TREE_ENTRY_SYMLINK;
  dir.entries[1].is_linked = false;
  dir.entries[2].len = 101;
  TEST_CHECK(readme_find(&dir, DOCUMENT_README, 100) == NULL);
  dir.entries[2].len = 100;
  TEST_CHECK(readme_find(&dir, DOCUMENT_README, 100) == &dir.entries[2]);
}

// `ACCESSIBILITY` is a document in any case, with a document's extension.
static void test_find_matches_accessibility_in_any_case(void) {
  struct Arena arena = test_arena();
  const char* names[] = {"Accessibility.MD", "SECURITY.md"};
  const struct TreeDir dir = make_dir(names, 2, &arena);
  const struct TreeEntry* accessibility = readme_find(&dir, DOCUMENT_ACCESSIBILITY, 100);
  TEST_ASSERT(accessibility != NULL);
  TEST_CHECK(test_text_is(accessibility->name, "Accessibility.MD"));
  TEST_CHECK(test_text_is(document_kind_name(DOCUMENT_ACCESSIBILITY), "accessibility"));
}

// The root's `ACCESSIBILITY` comes from `docs/` when the root has none, and sorts after `SECURITY`,
// the last of the documents.
static void test_find_documents_reads_accessibility_after_security_from_docs(void) {
  struct Arena arena = test_arena();
  const char* root_names[] = {"SECURITY.md"};
  const char* docs_names[] = {"accessibility.txt"};
  struct TreeDir dirs[2] = {make_dir(root_names, 1, &arena), make_dir(docs_names, 1, &arena)};
  dirs[1].path = S("docs");
  const struct BranchTree tree = {.dirs = dirs, .dir_count = 2};
  const struct TreeEntry* entries[DOCUMENT_KIND_COUNT] = {0};
  TEST_CHECK(readme_find_documents(&tree, &dirs[0], 100, entries) == 2);
  TEST_CHECK(entries[DOCUMENT_SECURITY] == &dirs[0].entries[0]);
  TEST_CHECK(entries[DOCUMENT_ACCESSIBILITY] == &dirs[1].entries[0]);
  TEST_CHECK(DOCUMENT_ACCESSIBILITY == DOCUMENT_SECURITY + 1);
  TEST_CHECK(DOCUMENT_KIND_COUNT == DOCUMENT_ACCESSIBILITY + 1);
}

// A relative link resolves against the document's directory, a `/`-rooted one against the root, and
// each keeps its query and fragment.
static void test_render_markup_resolves_relative_and_rooted_links(void) {
  ptrdiff_t missing = 0;
  const struct Str html =
      render("[a](../lib/main.c?x=1#L2) [b](/README.md) [c](./guide.md)", false, &missing);
  TEST_CHECK(
      test_text_is(html,
                   "<p><a href=\"../../r/blob/feature/x/lib/main.c/index.html?x=1#L2\">a</a> "
                   "<a href=\"../../r/blob/feature/x/README.md/index.html\">b</a> "
                   "<a href=\"../../r/blob/feature/x/docs/guide.md/index.html\">c</a></p>\n"));
  TEST_CHECK(missing == 0);
}

// A target that leaves out `.md` finds the Markdown file, a directory its tree page even beside a
// Markdown file of its name, the root the root's tree page, and an image its raw copy.
static void test_render_markup_finds_md_files_directories_and_images(void) {
  ptrdiff_t missing = 0;
  const struct Str html =
      render("[g](guide) [l](../lib/) [r](..) ![d](../media/dot.png)", false, &missing);
  TEST_CHECK(test_text_is(html,
                          "<p><a href=\"../../r/blob/feature/x/docs/guide.md/index.html\">g</a> "
                          "<a href=\"../../r/tree/feature/x/lib/index.html\">l</a> "
                          "<a href=\"../../r/tree/feature/x/index.html\">r</a> "
                          "<img src=\"../../r/raw/feature/x/media/dot.png\" alt=\"d\"></p>\n"));
  TEST_CHECK(missing == 0);
}

// An image of a file over the raw limit, which has no raw copy, shows its blob page.
static void test_render_markup_links_an_image_without_a_raw_copy_to_its_blob_page(void) {
  ptrdiff_t missing = 0;
  const struct Str html = render("![b](../media/big.png)", false, &missing);
  TEST_CHECK(test_text_is(
      html, "<p><img src=\"../../r/blob/feature/x/media/big.png/index.html\" alt=\"b\"></p>\n"));
  TEST_CHECK(missing == 0);
}

// A `%XX` escape in a target is decoded before the lookup.
static void test_render_markup_decodes_escapes(void) {
  ptrdiff_t missing = 0;
  const struct Str html = render("[g](%67uide.md)", false, &missing);
  TEST_CHECK(test_text_is(
      html, "<p><a href=\"../../r/blob/feature/x/docs/guide.md/index.html\">g</a></p>\n"));
  TEST_CHECK(missing == 0);
}

// An Org `file:` link's `::#<id>` becomes the fragment, and its other search options are dropped.
static void test_render_markup_splits_org_search_options(void) {
  ptrdiff_t missing = 0;
  const struct Str html =
      render("[[file:notes.org::#setup][s]] [[file:notes.org::*Install][i]]", true, &missing);
  TEST_CHECK(
      test_text_is(html,
                   "<p><a href=\"../../r/blob/feature/x/docs/notes.org/index.html#setup\">s</a> "
                   "<a href=\"../../r/blob/feature/x/docs/notes.org/index.html\">i</a></p>\n"));
  TEST_CHECK(missing == 0);
}

// Markdown's front matter shows as a fenced block of its language, TOML or YAML, unread, whose
// fence outlasts any run of backticks in it. An Org file and an unclosed block stay as they are.
static void test_render_markup_fences_front_matter(void) {
  ptrdiff_t missing = 0;
  TEST_CHECK(
      test_text_is(render("+++\ntitle = \"A\"\n+++\nBody.\n", false, &missing),
                   "<pre><code class=\"language-toml\">title = &quot;A&quot;\n</code></pre>\n"
                   "<p>Body.</p>\n"));
  TEST_CHECK(
      test_text_is(render("---\ns: \"````\"\n---\n", false, &missing),
                   "<pre><code class=\"language-yaml\">s: &quot;````&quot;\n</code></pre>\n"));
  TEST_CHECK(test_text_is(render("+++\nA\n", false, &missing), "<p>+++\nA</p>\n"));
  TEST_CHECK(test_text_is(render("+++\nA\n+++\n", true, &missing),
                          "<p><del>+</del>\nA\n<del>+</del></p>\n"));
  TEST_CHECK(missing == 0);
}

// An absolute URL, a fragment of the page itself and a link out of the repo are left as written and
// not counted, and a relative link to no path is left as written and counted.
static void test_render_markup_leaves_other_targets_as_written(void) {
  ptrdiff_t missing = 0;
  const struct Str html = render(
      "[a](https://example.org/) [b](#top) [c](../../outside) [d](missing.md)", false, &missing);
  TEST_CHECK(test_text_is(html,
                          "<p><a href=\"https://example.org/\">a</a> <a href=\"#top\">b</a> "
                          "<a href=\"../../outside\">c</a> <a href=\"missing.md\">d</a></p>\n"));
  TEST_CHECK(missing == 1);
}

TEST_LIST = {
    {"is markup goes by extension", test_is_markup_goes_by_extension},
    {"find matches base names in any case", test_find_matches_base_names_in_any_case},
    {"find skips links unlinked and large files", test_find_skips_links_unlinked_and_large_files},
    {"find matches accessibility in any case", test_find_matches_accessibility_in_any_case},
    {"find documents reads accessibility after security from docs",
     test_find_documents_reads_accessibility_after_security_from_docs},
    {"render markup resolves relative and rooted links",
     test_render_markup_resolves_relative_and_rooted_links},
    {"render markup finds md files directories and images",
     test_render_markup_finds_md_files_directories_and_images},
    {"render markup links an image without a raw copy to its blob page",
     test_render_markup_links_an_image_without_a_raw_copy_to_its_blob_page},
    {"render markup decodes escapes", test_render_markup_decodes_escapes},
    {"render markup splits org search options", test_render_markup_splits_org_search_options},
    {"render markup fences front matter", test_render_markup_fences_front_matter},
    {"render markup leaves other targets as written",
     test_render_markup_leaves_other_targets_as_written},
    {NULL, NULL},
};
