#include <acutest.h>
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
 *        `config_extra` and one repo, the fixture repository `showcase`.
 *
 * @param config_extra Terminated keys that lead the config, or `NULL` for none.
 * @param arena        Arena that owns the returned path. Must not be `NULL`.
 * @return The site's directory.
 */
static struct Str make_site(const char* config_extra, struct Arena* arena) {
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
             "path = \"%s/showcase\"\n",
             repos_dir);
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

// A build of a site writes its pages and its output cache, and logs nothing.
static void test_builds_the_site_and_its_output_cache(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, &arena);
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

// A dry run of a site that was never built creates nothing, not even `output_dir`.
static void test_dry_run_creates_nothing(void) {
  struct Arena arena = test_arena();
  const struct Str site = make_site(NULL, &arena);
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
  const struct Str site = make_site("output_dir = \"out\"\n", &arena);
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
  const struct Str site = make_site(NULL, &arena);
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

TEST_LIST = {
    {"builds the site and its output cache", test_builds_the_site_and_its_output_cache},
    {"dry run creates nothing", test_dry_run_creates_nothing},
    {"reports an output dir that is a file", test_reports_an_output_dir_that_is_a_file},
    {"failed build records what it wrote", test_failed_build_records_what_it_wrote},
    {NULL, NULL},
};
