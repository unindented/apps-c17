#include <acutest.h>
#include <errno.h>
#include <ftw.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/cmd_build.h"
#include "cli/cli.h"
#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/path_list.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_posix.h"
#include "output/build_cache_file.h"
#include "output/output_cache.h"
#include "runtime/job.h"
#include "runtime/walk.h"
#include "testing/test_support.h"

// A build reads its repositories through libgit2, which reaches the real filesystem, so these tests
// run on the POSIX host. Each builds a site in a directory of its own below the system's temporary
// directory, from the templates of the golden fixture site and the fixture repository `showcase`,
// which `surge.fixture_repos_setup` makes before them.

/** Templates of the golden fixture site, relative to the project directory the tests run in. */
#define FIXTURE_TEMPLATES_DIR "tests/fixtures/site/templates"

/**
 * @brief Removes one entry of a test site, as `nftw` visits it after its contents.
 *
 * @param path   Path of the entry. Must not be `NULL`.
 * @param stat   Unused.
 * @param flag   Unused.
 * @param walker Unused.
 * @return `0`, so the walk goes on whatever one removal does.
 */
static int remove_entry(const char* path, const struct stat* stat, int flag, struct FTW* walker) {
  (void)stat;
  (void)flag;
  (void)walker;
  (void)remove(path);
  return 0;
}

/**
 * @brief Removes a test site and everything below it.
 *
 * @param site_dir Terminated path of the site. Must not be `NULL`.
 */
