#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/** The required top-level keys, which every fixture starts with. */
#define SITE_KEYS                    \
  "title = \"Code\"\n"               \
  "description = \"Repositories\"\n" \
  "author = \"Ada\"\n"               \
  "base_url = \"https://git.example.org/\"\n"

/** One valid repo table, for fixtures whose subject is elsewhere. */
#define REPO_TABLE   \
  "[[repo]]\n"       \
  "name = \"one\"\n" \
  "path = \"repos/one\"\n"

/**
 * @brief Loads `text` as `surge.toml` on a fresh host into an initialized config.
 *
 * @param text       Terminated config text. Must not be `NULL`.
 * @param config_out Receives the loaded config. Must not be `NULL`.
 * @param log        Log that receives every error. Must not be `NULL`.
 * @return The result of `site_config_load`.
 */
static int load_text(const char* text, struct SiteConfig* config_out, struct Log* log) {
  struct Os* os = os_memory();
  os_memory_write(os, "surge.toml", text);
  site_config_init(config_out);
  // The values live in a reservation that is never released, so they outlive this call.
  struct Arena perm = test_arena();
  return site_config_load(config_out, S("surge.toml"), os, &perm, test_arena(), log);
}

/**
 * @brief Reports whether loading `text` reads the file and logs exactly `expected`.
 *
 * @param text     Terminated config text. Must not be `NULL`.
 * @param expected Terminated text the log must hold. Must not be `NULL`.
 * @return `true` when the load read the file and the log holds `expected`.
 */
static bool is_logged_as(const char* text, const char* expected) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  return load_text(text, &site_config, &log) == 0 && test_log_is(&log, "%s", expected);
}

// A config with only the required keys gets every default, and its `base_url` loses its trailing
// `/`.
static void test_load_applies_defaults(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS REPO_TABLE, &site_config, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(site_config.title, S("Code")));
  TEST_CHECK(str_eq(site_config.description, S("Repositories")));
  TEST_CHECK(str_eq(site_config.author, S("Ada")));
  TEST_CHECK(str_eq(site_config.base_url, S("https://git.example.org")));
  TEST_CHECK(str_eq(site_config.output_dir, S("public")));
  TEST_CHECK(str_eq(site_config.templates_dir, S("templates")));
  TEST_CHECK(str_eq(site_config.static_dir, S("static")));
  TEST_CHECK(site_config.log_page_size == 100);
  TEST_CHECK(site_config.feed_count == 25);
  TEST_CHECK(!site_config.has_headers_file);
  TEST_CHECK(site_config.limits.blob_len_max == 1024 * 1024);
  TEST_CHECK(site_config.limits.image_len_max == 2 * 1024 * 1024);
  TEST_CHECK(site_config.limits.raw_len_max == 100 * 1024 * 1024);
  TEST_CHECK(site_config.limits.diff_file_count_max == 300);
  TEST_CHECK(site_config.limits.diff_line_count_max == 20000);
  TEST_CHECK(site_config.limits.diff_len_max == 1024 * 1024);
  TEST_CHECK(site_config.limits.image_len_max == 2 * 1024 * 1024);
  TEST_CHECK(site_config.limits.pack_len_max == 25 * 1024 * 1024);
  TEST_ASSERT(site_config.repo_count == 1);
  TEST_CHECK(str_eq(site_config.repos[0].name, S("one")));
  TEST_CHECK(str_eq(site_config.repos[0].path, S("repos/one")));
  TEST_CHECK(site_config.repos[0].description.len == 0);
  TEST_CHECK(!site_config.repos[0].has_description);
  TEST_CHECK(site_config.repos[0].project_count == 0);
}

