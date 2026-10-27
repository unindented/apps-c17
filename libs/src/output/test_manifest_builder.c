#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/log.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os_memory.h"
#include "output/manifest.h"
#include "output/manifest_builder.h"
#include "testing/test_support.h"

/**
 * @brief Returns a host with the inputs of a small site: `tool.toml`, `templates/index.html` and a
 *        bare repository's directory `srv/one.git`.
 *
 * @return The host.
 */
static struct Os* make_site(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "tool.toml", "config");
  os_memory_write(os, "templates/index.html", "template");
  os_memory_write(os, "srv/one.git/HEAD", "ref: refs/heads/main\n");
  return os;
}

/**
 * @brief Prepares a builder over the site of `make_site`, with `public` as `output_dir`, the
 *        templates and the repository as roots, and the config as an input.
 *
 * @param builder  Builder to prepare. Must not be `NULL`.
 * @param manifest Empty manifest. Must not be `NULL`.
 * @param os       Host of the site. Must not be `NULL`.
 * @param perm     Arena that owns the builder's state. Must not be `NULL`.
 */
static void init_builder(struct ManifestBuilder* builder,
                         struct Manifest* manifest,
                         struct Os* os,
                         struct Arena* perm) {
  struct ManifestRoot* roots = arena_new(perm, 2, struct ManifestRoot);
  roots[0] = (struct ManifestRoot){.label = S("'templates_dir'"), .dir_path = S("templates")};
  roots[1] = (struct ManifestRoot){.label = S("the git directory of repo 'one'"),
                                   .dir_path = S("srv/one.git")};
  struct PathList inputs = {0};
  path_list_push(&inputs, S("tool.toml"), perm);
  manifest_builder_init(builder, manifest, S("public"), roots, 2, &inputs, os, perm);
}

// An `output_dir` apart from every root and from the config passes.
static void test_check_output_dir_accepts_a_separate_dir(void) {
  struct Os* os = make_site();
  const struct ManifestRoot roots[] = {{.label = S("'templates_dir'"), .dir_path = S("templates")}};
  struct Log log = test_log();
  os_memory_mkdir(os, "site/public");
  os_memory_write(os, "site/tool.toml", "config");
  TEST_CHECK(manifest_builder_check_output_dir(S("site/public"), roots, 1, S("site/tool.toml"), os,
                                               test_arena(), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// An `output_dir` inside a repository's git directory is refused, whether or not it exists.
static void test_check_output_dir_rejects_a_dir_inside_a_root(void) {
  struct Os* os = make_site();
  const struct ManifestRoot roots[] = {
      {.label = S("the git directory of repo 'one'"), .dir_path = S("srv/one.git")}};
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_check_output_dir(S("srv/one.git/public/site"), roots, 1,
                                               S("tool.toml"), os, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output directory would write inside the git directory of repo 'one': "
                         "'srv/one.git/public/site'\n"));
}

// An `output_dir` that holds a repository, as `/srv/git` holds `/srv/git/foo.git`, is refused.
static void test_check_output_dir_rejects_a_dir_holding_a_root(void) {
  struct Os* os = make_site();
  const struct ManifestRoot roots[] = {
      {.label = S("the git directory of repo 'one'"), .dir_path = S("srv/one.git")}};
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_check_output_dir(S("srv"), roots, 1, S("site/tool.toml"), os,
                                               test_arena(), &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "output directory would hold the git directory of repo 'one': 'srv'\n"));
}

// An `output_dir` that holds the config, such as `.`, is refused.
static void test_check_output_dir_rejects_a_dir_holding_the_config(void) {
  struct Os* os = make_site();
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_check_output_dir(S("."), NULL, 0, S("tool.toml"), os, test_arena(),
                                               &log) == -1);
  TEST_CHECK(test_log_is(&log, "output directory would hold the config: '.'\n"));
}

// A path claimed twice, in any case, is refused with both labels.
static void test_add_rejects_duplicates(void) {
  struct Os* os = make_site();
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_add(&builder, S("index.html"), S("site index"), test_arena(), &log) ==
             0);
  TEST_CHECK(
      manifest_builder_add(&builder, S("INDEX.html"), S("static file"), test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'site index' and 'static file': "
                         "'public/INDEX.html'\n"));
}

// A path through a symlink into a root, and a path that names the config, are refused.
static void test_add_rejects_paths_into_inputs(void) {
  struct Os* os = make_site();
  os_memory_symlink(os, "../srv/one.git", "public/link");
  os_memory_symlink(os, "..", "public/up");
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_add(&builder, S("link/HEAD"), S("page"), test_arena(), &log) == -1);
  TEST_CHECK(manifest_builder_add(&builder, S("up/tool.toml"), S("page"), test_arena(), &log) ==
             -1);
  TEST_CHECK(test_log_is(&log,
                         "output path would write inside the git directory of repo 'one' for "
                         "'page': 'public/link/HEAD'\n"
                         "output path would overwrite build input for 'page': "
                         "'public/up/tool.toml'\n"));
}