static void remove_site(const char* site_dir) {
  (void)nftw(site_dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/**
 * @brief Makes a site below the temporary directory: the golden site's templates, and a config of
 *        `config_extra` and one repo, the fixture repository `showcase`, with `repo_extra`.
 *
 * @param config_extra Terminated keys that lead the config, or `NULL` for none.
 * @param repo_extra   Terminated keys that end the repo's table, or `NULL` for none.
 * @param arena        Arena that owns the returned path. Must not be `NULL`.
 * @return The site's directory.
 */
static struct Str make_site(const char* config_extra, const char* repo_extra, struct Arena* arena) {
  const char* repos_dir = getenv("SURGE_FIXTURE_REPOS_DIR");
  TEST_ASSERT(repos_dir != NULL);
  const char* tmp_dir = getenv("TMPDIR");
  struct Buf site_template = {.arena = arena};
  buf_printf(&site_template, "%s/surge-test-XXXXXX", tmp_dir != NULL ? tmp_dir : "/tmp");
  char* site_dir = (char*)buf_cstr(&site_template);
  TEST_ASSERT(mkdtemp(site_dir) != NULL);
  const struct Str site = str_from_cstr(site_dir);

  struct Os* os = os_posix();
  struct Log log = test_log();
  struct PathList templates = {0};
  static const struct Str all_suffixes[] = {STR_INIT("")};
  TEST_ASSERT(walk_files_with_suffixes(&templates, S(FIXTURE_TEMPLATES_DIR), S("public"),
                                       all_suffixes, 1, false, os, arena, test_arena(),
                                       S("failed to list templates"), &log) == 0);
  for (ptrdiff_t i = 0; i < templates.count; i++) {
    struct Arena scratch = *arena;
    struct Str text = {0};
    TEST_ASSERT(os_read(os, templates.items[i], 1024 * 1024, OS_READ_TEXT, &scratch, &text,
                        S("failed to read template"), &log) == 0);
    struct Str name = {0};
    (void)path_is_below(templates.items[i], S(FIXTURE_TEMPLATES_DIR), &name);
    const struct Str copy = path_join(path_join(site, S("templates"), &scratch), name, &scratch);
    TEST_ASSERT(os_write(os, copy, (struct OsSource){.bytes = text}, S("failed to write template"),
                         &log) == 0);
  }
  struct Buf config = {.arena = arena};
  if (config_extra != NULL) {
    buf_append(&config, str_from_cstr(config_extra));
  }
  buf_printf(&config,
             "title = \"Code\"\n"
             "description = \"Repositories\"\n"
             "author = \"Ada\"\n"
             "base_url = \"https://git.example.org\"\n"
             "\n"
             "[[repo]]\n"
             "name = \"showcase\"\n"
             "path = \"%s/showcase\"\n"
             "%s",
             repos_dir, repo_extra != NULL ? repo_extra : "");
  TEST_ASSERT(os_write(os, path_join(site, S("surge.toml"), arena),
                       (struct OsSource){.bytes = buf_str(&config)}, S("failed to write config"),
                       &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  return site;
}

/**
 * @brief Builds the site in `site_dir` on one worker, with `options` for the rest, in test arenas.
 *
 * @param site_dir Directory of the site, whose `surge.toml` the build reads.
 * @param options  Options of the build, whose config path and worker count are filled in. Must not
 *                 be `NULL`.
 * @param log      Log that receives every error. Must not be `NULL`.
 * @return What `cmd_build_execute` returns.
 */
static int execute_build(struct Str site_dir, struct BuildOptions options, struct Log* log) {
  struct Arena perm = test_arena();
  options.config_path = path_join(site_dir, S("surge.toml"), &perm);
  options.worker_count = 1;
  struct JobArenas* worker_arenas = arena_new(&perm, 1, struct JobArenas);
  worker_arenas[0] = (struct JobArenas){.perm = test_arena(), .scratch = test_arena()};
  return cmd_build_execute(&options, worker_arenas, os_posix(), perm, test_arena(), log);
}

/**
 * @brief Returns what `os_stat` reports for `name` below `site_dir`, zeroed when it fails.
 *
 * @param site_dir     Directory of the site.
 * @param name         Terminated path below it. Must not be `NULL`.
 * @param is_found_out Receives whether the path exists. Must not be `NULL`.
 * @return The stat.
 */
static struct OsStat stat_below(struct Str site_dir, const char* name, bool* is_found_out) {
  struct Arena arena = test_arena();
  struct OsStat st = {0};
  *is_found_out = os_stat(os_posix(), path_join(site_dir, str_from_cstr(name), &arena), &st) == 0;
  return st;
}

/**
 * @brief Returns `attributes` with each value, which names an id, replaced by `#`.
 *
 * @param attributes Space-separated `key=value` pairs.
 * @param arena      Arena that owns the result. Must not be `NULL`.
 * @return The masked attributes.
 */
static struct Str mask_ids(struct Str attributes, struct Arena* arena) {
  struct Buf masked = {.arena = arena};
  struct StrCut cut = {.tail = attributes, .is_found = true};
  while (cut.is_found) {
    cut = str_cut(cut.tail, ' ');
    buf_append(&masked, str_cut(cut.head, '=').head);
    buf_append(&masked, cut.is_found ? S("=# ") : S("=#"));
  }
  return buf_str(&masked);
}

/**
 * @brief Writes `text` to `name` below `site_dir`, creating its parent directories.
 *
 * @param site_dir Directory of the site.
 * @param name     Terminated path below it. Must not be `NULL`.
 * @param text     Terminated contents. Must not be `NULL`.
 */
static void write_below(struct Str site_dir, const char* name, const char* text) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  TEST_ASSERT(os_write(os_posix(), path_join(site_dir, str_from_cstr(name), &arena),
                       (struct OsSource){.bytes = str_from_cstr(text)}, S("failed to write"),
                       &log) == 0);
}

/**
 * @brief Reports whether `name` below `site_dir` holds exactly `expected`.
 *
 * @param site_dir Directory of the site.
 * @param name     Terminated path below it. Must not be `NULL`.
 * @param expected Terminated text the file must hold. Must not be `NULL`.
 * @return `true` when the file is read and holds `expected`.
 */
static bool is_file_below(struct Str site_dir, const char* name, const char* expected) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct Str text = {0};
  if (os_read(os_posix(), path_join(site_dir, str_from_cstr(name), &arena), 1024 * 1024,
              OS_READ_TEXT, &arena, &text, S("failed to read"), &log) != 0) {
    return false;
  }
  return test_text_is(text, "%s", expected);
}

/**
 * @brief Returns the page recipe that the output cache of the site's last build records for every
 *        page, or empty when two rows name different recipes.
 *
 * @param site_dir Directory of the site.
 * @param arena    Arena that owns the recipe. Must not be `NULL`.
 * @return The recipe's id, or empty.
 */
static struct Str cached_page_recipe(struct Str site_dir, struct Arena* arena) {
  struct OutputCache cache = {0};
  struct Str reason = {0};
  TEST_ASSERT(output_cache_load(path_join(site_dir, S("public/_surge/output-cache.tsv"), arena),
                                os_posix(), arena, test_arena(), &cache,
                                &reason) == BUILD_CACHE_FILE_LOADED);
  struct Str recipe = {0};
  ptrdiff_t page_count = 0;
  for (ptrdiff_t i = 0; i < cache.row_count; i++) {
    // Every page and feed declares the recipe first, and no other output declares one.
    const struct OutputCacheRow* row = &cache.rows[i];
    if (!str_startswith(row->attributes, S("recipe="))) {
      continue;
    }
    const struct Str id = str_cut(str_cut(row->attributes, ' ').head, '=').tail;
    if (recipe.len > 0 && !str_eq(id, recipe)) {
      TEST_MSG("page '%.*s' names another recipe: '%.*s'", STR_FMT(row->relative_path),
               STR_FMT(row->attributes));
      return (struct Str){0};
    }
    recipe = id;
    page_count++;
  }
  TEST_CHECK(page_count > 0);
  return recipe;
}

// A build of a site writes its pages and its output cache, and logs nothing.
static void test_builds_the_site_and_its_output_cache(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  bool is_found = false;
  (void)stat_below(site, "public/index.html", &is_found);
  TEST_CHECK(is_found);
  const struct OsStat cache = stat_below(site, "public/_surge/output-cache.tsv", &is_found);
  TEST_CHECK(is_found && cache.len > 0);
  remove_site(str_cstr(site, &arena));
}

// Two keys may name one template, which renders each kind of page with that page's own data.
static void test_renders_a_shared_template_with_each_page_s_data(void) {
  struct Arena arena = test_arena();
  const struct Str site =
      make_site("templates.commit_list = \"shared.html\"\ntemplates.commit = \"shared.html\"\n",
                NULL, &arena);
  write_below(site, "templates/shared.html",
              "{{#page}}list {{number}}{{/page}}{{^page}}commit {{commit.short_oid}}{{/page}}\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(is_file_below(site, "public/showcase/commits/main/index.html", "list 1\n"));
  TEST_CHECK(is_file_below(
      site, "public/showcase/commit/119edc43de569e78adf5af0a0608e005771ac3ca/index.html",
      "commit 119edc4\n"));
  remove_site(str_cstr(site, &arena));
}

// A key that names another template between two builds changes the page recipe that every page
// declares, so the second build makes every page again, as a template edit does. Both templates are
// below `templates_dir` for both builds, so only the key changes.
static void test_changed_template_key_makes_every_page_again(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  write_below(site, "templates/home.html", "home\n");
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct Str recipe = cached_page_recipe(site, &arena);
  TEST_CHECK(recipe.len > 0);

  struct Arena config_arena = test_arena();
  struct Str config = {0};
  const struct Str config_path = path_join(site, S("surge.toml"), &config_arena);
  TEST_ASSERT(os_read(os_posix(), config_path, 1024 * 1024, OS_READ_TEXT, &config_arena, &config,
                      S("failed to read config"), &log) == 0);
  write_below(site, "surge.toml",
              str_cstr(str_concat(S("templates.tree = \"home.html\"\n"), config, &config_arena),
                       &config_arena));
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  const struct Str changed = cached_page_recipe(site, &arena);
  TEST_CHECK(changed.len > 0 && !str_eq(changed, recipe));
  TEST_CHECK(is_file_below(site, "public/showcase/index.html", "home\n"));
  remove_site(str_cstr(site, &arena));
}

// A dry run of a site that was never built creates nothing, not even `output_dir`.
static void test_dry_run_creates_nothing(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){.is_dry_run = true}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  bool is_found = true;
  (void)stat_below(site, "public", &is_found);
  TEST_CHECK(!is_found);
  remove_site(str_cstr(site, &arena));
}

// An `output_dir` that exists as a file cannot be prepared, which the build reports with the path.
static void test_reports_an_output_dir_that_is_a_file(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site("output_dir = \"out\"\n", NULL, &arena);
  const struct Str out = path_join(site, S("out"), &arena);
  struct Log write_log = test_log();
  TEST_ASSERT(os_write(os_posix(), out, (struct OsSource){.bytes = S("x")}, S("failed to write"),
                       &write_log) == 0);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to prepare output directory: exists and is not a directory "
                         "('%.*s')\n",
                         STR_FMT(out)));
  remove_site(str_cstr(site, &arena));
}