// Every optional key is read, in every table, and the paths lose their trailing `/`.
static void test_load_reads_every_key(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "output_dir = \"out/\"\n"
                                 "templates_dir = \"tpl\"\n"
                                 "static_dir = \"/srv/static\"\n"
                                 "log_page_size = 2\n"
                                 "feed_count = 0\n"
                                 "headers_file = true\n"
                                 "[limits]\n"
                                 "blob_len_max = 10\n"
                                 "raw_len_max = 11\n"
                                 "diff_file_count_max = 12\n"
                                 "diff_line_count_max = 13\n"
                                 "diff_len_max = 14\n"
                                 "image_len_max = 16\n"
                                 "pack_len_max = 15\n"
                                 "[[repo]]\n"
                                 "name = \"apps-c17\"\n"
                                 "path = \"../apps-c17/\"\n"
                                 "description = \"C17 apps\"\n"
                                 "default_branch = \"main\"\n"
                                 "releases_dir = \"releases\"\n"
                                 "[[repo.project]]\n"
                                 "name = \"sosig\"\n"
                                 "path = \"apps/sosig/\"\n"
                                 "description = \"Static site generator\"\n"
                                 "tag_prefix = \"sosig-v\"\n"
                                 "mise = true\n"
                                 "[[repo.project]]\n"
                                 "name = \"surge\"\n"
                                 "path = \"apps/surge\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(site_config.output_dir, S("out")));
  TEST_CHECK(str_eq(site_config.templates_dir, S("tpl")));
  TEST_CHECK(str_eq(site_config.static_dir, S("/srv/static")));
  TEST_CHECK(site_config.log_page_size == 2);
  TEST_CHECK(site_config.feed_count == 0);
  TEST_CHECK(site_config.has_headers_file);
  TEST_CHECK(site_config.limits.blob_len_max == 10 && site_config.limits.raw_len_max == 11);
  TEST_CHECK(site_config.limits.diff_file_count_max == 12 &&
             site_config.limits.diff_line_count_max == 13);
  TEST_CHECK(site_config.limits.diff_len_max == 14 && site_config.limits.pack_len_max == 15);
  TEST_CHECK(site_config.limits.image_len_max == 16);
  TEST_ASSERT(site_config.repo_count == 1);
  const struct SurgeRepo* repo = &site_config.repos[0];
  TEST_CHECK(str_eq(repo->path, S("../apps-c17")));
  TEST_CHECK(str_eq(repo->description, S("C17 apps")));
  TEST_CHECK(str_eq(repo->default_branch, S("main")));
  TEST_CHECK(str_eq(repo->releases_dir, S("releases")));
  TEST_ASSERT(repo->project_count == 2);
  TEST_CHECK(str_eq(repo->projects[0].name, S("sosig")));
  TEST_CHECK(str_eq(repo->projects[0].path, S("apps/sosig")));
  TEST_CHECK(str_eq(repo->projects[0].description, S("Static site generator")));
  TEST_CHECK(str_eq(repo->projects[0].tag_prefix, S("sosig-v")));
  TEST_CHECK(repo->projects[0].is_mise);
  TEST_CHECK(str_eq(repo->projects[1].name, S("surge")));
  TEST_CHECK(repo->projects[1].tag_prefix.len == 0);
  TEST_CHECK(!repo->projects[1].is_mise);
  TEST_CHECK(!repo->is_mise);
}

// A relative path, given or default, resolves against the config's directory, and an absolute one
// stays as it is.
static void test_load_resolves_paths_against_config_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site/surge.toml",
                  SITE_KEYS
                  "templates_dir = \"tpl\"\n"
                  "static_dir = \"/srv/static\"\n"
                  "[[repo]]\n"
                  "name = \"one\"\n"
                  "path = \"../one.git\"\n"
                  "releases_dir = \"releases\"\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Arena perm = test_arena();
  struct Log log = test_log();
  TEST_CHECK(site_config_load(&site_config, S("site/surge.toml"), os, &perm, test_arena(), &log) ==
             0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(site_config.output_dir, S("site/public")));
  TEST_CHECK(str_eq(site_config.templates_dir, S("site/tpl")));
  TEST_CHECK(str_eq(site_config.static_dir, S("/srv/static")));
  TEST_ASSERT(site_config.repo_count == 1);
  TEST_CHECK(str_eq(site_config.repos[0].path, S("site/../one.git")));
  TEST_CHECK(str_eq(site_config.repos[0].releases_dir, S("site/releases")));
}