// An output inside a root is reported once per cause: an output through the same directory from the
// same source, or from a source below it, fails without a line of its own. Another source through
// that directory, the same source through another directory, and a source reported again after
// another one are each reported.
static void test_add_reports_a_root_once_per_source(void) {
  struct Os* os = make_site();
  os_memory_symlink(os, "../templates", "public/link");
  os_memory_symlink(os, "../templates", "public/other");
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  struct Log log = test_log();
  static const char* const outputs[][2] = {
      {"link/a.html", "photos"}, {"link/b.html", "photos/sub"}, {"link/c.css", "static/c.css"},
      {"link/d.html", "photos"}, {"other/e.html", "photos"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(outputs) / sizeof(outputs[0])); i++) {
    TEST_CHECK(manifest_builder_add(&builder, str_from_cstr(outputs[i][0]),
                                    str_from_cstr(outputs[i][1]), test_arena(), &log) == -1);
  }
  TEST_CHECK(test_log_is(&log,
                         "output path would write inside 'templates_dir' for 'photos': "
                         "'public/link/a.html'\n"
                         "output path would write inside 'templates_dir' for 'static/c.css': "
                         "'public/link/c.css'\n"
                         "output path would write inside 'templates_dir' for 'photos': "
                         "'public/other/e.html'\n"));
}

// Files are recorded at their paths below their root, labelled by the files, and a skipped file is
// left out. Every file is checked even after one is refused, and without a skip list every file is
// recorded.
static void test_add_files_records_each_file_below_its_root(void) {
  struct Os* os = make_site();
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  struct PathList paths = {0};
  path_list_push(&paths, S("static/a.css"), &perm);
  path_list_push(&paths, S("static/icons/b.svg"), &perm);
  path_list_push(&paths, S("static/c.txt"), &perm);
  const bool is_skipped[] = {false, false, true};
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_add_files(&builder, &paths, S("static"), is_skipped, test_arena(),
                                        &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(manifest.count == 2);
  TEST_CHECK(manifest_find_mark(&manifest, S("public/a.css")) == MANIFEST_MARK_PENDING);
  TEST_CHECK(manifest_builder_add(&builder, S("icons/b.svg"), S("page"), test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'static/icons/b.svg' and 'page': "
                         "'public/icons/b.svg'\n"));
}

// A template list is recorded by name, each labelled by its key and index, and every name is
// checked even after one is refused.
static void test_add_template_list_labels_each_name_by_its_index(void) {
  struct Os* os = make_site();
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  static const struct Str names[] = {STR_INIT("index.html"), STR_INIT("INDEX.html"),
                                     STR_INIT("feed.xml")};
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_add_template_list(&builder, S("aggregate_templates"), names, 3,
                                                test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "duplicate output path for 'aggregate_templates[0]' and "
                         "'aggregate_templates[1]': 'public/INDEX.html'\n"));
  TEST_CHECK(manifest.count == 2);
}

// A path that nests under another is reported once every path is recorded.
static void test_finish_reports_nesting(void) {
  struct Os* os = make_site();
  struct Arena perm = test_arena();
  struct Manifest manifest = {0};
  struct ManifestBuilder builder;
  init_builder(&builder, &manifest, os, &perm);
  struct Log log = test_log();
  TEST_CHECK(manifest_builder_add(&builder, S("a"), S("file"), test_arena(), &log) == 0);
  TEST_CHECK(manifest_builder_add(&builder, S("a/b.html"), S("page"), test_arena(), &log) == 0);
  TEST_CHECK(manifest_builder_finish(&builder, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'page' nests under output path for 'file': "
                         "'public/a/b.html'\n"));
}

TEST_LIST = {
    {"check output dir accepts a separate dir", test_check_output_dir_accepts_a_separate_dir},
    {"check output dir rejects a dir inside a root",
     test_check_output_dir_rejects_a_dir_inside_a_root},
    {"check output dir rejects a dir holding a root",
     test_check_output_dir_rejects_a_dir_holding_a_root},
    {"check output dir rejects a dir holding the config",
     test_check_output_dir_rejects_a_dir_holding_the_config},
    {"add rejects duplicates", test_add_rejects_duplicates},
    {"add rejects paths into inputs", test_add_rejects_paths_into_inputs},
    {"add reports a root once per source", test_add_reports_a_root_once_per_source},
    {"add files records each file below its root", test_add_files_records_each_file_below_its_root},
    {"add template list labels each name by its index",
     test_add_template_list_labels_each_name_by_its_index},
    {"finish reports nesting", test_finish_reports_nesting},
    {NULL, NULL},
};