// A build that fails after it starts writing still writes its output cache, with a row for each
// page it wrote and none for the page that failed, so the next build keeps only what this one made.
static void test_failed_build_records_what_it_wrote(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  struct Log write_log = test_log();
  TEST_ASSERT(os_write(os_posix(), path_join(site, S("templates/index.html"), &arena),
                       (struct OsSource){.bytes = S("{{{site.title}}}")}, S("failed to write"),
                       &write_log) == 0);
  log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == -1);
  TEST_CHECK(
      test_log_is(&log,
                  "unescaped Mustache interpolation is supported only for a key that ends in "
                  "'_html' (in 'index.html') (while rendering 'index.html')\n"));
  struct OutputCache cache = {0};
  struct Str reason = {0};
  TEST_ASSERT(output_cache_load(path_join(site, S("public/_surge/output-cache.tsv"), &arena),
                                os_posix(), &arena, test_arena(), &cache,
                                &reason) == BUILD_CACHE_FILE_LOADED);
  const struct OutputCacheRow* row = output_cache_find(&cache, S("showcase/index.html"));
  TEST_ASSERT(row != NULL);
  if (row == NULL) {
    return;
  }
  TEST_CHECK(str_eq(mask_ids(row->attributes, &arena), S("recipe=# map=# repo=# refs=#")));
  TEST_MSG("attributes: '%.*s'", STR_FMT(row->attributes));
  bool is_found = false;
  const struct OsStat page = stat_below(site, "public/showcase/index.html", &is_found);
  TEST_CHECK(is_found && row->len == page.len);
  TEST_CHECK(output_cache_find(&cache, S("index.html")) == NULL);
  remove_site(str_cstr(site, &arena));
}