// Two repos may each have a project of the same name.
static void test_load_accepts_project_names_across_repos(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\n"
                                 "[[repo.project]]\nname = \"lib\"\npath = \"x\"\n"
                                 "[[repo]]\nname = \"b\"\npath = \"b\"\n"
                                 "[[repo.project]]\nname = \"lib\"\npath = \"x\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// A project's path keeps any byte a tree name can hold, and loses its trailing `/`.
static void test_load_accepts_project_paths_of_any_bytes(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\n"
                                 "[[repo.project]]\nname = \"p\"\npath = \"libs/c++\"\n"
                                 "[[repo.project]]\nname = \"q\"\npath = \"docs/my guide/\"\n"
                                 "[[repo.project]]\nname = \"r\"\npath = \"packages/@scope/x\"\n"
                                 "[[repo.project]]\nname = \"s\"\npath = \"..a/b..\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_ASSERT(site_config.repo_count == 1 && site_config.repos[0].project_count == 4);
  TEST_CHECK(str_eq(site_config.repos[0].projects[0].path, S("libs/c++")));
  TEST_CHECK(str_eq(site_config.repos[0].projects[1].path, S("docs/my guide")));
  TEST_CHECK(str_eq(site_config.repos[0].projects[2].path, S("packages/@scope/x")));
  TEST_CHECK(str_eq(site_config.repos[0].projects[3].path, S("..a/b..")));
}

// Every missing required key is reported in one load, each named by its table.
static void test_load_reports_missing_required_keys(void) {
  TEST_CHECK(
      is_logged_as("[[repo]]\n"
                   "description = \"x\"\n"
                   "[[repo.project]]\n"
                   "tag_prefix = \"v\"\n",
                   "missing required config key 'title'\n"
                   "missing required config key 'description'\n"
                   "missing required config key 'author'\n"
                   "missing required config key 'base_url'\n"
                   "missing required config key 'repo[0].name'\n"
                   "missing required config key 'repo[0].path'\n"
                   "missing required config key 'repo[0].project[0].name'\n"
                   "missing required config key 'repo[0].project[0].path'\n"));
}

// A config without a repo, or with an empty list of them, has nothing to render.
static void test_load_requires_a_repo(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS, "missing required config key 'repo'\n"));
  TEST_CHECK(is_logged_as(SITE_KEYS "repo = []\n", "config key 'repo' must not be empty\n"));
}

// A key of the wrong type is reported apart from a missing one, and keeps its default, or stays
// empty when it is required.
static void test_load_reports_wrong_types(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text("title = 1\n"
                       "description = \"d\"\n"
                       "author = [\"a\"]\n"
                       "base_url = \"https://x\"\n"
                       "output_dir = 2\n"
                       "log_page_size = \"x\"\n"
                       "headers_file = \"yes\"\n"
                       "limits = 3\n"
                       "repo = 4\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'title' must be a string\n"
                         "config key 'author' must be a string\n"
                         "config key 'output_dir' must be a string\n"
                         "config key 'log_page_size' must be an integer\n"
                         "config key 'headers_file' must be a boolean\n"
                         "config key 'limits' must be a table\n"
                         "config key 'repo' must be an array of tables\n"));
  TEST_CHECK(site_config.author.len == 0);
  TEST_CHECK(str_eq(site_config.output_dir, S("public")));
  TEST_CHECK(site_config.is_output_dir_rejected);
  TEST_CHECK(site_config.log_page_size == 100);
  TEST_CHECK(!site_config.has_headers_file);
}

// A key outside the schema is reported at every level, with its full dotted name.
static void test_load_rejects_unknown_keys(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "colour = 1\n"
                                    "[limits]\n"
                                    "tree_len_max = 1\n"
                                    "[[repo]]\n"
                                    "name = \"one\"\n"
                                    "path = \"one\"\n"
                                    "branches = []\n"
                                    "[[repo.project]]\n"
                                    "name = \"p\"\n"
                                    "path = \"p\"\n"
                                    "release = true\n",
                          "unknown config key 'colour'\n"
                          "unknown config key 'limits.tree_len_max'\n"
                          "unknown config key 'repo[0].branches'\n"
                          "unknown config key 'repo[0].project[0].release'\n"));
}