// A page template that does not compile stops the build when the templates load, so a static entry
// that collides with a repo, which the manifest would refuse, is never reached, and nothing is
// written.
static void test_stops_at_a_template_that_does_not_load(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  struct Log write_log = test_log();
  TEST_ASSERT(os_write(os_posix(), path_join(site, S("templates/tree.html"), &arena),
                       (struct OsSource){.bytes = S("{{#entries}}")}, S("failed to write"),
                       &write_log) == 0);
  TEST_ASSERT(os_write(os_posix(), path_join(site, S("static/showcase"), &arena),
                       (struct OsSource){.bytes = S("x")}, S("failed to write"), &write_log) == 0);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "section-opening tag has no closer at line 1, column 1 (in 'tree.html')\n"));
  bool is_found = true;
  (void)stat_below(site, "public", &is_found);
  TEST_CHECK(!is_found);
  remove_site(str_cstr(site, &arena));
}

// A template that a key names and that is missing stops the build when the templates load, naming
// the template and the key. A template that two keys name names the first in table order.
static void test_reports_a_missing_template_with_its_key(void) {
  static const struct {
    const char* config;
    const char* key;
  } cases[] = {
      {"templates.tree = \"missing.html\"\n", "templates.tree"},
      {"templates.not_found = \"missing.html\"\ntemplates.blob = \"missing.html\"\n",
       "templates.blob"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    const struct Str site = make_site(cases[i].config, NULL, &arena);
    struct Log log = test_log();
    TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == -1);
    const struct Str missing = path_join(site, S("templates/missing.html"), &arena);
    TEST_CHECK(test_log_is(&log, "failed to read template: %s ('%.*s') (for '%s')\n",
                           strerror(ENOENT), STR_FMT(missing), cases[i].key));
    bool is_found = true;
    (void)stat_below(site, "public", &is_found);
    TEST_CHECK(!is_found);
    remove_site(str_cstr(site, &arena));
  }
}