// `base_url` needs an HTTP(S) scheme and a host, and a rejected one becomes empty. `core/url`'s
// tests cover the other URLs without a host.
static void test_load_rejects_base_url_without_host(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text("title = \"t\"\n"
                       "description = \"d\"\n"
                       "author = \"a\"\n"
                       "base_url = \"https:///path\"\n" REPO_TABLE,
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'base_url' must be an absolute 'http://' or 'https://' URL "
                         "with a host: 'https:///path'\n"));
  TEST_CHECK(site_config.base_url.len == 0);
}

// A `base_url` with a query, a fragment, a space or a control byte would break every link below it,
// so it is rejected, and the config keeps no `base_url`. `core/url`'s tests cover each byte.
static void test_load_rejects_base_url_with_path_breaking_byte(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text("title = \"t\"\n"
                       "description = \"d\"\n"
                       "author = \"a\"\n"
                       "base_url = \"https://example.org/x?y=1\"\n" REPO_TABLE,
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'base_url' must not hold '?', '#', a space or a control "
                         "character: 'https://example.org/x?y=1'\n"));
  TEST_CHECK(site_config.base_url.len == 0);
}

// A count below its minimum or above its maximum is rejected with its bound, and keeps its default.
// `BLOB_LEN_MAX_LIMIT` and `LOG_PAGE_SIZE_MAX` are file-local to `site_config.c`, so they are
// spelled out here with `SURGE_RAW_LEN_MAX_LIMIT`, and changing any of them must update this test.
static void test_load_bounds_integers(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "log_page_size = 0\n"
                                 "feed_count = -1\n"
                                 "[limits]\n"
                                 "blob_len_max = 4194305\n"
                                 "raw_len_max = 1073741825\n"
                                 "image_len_max = 4194305\n" REPO_TABLE,
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'log_page_size' must be at least 1: 0\n"
                         "config key 'feed_count' must be at least 0: -1\n"
                         "config key 'limits.blob_len_max' exceeds max blob length (4194304 "
                         "bytes) at 4194305 bytes\n"
                         "config key 'limits.raw_len_max' exceeds max raw length (1073741824 "
                         "bytes) at 1073741825 bytes\n"
                         "config key 'limits.image_len_max' exceeds max image length (4194304 "
                         "bytes) at 4194305 bytes\n"));
  TEST_CHECK(site_config.log_page_size == 100);
  TEST_CHECK(site_config.feed_count == 25);
  TEST_CHECK(site_config.limits.blob_len_max == 1024 * 1024);
  TEST_CHECK(site_config.limits.raw_len_max == 100 * 1024 * 1024);
  struct SiteConfig large;
  struct Log large_log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "log_page_size = 10001\n" REPO_TABLE, &large, &large_log) == 0);
  TEST_CHECK(test_log_is(
      &large_log, "config key 'log_page_size' exceeds max log page size (10000) at 10001\n"));
  TEST_CHECK(large.log_page_size == 100);
}

// A directory that is empty, or trims to empty, `/` included, names no directory, and keeps its
// default.
static void test_load_rejects_empty_dirs(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "templates_dir = \"\"\nstatic_dir = \"/\"\n" REPO_TABLE,
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'templates_dir' must not be empty\n"
                         "config key 'static_dir' must not be empty\n"));
  TEST_CHECK(str_eq(site_config.templates_dir, S("templates")));
  TEST_CHECK(site_config.is_templates_dir_rejected);
  TEST_CHECK(str_eq(site_config.static_dir, S("static")));
  TEST_CHECK(site_config.is_static_dir_rejected);
  // A repo's directories follow the same rule.
  struct Log repo_log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"/\"\nreleases_dir = \"//\"\n",
                       &site_config, &repo_log) == 0);
  TEST_CHECK(test_log_is(&repo_log,
                         "config key 'repo[0].path' must not be empty\n"
                         "config key 'repo[0].releases_dir' must not be empty\n"));
}

// An empty `default_branch` names no branch, so it is rejected rather than read as absent, and the
// repo keeps no default branch.
static void test_load_rejects_empty_default_branch(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\ndefault_branch = \"\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log, "config key 'repo[0].default_branch' must not be empty\n"));
  TEST_ASSERT(site_config.repo_count == 1);
  TEST_CHECK(site_config.repos[0].default_branch.data == NULL);
}

// A name with a byte outside the allowed set, a leading `.` or a `.git` suffix is rejected, and the
// repo keeps no name.
static void test_load_rejects_invalid_names(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a b\"\npath = \"a\"\n"
                                 "[[repo]]\nname = \".hidden\"\npath = \"b\"\n"
                                 "[[repo]]\nname = \"c.GIT\"\npath = \"c\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'repo[0].name' must use only letters, digits, '.', '_' and "
                         "'-', not start with '.' and not end in '.git': 'a b'\n"
                         "config key 'repo[1].name' must use only letters, digits, '.', '_' and "
                         "'-', not start with '.' and not end in '.git': '.hidden'\n"
                         "config key 'repo[2].name' must use only letters, digits, '.', '_' and "
                         "'-', not start with '.' and not end in '.git': 'c.GIT'\n"));
  TEST_ASSERT(site_config.repo_count == 3);
  TEST_CHECK(site_config.repos[0].name.len == 0);
}

// An empty name says so, as an empty path does.
static void test_load_rejects_empty_names(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"\"\npath = \"a\"\n",
                          "config key 'repo[0].name' must not be empty\n"));
}

// A name longer than the output's segment leaves for `.git` is rejected with the limit.
static void test_load_rejects_long_names(void) {
  enum { NAME_LEN = SURGE_NAME_LEN_MAX + 1 };
  char name[NAME_LEN + 1];
  memset(name, 'n', NAME_LEN);
  name[NAME_LEN] = '\0';
  struct Arena arena = test_arena();
  struct Buf text = {.arena = &arena};
  buf_printf(&text, SITE_KEYS "[[repo]]\nname = \"%s\"\npath = \"a\"\n", name);
  struct Arena expected_arena = test_arena();
  struct Buf expected = {.arena = &expected_arena};
  buf_printf(&expected,
             "config key 'repo[0].name' exceeds max name length (%d bytes) at %d bytes: '%s'\n",
             SURGE_NAME_LEN_MAX, NAME_LEN, name);
  TEST_CHECK(is_logged_as(buf_cstr(&text), buf_cstr(&expected)));
}

// A repo may not take a name the output root uses, in any case, and a project may not take a name
// that `releases/` already holds.
static void test_load_rejects_reserved_names(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"Index.HTML\"\npath = \"a\"\n"
                                    "[[repo]]\nname = \"404.html\"\npath = \"b\"\n"
                                    "[[repo]]\nname = \"_Headers\"\npath = \"f\"\n"
                                    "[[repo]]\nname = \"_SURGE\"\npath = \"c\"\n"
                                    "[[repo.project]]\nname = \"Download\"\npath = \"d\"\n"
                                    "[[repo.project]]\nname = \"tag\"\npath = \"e\"\n"
                                    "[[repo.project]]\nname = \"INDEX.html\"\npath = \"g\"\n",
                          "config key 'repo[0].name' must not be 'index.html' in any case: "
                          "'Index.HTML'\n"
                          "config key 'repo[1].name' must not be '404.html' in any case: "
                          "'404.html'\n"
                          "config key 'repo[2].name' must not be '_headers' in any case: "
                          "'_Headers'\n"
                          "config key 'repo[3].name' must not be '_surge' in any case: "
                          "'_SURGE'\n"
                          "config key 'repo[3].project[0].name' must not be 'download' in any "
                          "case: 'Download'\n"
                          "config key 'repo[3].project[1].name' must not be 'tag' in any "
                          "case: 'tag'\n"
                          "config key 'repo[3].project[2].name' must not be 'index.html' in any "
                          "case: 'INDEX.html'\n"));
}