// A release template is needed only by a site with a release project: without one, a missing
// `release.html` fails no build, and with a repo's `releases_dir`, it stops the build, named with
// its key.
static void test_needs_release_templates_only_with_a_release_project(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, NULL, &arena);
  TEST_ASSERT(remove(str_cstr(path_join(site, S("templates/release.html"), &arena), &arena)) == 0);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  remove_site(str_cstr(site, &arena));

  const struct Str release_site = make_site(NULL, "releases_dir = \"releases\"\n", &arena);
  const struct Str release_path = path_join(release_site, S("templates/release.html"), &arena);
  TEST_ASSERT(remove(str_cstr(release_path, &arena)) == 0);
  log = test_log();
  TEST_CHECK(execute_build(release_site, (struct BuildOptions){0}, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to read template: %s ('%.*s') (for 'templates.release')\n",
                         strerror(ENOENT), STR_FMT(release_path)));
  remove_site(str_cstr(release_site, &arena));
}

// A root that fails its check stops the build before the overlap check, so a `static_dir` that is a
// file is the one error even with an `output_dir` inside `templates_dir`.
static void test_stops_at_a_bad_root_before_the_overlap_check(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site("output_dir = \"templates/public\"\n", NULL, &arena);
  const struct Str static_path = path_join(site, S("static"), &arena);
  struct Log write_log = test_log();
  TEST_ASSERT(os_write(os_posix(), static_path, (struct OsSource){.bytes = S("x")},
                       S("failed to write"), &write_log) == 0);
  struct Log log = test_log();
  TEST_CHECK(execute_build(site, (struct BuildOptions){0}, &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "failed to resolve config directory 'static_dir': not a directory ('%.*s')\n",
      STR_FMT(static_path)));
  bool is_found = true;
  (void)stat_below(site, "templates/public", &is_found);
  TEST_CHECK(!is_found);
  remove_site(str_cstr(site, &arena));
}

TEST_LIST = {
    {"builds the site and its output cache", test_builds_the_site_and_its_output_cache},
    {"renders a shared template with each page's data",
     test_renders_a_shared_template_with_each_page_s_data},
    {"changed template key makes every page again",
     test_changed_template_key_makes_every_page_again},
    {"dry run creates nothing", test_dry_run_creates_nothing},
    {"reports an output dir that is a file", test_reports_an_output_dir_that_is_a_file},
    {"failed build records what it wrote", test_failed_build_records_what_it_wrote},
    {"stops at a template that does not load", test_stops_at_a_template_that_does_not_load},
    {"reports a missing template with its key", test_reports_a_missing_template_with_its_key},
    {"needs release templates only with a release project",
     test_needs_release_templates_only_with_a_release_project},
    {"stops at a bad root before the overlap check",
     test_stops_at_a_bad_root_before_the_overlap_check},
    {NULL, NULL},
};