// Each later repo of one name is reported once, against the first.
static void test_load_reports_each_colliding_repo_once(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\n"
                                    "[[repo]]\nname = \"A\"\npath = \"b\"\n"
                                    "[[repo]]\nname = \"a\"\npath = \"c\"\n",
                          "config keys 'repo[0].name' and 'repo[1].name' name repos that collide "
                          "after case folding: 'a' and 'A'\n"
                          "config keys 'repo[0].name' and 'repo[2].name' name repos that collide "
                          "after case folding: 'a' and 'a'\n"));
}

// Two repos, or two projects of one repo, whose names differ only in case refuse the load, and the
// message names both.
static void test_load_rejects_colliding_names(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"Code\"\npath = \"a\"\n"
                                    "[[repo.project]]\nname = \"lib\"\npath = \"x\"\n"
                                    "[[repo.project]]\nname = \"LIB\"\npath = \"y\"\n"
                                    "[[repo]]\nname = \"code\"\npath = \"b\"\n",
                          "config keys 'repo[0].project[0].name' and 'repo[0].project[1].name' "
                          "name projects that collide after case folding: 'lib' and 'LIB'\n"
                          "config keys 'repo[0].name' and 'repo[1].name' name repos that "
                          "collide after case folding: 'Code' and 'code'\n"));
}

// Two tag prefixes overlap when one starts the other, so a tag would belong to both projects.
static void test_load_rejects_overlapping_tag_prefixes(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\n"
                                    "[[repo.project]]\nname = \"p\"\npath = \"p\"\n"
                                    "tag_prefix = \"sosig-\"\n"
                                    "[[repo.project]]\nname = \"q\"\npath = \"q\"\n"
                                    "tag_prefix = \"sosig-v\"\n"
                                    "[[repo.project]]\nname = \"r\"\npath = \"r\"\n"
                                    "tag_prefix = \"\"\n",
                          "config key 'repo[0].project[2].tag_prefix' must not be empty\n"
                          "config keys 'repo[0].project[0].tag_prefix' and "
                          "'repo[0].project[1].tag_prefix' overlap: 'sosig-' and 'sosig-v'\n"));
}

// A repo's `mise` needs releases of its own, and a project's needs a `tag_prefix`, and either must
// be a boolean.
static void test_load_rejects_mise_without_releases(void) {
  TEST_CHECK(is_logged_as(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\nmise = true\n"
                                    "[[repo]]\nname = \"b\"\npath = \"b\"\nmise = true\n"
                                    "releases_dir = \"r\"\n"
                                    "[[repo.project]]\nname = \"p\"\npath = \"p\"\n"
                                    "tag_prefix = \"p-v\"\n"
                                    "[[repo.project]]\nname = \"q\"\npath = \"q\"\nmise = true\n"
                                    "[[repo.project]]\nname = \"s\"\npath = \"s\"\nmise = 1\n",
                          "config key 'repo[0].mise' needs 'repo[0].releases_dir'\n"
                          "config key 'repo[1].project[1].mise' needs "
                          "'repo[1].project[1].tag_prefix'\n"
                          "config key 'repo[1].project[2].mise' must be a boolean\n"
                          "config key 'repo[1].mise' must not be set in a repo whose projects "
                          "have a 'tag_prefix': set the projects' 'mise' instead\n"));
}

// A project's path must not be empty, start with `/`, or have an empty, `.` or `..` segment, and
// the project keeps no path.
static void test_load_rejects_unsafe_project_paths(void) {
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_CHECK(load_text(SITE_KEYS "[[repo]]\nname = \"a\"\npath = \"a\"\n"
                                 "[[repo.project]]\nname = \"p\"\npath = \"../p\"\n"
                                 "[[repo.project]]\nname = \"q\"\npath = \"/q\"\n"
                                 "[[repo.project]]\nname = \"r\"\npath = \"/\"\n"
                                 "[[repo.project]]\nname = \"s\"\npath = \"a//b\"\n"
                                 "[[repo.project]]\nname = \"t\"\npath = \"a/./b\"\n"
                                 "[[repo.project]]\nname = \"u\"\npath = \"a/..\"\n"
                                 "[[repo.project]]\nname = \"v\"\npath = \"\"\n",
                       &site_config, &log) == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 'repo[0].project[0].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: '../p'\n"
                         "config key 'repo[0].project[1].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: '/q'\n"
                         "config key 'repo[0].project[2].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: '/'\n"
                         "config key 'repo[0].project[3].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: 'a//b'\n"
                         "config key 'repo[0].project[4].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: 'a/./b'\n"
                         "config key 'repo[0].project[5].path' must be a relative path in the "
                         "repo without an empty, '.' or '..' segment: 'a/..'\n"
                         "config key 'repo[0].project[6].path' must not be empty\n"));
  TEST_ASSERT(site_config.repo_count == 1 && site_config.repos[0].project_count == 7);
  for (ptrdiff_t i = 0; i < 7; i++) {
    TEST_CHECK(site_config.repos[0].projects[i].path.len == 0);
  }
}

// A file that cannot be read reports the cause and the path.
static void test_load_reports_missing_file(void) {
  struct Os* os = os_memory();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Arena perm = test_arena();
  struct Log log = test_log();
  TEST_CHECK(site_config_load(&site_config, S("surge.toml"), os, &perm, test_arena(), &log) == -1);
  TEST_CHECK(
      test_log_is(&log, "failed to read config: No such file or directory ('surge.toml')\n"));
}

// The printed config is TOML that loads back to the same values, and leaves out the optionals that
// are empty and the `mise` keys that are `false`. A control byte prints as its short escape where
// TOML has one, and as `\uXXXX` otherwise.
static void test_print_round_trips(void) {
  static const char text[] = SITE_KEYS
      "headers_file = true\n"
      "[[repo]]\n"
      "name = \"apps-c17\"\n"
      "path = \"repos/apps \\\"c17\\\"\"\n"
      "description = \"C\\b\\f\\r\\u0001\"\n"
      "default_branch = \"main\"\n"
      "[[repo.project]]\n"
      "name = \"sosig\"\n"
      "path = \"apps/sosig\"\n"
      "tag_prefix = \"sosig-v\"\n"
      "mise = true\n";
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_ASSERT(load_text(text, &site_config, &log) == 0);
  struct Os* os = os_memory();
  TEST_CHECK(site_config_print(&site_config, os, test_arena()) == 0);
  static const char expected[] =
      "title = \"Code\"\n"
      "description = \"Repositories\"\n"
      "author = \"Ada\"\n"
      "base_url = \"https://git.example.org\"\n"
      "output_dir = \"public\"\n"
      "templates_dir = \"templates\"\n"
      "static_dir = \"static\"\n"
      "log_page_size = 100\n"
      "feed_count = 25\n"
      "headers_file = true\n"
      "\n"
      "[limits]\n"
      "blob_len_max = 1048576\n"
      "raw_len_max = 104857600\n"
      "diff_file_count_max = 300\n"
      "diff_line_count_max = 20000\n"
      "diff_len_max = 1048576\n"
      "image_len_max = 2097152\n"
      "pack_len_max = 26214400\n"
      "\n"
      "[[repo]]\n"
      "name = \"apps-c17\"\n"
      "path = \"repos/apps \\\"c17\\\"\"\n"
      "description = \"C\\b\\f\\r\\u0001\"\n"
      "default_branch = \"main\"\n"
      "\n"
      "[[repo.project]]\n"
      "name = \"sosig\"\n"
      "path = \"apps/sosig\"\n"
      "tag_prefix = \"sosig-v\"\n"
      "mise = true\n";
  TEST_CHECK(os_memory_is_printed(os, 1, expected));

  struct SiteConfig reloaded;
  struct Log reload_log = test_log();
  TEST_CHECK(load_text(expected, &reloaded, &reload_log) == 0);
  TEST_CHECK(test_log_is_empty(&reload_log));
  TEST_CHECK(str_eq(reloaded.author, S("Ada")));
  TEST_ASSERT(reloaded.repo_count == 1 && reloaded.repos[0].project_count == 1);
  TEST_CHECK(str_eq(reloaded.repos[0].path, site_config.repos[0].path));
  TEST_CHECK(str_eq(reloaded.repos[0].description, S("C\b\f\r\x01")));
  TEST_CHECK(str_eq(reloaded.repos[0].projects[0].tag_prefix, S("sosig-v")));
  TEST_CHECK(reloaded.repos[0].projects[0].is_mise);
  TEST_CHECK(reloaded.has_headers_file);
}

// A repo's `description` set to an empty string shows no description and hides the repository's
// `description` file, so the load marks it set and the printed config keeps it.
static void test_empty_repo_description_is_set(void) {
  static const char text[] = SITE_KEYS
      "[[repo]]\n"
      "name = \"one\"\n"
      "path = \"repos/one\"\n"
      "description = \"\"\n";
  struct SiteConfig site_config;
  struct Log log = test_log();
  TEST_ASSERT(load_text(text, &site_config, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_ASSERT(site_config.repo_count == 1);
  TEST_CHECK(site_config.repos[0].has_description && site_config.repos[0].description.len == 0);
  struct Os* os = os_memory();
  TEST_CHECK(site_config_print(&site_config, os, test_arena()) == 0);
  TEST_CHECK(os_memory_is_printed(os, 1,
                                  "title = \"Code\"\n"
                                  "description = \"Repositories\"\n"
                                  "author = \"Ada\"\n"
                                  "base_url = \"https://git.example.org\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "log_page_size = 100\n"
                                  "feed_count = 25\n"
                                  "headers_file = false\n"
                                  "\n"
                                  "[limits]\n"
                                  "blob_len_max = 1048576\n"
                                  "raw_len_max = 104857600\n"
                                  "diff_file_count_max = 300\n"
                                  "diff_line_count_max = 20000\n"
                                  "diff_len_max = 1048576\n"
                                  "image_len_max = 2097152\n"
                                  "pack_len_max = 26214400\n"
                                  "\n"
                                  "[[repo]]\n"
                                  "name = \"one\"\n"
                                  "path = \"repos/one\"\n"
                                  "description = \"\"\n"));
}

TEST_LIST = {
    {"load applies defaults", test_load_applies_defaults},
    {"load reads every key", test_load_reads_every_key},
    {"load resolves paths against config dir", test_load_resolves_paths_against_config_dir},
    {"load accepts project names across repos", test_load_accepts_project_names_across_repos},
    {"load accepts project paths of any bytes", test_load_accepts_project_paths_of_any_bytes},
    {"load reports missing required keys", test_load_reports_missing_required_keys},
    {"load requires a repo", test_load_requires_a_repo},
    {"load reports wrong types", test_load_reports_wrong_types},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects base url without host", test_load_rejects_base_url_without_host},
    {"load rejects base url with path breaking byte",
     test_load_rejects_base_url_with_path_breaking_byte},
    {"load bounds integers", test_load_bounds_integers},
    {"load rejects empty dirs", test_load_rejects_empty_dirs},
    {"load rejects empty default branch", test_load_rejects_empty_default_branch},
    {"load rejects invalid names", test_load_rejects_invalid_names},
    {"load rejects empty names", test_load_rejects_empty_names},
    {"load rejects long names", test_load_rejects_long_names},
    {"load rejects reserved names", test_load_rejects_reserved_names},
    {"load rejects colliding names", test_load_rejects_colliding_names},
    {"load reports each colliding repo once", test_load_reports_each_colliding_repo_once},
    {"load rejects overlapping tag prefixes", test_load_rejects_overlapping_tag_prefixes},
    {"load rejects mise without releases", test_load_rejects_mise_without_releases},
    {"load rejects unsafe project paths", test_load_rejects_unsafe_project_paths},
    {"load reports missing file", test_load_reports_missing_file},
    {"print round trips", test_print_round_trips},
    {"empty repo description is set", test_empty_repo_description_is_set},
    {NULL, NULL},
};
