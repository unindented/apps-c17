#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "formats/toml.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

// Every test here runs against the in-memory host, whose root is the working directory a load
// resolves the config path from, and which keeps what `site_config_print` prints.

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 8192 };

/**
 * Path `load_text` writes a config to. It lies below a directory, so a relative directory resolved
 * against the config's directory shows that directory.
 */
#define CONFIG_PATH "site/sosig.toml"

/**
 * @brief Loads a config from a host with a fresh long-lived arena and a fresh scratch arena.
 *
 * Each call reserves its own two arenas, so the loaded values stay valid for the rest of the test
 * and never overlap the scratch the load worked in.
 *
 * @param site_config Initialized config that receives the values. Must not be `NULL`.
 * @param config_path Terminated path of the config to load. Must not be `NULL`.
 * @param os          Host holding the config. Must not be `NULL`.
 * @param log         Log that receives every error the load records. Must not be `NULL`.
 * @return The result of `site_config_load`.
 */
static int load_site_config(struct SiteConfig* site_config,
                            const char* config_path,
                            struct Os* os,
                            struct Log* log) {
  struct Arena perm = test_arena();
  return site_config_load(site_config, str_from_cstr(config_path), os, &perm, test_arena(), log);
}

/**
 * @brief Writes TOML to `CONFIG_PATH` on a fresh host and loads it with `load_site_config`.
 *
 * @param site_config Initialized config that receives the values. Must not be `NULL`.
 * @param toml        Terminated TOML text of the config. Must not be `NULL`.
 * @param log         Log that receives every error the load records. Must not be `NULL`.
 * @return The result of `load_site_config`.
 */
static int load_text(struct SiteConfig* site_config, const char* toml, struct Log* log) {
  struct Os* os = os_memory();
  os_memory_write(os, CONFIG_PATH, toml);
  return load_site_config(site_config, CONFIG_PATH, os, log);
}

/**
 * @brief Reports whether `dir` is `name` resolved against the directory of `CONFIG_PATH`.
 *
 * @param dir  Directory the load produced.
 * @param name Terminated directory as the config names it, or its default. Must not be `NULL`.
 * @return `true` when `dir` is `site/<name>`.
 */
static bool is_config_dir(struct Str dir, const char* name) {
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "site/%s", name);
  TEST_MSG("expected '%s', got '%.*s'", buf_cstr(&expected), STR_FMT(dir));
  return str_eq(dir, buf_str(&expected));
}

/**
 * @brief Prints a site configuration on a fresh host.
 *
 * Records a test failure when the print fails.
 *
 * @param site_config Configuration to print. Must not be `NULL`.
 * @return The host, whose standard output holds the printed text.
 */
static struct Os* print_config(const struct SiteConfig* site_config) {
  struct Os* os = os_memory();
  TEST_CHECK(site_config_print(site_config, os, test_arena()) == 0);
  return os;
}

/**
 * @brief Loads a config with the given extra TOML and checks it fails with one diagnostic.
 *
 * @param toml_extra Terminated TOML appended to the required keys of an otherwise valid config.
 * @param expected   Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects(const char* toml_extra, const char* expected) {
  char toml[4096];
  const int n = snprintf(toml, sizeof(toml),
                         "title = \"Example Site\"\n"
                         "description = \"Example notes\"\n"
                         "author = \"Example Author\"\n"
                         "base_url = \"https://example.com\"\n"
                         "%s",
                         toml_extra);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));
}

/**
 * @brief Loads a config with the given permalink and checks it fails with one diagnostic.
 *
 * @param permalink Permalink pattern to load, embedded in an otherwise valid config.
 * @param expected  Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects_permalink(const char* permalink, const char* expected) {
  char toml_extra[512];
  const int n = snprintf(toml_extra, sizeof(toml_extra), "permalink = \"%s\"\n", permalink);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
  check_load_rejects(toml_extra, expected);
}

// Loading a file with only the required keys fills the rest from `site_config_init`'s defaults.
// Each default directory resolves against the directory of the config.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == 0);

  TEST_CHECK(str_eq(config.title, S("Example Site")));
  TEST_CHECK(str_eq(config.description, S("Example notes")));
  TEST_CHECK(str_eq(config.author, S("Example Author")));
  TEST_CHECK(str_eq(config.base_url, S("https://example.com")));
  TEST_CHECK(str_eq(config.permalink, S("/{section}/{slug}.html")));
  TEST_CHECK(is_config_dir(config.content_dir, "content"));
  TEST_CHECK(is_config_dir(config.output_dir, "public"));
  TEST_CHECK(is_config_dir(config.templates_dir, "templates"));
  TEST_CHECK(is_config_dir(config.static_dir, "static"));
  TEST_CHECK(str_eq(config.content_template, S("content.html")));
  TEST_ASSERT(config.aggregate_template_count == 1);
  TEST_CHECK(str_eq(config.aggregate_templates[0], S("index.html")));
  TEST_ASSERT(config.feed_template_count == 1);
  TEST_CHECK(str_eq(config.feed_templates[0], S("atom.xml")));
  TEST_CHECK(config.feed_count == 25);
  TEST_CHECK(config.frontmatter_ignored_key_count == 0);
}

// Loading a file that sets every optional key overrides each corresponding default.
static void test_load_overrides_optional_keys(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "permalink = \"/{slug}/index.html\"\n"
      "content_dir = \"posts\"\n"
      "output_dir = \"dist\"\n"
      "templates_dir = \"layouts\"\n"
      "static_dir = \"files\"\n"
      "content_template = \"post.html\"\n"
      "aggregate_templates = [\"index.html\", \"archive.html\"]\n"
      "feed_templates = []\n"
      "feed_count = 5\n"
      "frontmatter_ignored_keys = [\"identifier\", \"signature\"]\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == 0);

  TEST_CHECK(str_eq(config.permalink, S("/{slug}/index.html")));
  TEST_CHECK(is_config_dir(config.content_dir, "posts"));
  TEST_CHECK(is_config_dir(config.output_dir, "dist"));
  TEST_CHECK(is_config_dir(config.templates_dir, "layouts"));
  TEST_CHECK(is_config_dir(config.static_dir, "files"));
  TEST_CHECK(str_eq(config.content_template, S("post.html")));
  TEST_ASSERT(config.aggregate_template_count == 2);
  TEST_CHECK(str_eq(config.aggregate_templates[0], S("index.html")));
  TEST_CHECK(str_eq(config.aggregate_templates[1], S("archive.html")));
  TEST_CHECK(config.feed_template_count == 0);
  TEST_CHECK(config.feed_count == 5);
  TEST_ASSERT(config.frontmatter_ignored_key_count == 2);
  TEST_CHECK(str_eq(config.frontmatter_ignored_keys[0], S("identifier")));
  TEST_CHECK(str_eq(config.frontmatter_ignored_keys[1], S("signature")));
}

// Directory keys have trailing separators trimmed at load. Left in place, a trailing slash defeats
// the `<content_dir>/` prefix strip during section derivation and leaks the content directory name
// into every URL. The end-to-end consequence is asserted in the `cmd_build` tests. An absolute
// directory survives normalization unchanged, and a parent-relative one resolves as it is written.
static void test_load_normalizes_directory_keys(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "content_dir = \"content/\"\n"
      "output_dir = \"/srv/www///\"\n"
      "templates_dir = \"../shared/templates/\"\n"
      "static_dir = \"files/\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(is_config_dir(config.content_dir, "content"));
  TEST_CHECK(str_eq(config.output_dir, S("/srv/www")));
  TEST_CHECK(is_config_dir(config.templates_dir, "../shared/templates"));
  TEST_CHECK(is_config_dir(config.static_dir, "files"));
}

// A relative directory, given or default, resolves against the config's directory, and an absolute
// one stays as it is, so `-c site/sosig.toml` reads and writes next to the config.
static void test_load_resolves_directories_against_config_dir(void) {
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config,
                       "title = \"Example Site\"\n"
                       "description = \"Example notes\"\n"
                       "author = \"Example Author\"\n"
                       "base_url = \"https://example.com\"\n"
                       "content_dir = \"../posts\"\n"
                       "templates_dir = \"layouts/\"\n"
                       "static_dir = \"/srv/static\"\n",
                       &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(config.content_dir, S("site/../posts")));
  TEST_CHECK(str_eq(config.output_dir, S("site/public")));
  TEST_CHECK(str_eq(config.templates_dir, S("site/layouts")));
  TEST_CHECK(str_eq(config.static_dir, S("/srv/static")));
}

// An absolute `base_url` keeps its scheme, host, port and path, and loses only a trailing `/`,
// which templates would otherwise double when joining it with a path that starts with `/`.
static void test_load_normalizes_base_url(void) {
  static const char* const cases[][2] = {
      {"https://example.com", "https://example.com"},
      {"https://example.com/", "https://example.com"},
      {"https://example.com///", "https://example.com"},
      {"http://example.com/blog/", "http://example.com/blog"},
      {"https://example.com:8443/blog", "https://example.com:8443/blog"},
      {"HTTPS://Example.COM", "HTTPS://Example.COM"},  // the scheme matches case-insensitively
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Site\"\n"
                           "description = \"Example notes\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(config.base_url, str_from_cstr(cases[i][1])));
  }
}

// Overlapping directories load, because comparing path text cannot tell whether two directories
// overlap. The build compares them by identity before it walks or writes anything.
static void test_load_leaves_directory_overlap_to_the_build(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "content_dir = \"posts\"\n"
      "output_dir = \"posts/generated\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(is_config_dir(config.content_dir, "posts"));
  TEST_CHECK(is_config_dir(config.output_dir, "posts/generated"));
}

// Permalink patterns that expand to a safe relative path are accepted, including a directory-style
// pattern and one that omits the section.
static void test_load_accepts_valid_permalinks(void) {
  static const char* const permalinks_valid[] = {
      "/{section}/{slug}.html", "/{slug}.html", "/{section}/{slug}/",
      "/posts/{slug}.html",     "{slug}",       "/a.b-c_d/{slug}.html",
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(permalinks_valid) / sizeof(permalinks_valid[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Site\"\n"
                           "description = \"Example notes\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"https://example.com\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_valid[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(config.permalink, str_from_cstr(permalinks_valid[i])));
  }
}

// Zero is accepted, which is the boundary below every other accepting case in this file. A guard of
// `< 1` on `feed_count` would reject a legal config while still passing all of them. Zero means a
// feed template sees no entries, which is a configuration a user can ask for.
static void test_load_accepts_zero_feed_count(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "feed_count = 0\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(config.feed_count == 0);
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&config, CONFIG_PATH, os_memory(), &log) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('%s')",
               error_system_message(reason, sizeof(reason), ENOENT), CONFIG_PATH);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(test_log_is_one(&log, expected));
}

// A config larger than `TOML_CONFIG_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The host stores only the size of the extension, so the fixture costs no
// memory.
static void test_load_rejects_oversize_file(void) {
  const ptrdiff_t config_len = (ptrdiff_t)TOML_CONFIG_LEN_MAX + 1;
  struct Os* os = os_memory();
  os_memory_write(os, CONFIG_PATH, "title = \"Site\"\n");
  os_memory_truncate(os, CONFIG_PATH, config_len);
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected),
               "failed to read config: exceeds max file size (%d bytes) at %td bytes ('%s')",
               TOML_CONFIG_LEN_MAX, config_len, CONFIG_PATH);
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));

  TEST_CHECK(load_site_config(&config, CONFIG_PATH, os, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));
}

// A config file holding an embedded `NUL` is rejected at the read, before the parse, naming the
// config path. `os_read` rejects it for every caller, because the byte has no meaning in any format
// `sosig` reads or writes.
static void test_load_rejects_nul_in_file(void) {
  static const char config_data[] = "title = \"Site\"\0author = \"Example Author\"\n";
  struct Os* os = os_memory();
  os_memory_write_bytes(os, S(CONFIG_PATH),
                        (struct Str){config_data, (ptrdiff_t)sizeof(config_data) - 1});
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();

  TEST_CHECK(load_site_config(&config, CONFIG_PATH, os, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log, "failed to read config: contains an embedded NUL byte ('" CONFIG_PATH "')"));
}

// `site_config_load` reports a syntactically malformed config as a parse failure naming the config
// path, rather than reaching the field pass with an empty table and reporting a false missing-key
// error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "base_url = \n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  // The prefix and the trailing attribution are this module's own contribution. The text between
  // them is the vendored parser's `errmsg`, left unpinned so rewording it upstream does not fail
  // this test. The two together are still the whole first-party claim, which neither the
  // missing-key message nor the read-failure message could satisfy.
  const struct Str prefix = S("failed to parse config: ");
  const struct Str message = buf_str(&log.buf);
  TEST_CHECK(log.errors == 1);
  TEST_CHECK(str_startswith(message, prefix));
  const struct Str suffix = S(" (in '" CONFIG_PATH "')\n");
  // A cause of at least one byte has to sit between the two, or the parser reported nothing.
  TEST_CHECK(message.len > prefix.len + suffix.len);
  TEST_CHECK(str_endswith(message, suffix));
}

// The load stops at its first error. The unknown key is checked first, so the other mistakes of
// this config go unreported.
static void test_load_stops_at_first_invalid_key(void) {
  const char toml[] =
      "title = 1\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"ftp://example.com\"\n"
      "colour = \"blue\"\n"
      "content_dir = \"/\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is(&log, "unknown config key 'colour'\n"));
}

/**
 * The required keys, each with a valid line, in the order the config lists them. A test builds a
 * config from every line but one, or with one line replaced.
 */
static const char* const REQUIRED_KEY_LINES[][2] = {
    {"title", "title = \"Example Site\"\n"},
    {"description", "description = \"Example notes\"\n"},
    {"author", "author = \"Example Author\"\n"},
    {"base_url", "base_url = \"https://example.com\"\n"},
};

enum { REQUIRED_KEY_COUNT = sizeof(REQUIRED_KEY_LINES) / sizeof(REQUIRED_KEY_LINES[0]) };

/**
 * @brief Writes the required keys, with the one at `index` replaced, into a terminated buffer.
 *
 * @param index        Index in `REQUIRED_KEY_LINES` of the key to replace.
 * @param replacement  Terminated line written in place of that key, or `""` to leave it out.
 * @param toml_out     Buffer that receives the terminated TOML.
 * @param toml_out_len Size of `toml_out` in bytes. Must be non-zero.
 */
static void format_required_keys(ptrdiff_t index,
                                 const char* replacement,
                                 char* toml_out,
                                 ptrdiff_t toml_out_len) {
  ptrdiff_t len = 0;
  toml_out[0] = '\0';
  for (ptrdiff_t i = 0; i < REQUIRED_KEY_COUNT; i++) {
    const char* line = i == index ? replacement : REQUIRED_KEY_LINES[i][1];
    const int n = snprintf(toml_out + len, (size_t)(toml_out_len - len), "%s", line);
    TEST_ASSERT(n >= 0 && n < toml_out_len - len);
    len += n;
  }
}

// Omitting any one required key is reported as missing rather than as a type error, naming that key
// alone.
static void test_load_rejects_missing_required_key(void) {
  for (ptrdiff_t i = 0; i < REQUIRED_KEY_COUNT; i++) {
    char toml[512];
    format_required_keys(i, "", toml, sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == -1);
    char expected[EXPECTED_SIZE];
    (void)snprintf(expected, sizeof(expected), "missing required config key '%s'",
                   REQUIRED_KEY_LINES[i][0]);
    TEST_CHECK(test_log_is_one(&log, expected));
    TEST_MSG("key '%s'", REQUIRED_KEY_LINES[i][0]);
  }
}

// Any one required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  for (ptrdiff_t i = 0; i < REQUIRED_KEY_COUNT; i++) {
    char line[64];
    (void)snprintf(line, sizeof(line), "%s = 123\n", REQUIRED_KEY_LINES[i][0]);
    char toml[512];
    format_required_keys(i, line, toml, sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == -1);
    char expected[EXPECTED_SIZE];
    (void)snprintf(expected, sizeof(expected), "config key '%s' must be a string",
                   REQUIRED_KEY_LINES[i][0]);
    TEST_CHECK(test_log_is_one(&log, expected));
    TEST_MSG("key '%s'", REQUIRED_KEY_LINES[i][0]);
  }
}

// A key the schema does not define is rejected, naming the key, so a misspelling cannot silently
// keep a default. A key that is a prefix of a known key, or extends one, is as unknown as any
// other, and so is a table the schema has no place for.
static void test_load_rejects_unknown_keys(void) {
  static const char* const cases[][2] = {
      {"title = \"Example Site\"\ndescription = \"Example notes\"\n"
       "author = \"Example Author\"\nbase_url = \"https://example.com\"\nouput_dir = \"site\"\n",
       "unknown config key 'ouput_dir'"},
      {"titl = \"Example Site\"\ntitle = \"Example Site\"\ndescription = \"Example notes\"\n"
       "author = \"Example Author\"\nbase_url = \"https://example.com\"\n",
       "unknown config key 'titl'"},
      {"title = \"Example Site\"\ndescription = \"Example notes\"\n"
       "author = \"Example Author\"\nbase_url = \"https://example.com\"\n"
       "titles = \"Example Site\"\n",
       "unknown config key 'titles'"},
      {"title = \"Example Site\"\ndescription = \"Example notes\"\n"
       "author = \"Example Author\"\nbase_url = \"https://example.com\"\n"
       "descriptions = \"Example notes\"\n",
       "unknown config key 'descriptions'"},
      {"title = \"Example Site\"\ndescription = \"Example notes\"\n"
       "author = \"Example Author\"\nbase_url = \"https://example.com\"\n[extra]\nkey = 1\n",
       "unknown config key 'extra'"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
  }
}

// No key in the schema holds a table, so a table under a known key fails that key's type check.
static void test_load_rejects_table_values(void) {
  static const char* const cases[][2] = {
      {"description = \"Example notes\"\nauthor = \"Example Author\"\n"
       "base_url = \"https://example.com\"\n[title]\nname = \"Example Site\"\n",
       "config key 'title' must be a string"},
      {"title = \"Example Site\"\nauthor = \"Example Author\"\n"
       "base_url = \"https://example.com\"\n[description]\ntext = \"Example notes\"\n",
       "config key 'description' must be a string"},
      {"title = \"Example Site\"\ndescription = \"Example notes\"\nauthor = \"Example Author\"\n"
       "base_url = \"https://example.com\"\n[aggregate_templates]\nname = \"index.html\"\n",
       "config key 'aggregate_templates' must be an array"},
      {"title = \"Example Site\"\ndescription = \"Example notes\"\nauthor = \"Example Author\"\n"
       "base_url = \"https://example.com\"\n[feed_count]\ncount = 10\n",
       "config key 'feed_count' must be an integer"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
  }
}

// An integer key rejects a TOML type other than integer and enforces its exact lower bound, which
// is 0 for `feed_count`. A non-integer fails a different assertion from an out-of-range one: the
// user has to change the type, not the number.
static void test_load_rejects_invalid_integer_type_and_minimum(void) {
  check_load_rejects("feed_count = \"ten\"\n", "config key 'feed_count' must be an integer");
  check_load_rejects("feed_count = -1\n", "config key 'feed_count' must be at least 0: -1");
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from `os_read`: the
// file itself holds no `NUL`. The byte has no meaning in any format `sosig` reads or writes, so the
// load refuses it.
static void test_load_rejects_nul_in_string_values(void) {
  static const char* const cases[][2] = {
      {"title = \"a\\u0000b\"\ndescription = \"D\"\nauthor = \"A\"\n"
       "base_url = \"https://example.com\"\n",
       "config key 'title' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"a\\u0000b\"\nauthor = \"A\"\n"
       "base_url = \"https://example.com\"\n",
       "config key 'description' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\nbase_url = \"a\\u0000b\"\n",
       "config key 'base_url' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\n"
       "base_url = \"https://example.com\"\ncontent_dir = \"a\\u0000b\"\n",
       "config key 'content_dir' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\n"
       "base_url = \"https://example.com\"\naggregate_templates = [\"a\\u0000b\"]\n",
       "config key 'aggregate_templates' must not contain a NUL byte"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
  }
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"content_dir = \"\"\n", "config key 'content_dir' must not be empty"},
      {"output_dir = \"/\"\n", "config key 'output_dir' must not be empty"},
      {"templates_dir = \"///\"\n", "config key 'templates_dir' must not be empty"},
      {"static_dir = \"\"\n", "config key 'static_dir' must not be empty"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    check_load_rejects(cases[i][0], cases[i][1]);
  }
}

// `base_url` needs an HTTP(S) scheme and a host. `core/url`'s tests cover the other URLs without a
// host.
static void test_load_rejects_base_url_without_host(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https:///path\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log,
      "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a "
      "host: 'https:///path'"));
}

// `base_url` is required, so a present but empty `base_url` is rejected as a URL without a host,
// where fram, whose `base_url` is optional, reads it as absent.
static void test_load_rejects_empty_base_url(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log,
      "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a host: ''"));
}

// A `base_url` with a query, a fragment, a space or a control byte would break every link below it,
// so it is rejected. `core/url`'s tests cover each byte.
static void test_load_rejects_base_url_with_path_breaking_byte(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com/x?y=1\"\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(&log,
                             "config key 'base_url' must not hold '?', '#', a space or a control "
                             "character: 'https://example.com/x?y=1'"));
}

// An unsafe `content_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per content entry that has no
// frontmatter `template` of its own. Parent-relative, absolute and empty names are all unsafe.
static void test_load_rejects_unsafe_content_template(void) {
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(unsafe_names) / sizeof(unsafe_names[0])); i++) {
    char toml_extra[256];
    const int n =
        snprintf(toml_extra, sizeof(toml_extra), "content_template = \"%s\"\n", unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'content_template' must be a safe relative template name: '%s'",
                 unsafe_names[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    check_load_rejects(toml_extra, expected);
  }
}

// A non-array value, a non-string element, and an unsafe name for a template array key are each
// rejected.
static void test_load_rejects_invalid_template_arrays(void) {
  check_load_rejects("aggregate_templates = \"index.html\"\n",
                     "config key 'aggregate_templates' must be an array");
  check_load_rejects("aggregate_templates = [1]\n",
                     "config key 'aggregate_templates' must contain only strings");
  check_load_rejects("aggregate_templates = [\"../evil.html\"]\n",
                     "config key 'aggregate_templates' must contain only safe relative template "
                     "names: '../evil.html'");
}

// A non-array value and a non-string element of `frontmatter_ignored_keys` are each rejected. Any
// string is a key, so no string is unsafe.
static void test_load_rejects_invalid_frontmatter_ignored_keys(void) {
  check_load_rejects("frontmatter_ignored_keys = \"identifier\"\n",
                     "config key 'frontmatter_ignored_keys' must be an array");
  check_load_rejects("frontmatter_ignored_keys = [\"identifier\", 1]\n",
                     "config key 'frontmatter_ignored_keys' must contain only strings");
}

// Every element of a template array is checked even after one fails, so each unsafe name is
// reported in element order. A wrong type or a `NUL` byte is reported once however many elements
// have it, since its message names no element.
static void test_load_reports_every_invalid_template_name(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "feed_templates = [\"../a.xml\", 1, \"a\\u0000b\", \"ok.xml\", \"/b.xml\", 2, "
      "\"c\\u0000d\"]\n";
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&config, toml, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "config key 'feed_templates' must contain only safe relative template "
                         "names: '../a.xml'\n"
                         "config key 'feed_templates' must contain only strings\n"
                         "config key 'feed_templates' must not contain a NUL byte\n"
                         "config key 'feed_templates' must contain only safe relative template "
                         "names: '/b.xml'\n"));
  TEST_CHECK(log.errors == 4);
}

// A permalink that could escape the output directory, or that uses bytes the expanded path cannot
// contain, is rejected once at load rather than once per content entry during the render.
static void test_load_rejects_unsafe_permalink(void) {
  static const char* const permalinks_unsafe[] = {
      "/../{slug}.html",     // a `..` segment escapes `output_dir`
      "/./{slug}.html",      // a `.` segment
      "{section}/..",        // a trailing `..` segment
      "/{slug}?draft.html",  // a byte a safe relative path cannot contain
      "/{unknown}/{slug}",   // an unrecognized token stays literal and cannot be safe
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(permalinks_unsafe) / sizeof(permalinks_unsafe[0]));
       i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Site\"\n"
                           "description = \"Example notes\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"https://example.com\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_unsafe[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a safe relative path using only "
                 "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                 "with no empty, '.' or '..' path segment: '%s'",
                 permalinks_unsafe[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));
  }
}

// A permalink that expands to the same path for every content entry is rejected at load, naming the
// token that would make it vary. Left through, two or more entries collide at the manifest with a
// message describing the symptom. A single entry publishes a wrong site with no diagnostic.
static void test_load_rejects_permalink_without_slug(void) {
  static const char* const permalinks_indistinct[] = {
      "/about.html",            // a fixed path, the same for every entry
      "",                       // expands to `/index.html`
      "/{section}/",            // varies by section, but not within one
      "/{section}/index.html",  // the same, spelled out
  };

  for (ptrdiff_t i = 0;
       i < (ptrdiff_t)(sizeof(permalinks_indistinct) / sizeof(permalinks_indistinct[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Site\"\n"
                           "description = \"Example notes\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"https://example.com\"\n"
                           "permalink = \"%s\"\n",
                           permalinks_indistinct[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
    struct SiteConfig config;
    site_config_init(&config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&config, toml, &log) == -1);
    char expected[EXPECTED_SIZE];
    const int expected_len =
        snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a distinct path per content entry; "
                 "include the '{slug}' token: '%s'",
                 permalinks_indistinct[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    TEST_CHECK(test_log_is_one(&log, expected));
  }
}

// A permalink that is unsafe only with an empty `{section}` reports the unsafe expansion ahead of
// the slug collision the populated section shows. The populated section is judged first, so a check
// that ran per expansion reported the collision.
static void test_load_prefers_safety_over_distinctness(void) {
  // With an empty section, `{section}.` expands to a `.` segment, which is unsafe. With the
  // populated sample it expands to `section.`, which is safe, and the same path for every slug.
  const char* pattern = "/{section}./index.html";
  char expected[EXPECTED_SIZE];
  (void)snprintf(expected, sizeof(expected),
                 "config key 'permalink' must expand to a safe relative path using only "
                 "letters, digits, '_', '-', '.', '/' and the '{slug}'/'{section}' tokens, "
                 "with no empty, '.' or '..' path segment: '%s'",
                 pattern);
  check_load_rejects_permalink(pattern, expected);
}

// Printing emits every key, with the defaults `site_config_init` supplies for the optional ones.
// `title`, `description`, `author` and `base_url` have no default and are set here so the output
// shows each.
static void test_print_defaults(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.title = S("Example Site");
  config.description = S("Example notes");
  config.author = S("Example Author");
  config.base_url = S("https://example.com");

  // The whole output, not a set of per-line searches: only an exact comparison can catch a key that
  // is missing, duplicated, extra, or emitted in the wrong order.
  TEST_CHECK(os_memory_is_printed(print_config(&config), 1,
                                  "title = \"Example Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Example Author\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"content\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = [\"index.html\"]\n"
                                  "feed_templates = [\"atom.xml\"]\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.title = S("Quote \" and \\ backslash");
  config.description = S("Example notes");
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  config.author = S("a\tA\bB\nC\fD\rE\x01Z\x7FG");
  config.base_url = S("https://example.com");

  // Compared whole, so an escape leaking into a neighboring field cannot hide.
  TEST_CHECK(os_memory_is_printed(print_config(&config), 1,
                                  "title = \"Quote \\\" and \\\\ backslash\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"a\\tA\\bB\\nC\\fD\\rE\\u0001Z\\u007FG\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"content\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = [\"index.html\"]\n"
                                  "feed_templates = [\"atom.xml\"]\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
}

// Printing a config and loading the printed output back from the working directory yields the same
// field values, exercising the round-trip invariant documented on `site_config_print` across
// escaped strings (including a `U+007F` byte), a non-default permalink, relative directories, and
// multi-element string arrays.
static void test_print_load_round_trips(void) {
  static const struct Str aggregates[] = {STR_INIT("index.html"), STR_INIT("archive.html")};
  static const struct Str feeds[] = {STR_INIT("atom.xml")};
  static const struct Str ignored_keys[] = {STR_INIT("identifier"), STR_INIT("signature")};

  struct SiteConfig config;
  site_config_init(&config);
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  config.title = S("Quote \" back\\slash\nnewline\ttab\x7F!");
  config.description = S("Notes on \"C17\"\n");
  config.author = S("Example Author");
  config.base_url = S("https://example.com/base");
  config.permalink = S("/{slug}/index.html");
  config.content_dir = S("posts");
  config.output_dir = S("dist");
  config.templates_dir = S("layouts");
  config.static_dir = S("files");
  config.content_template = S("post.html");
  config.aggregate_templates = aggregates;
  config.aggregate_template_count = 2;
  config.feed_templates = feeds;
  config.feed_template_count = 1;
  config.feed_count = 7;
  config.frontmatter_ignored_keys = ignored_keys;
  config.frontmatter_ignored_key_count = 2;

  struct Os* os = os_memory();
  os_memory_write_bytes(os, S("sosig.toml"), os_memory_printed(print_config(&config), 1));

  struct SiteConfig reloaded;
  site_config_init(&reloaded);
  struct Log log = test_log();
  // A config in the working directory resolves a relative directory to itself.
  TEST_CHECK(load_site_config(&reloaded, "sosig.toml", os, &log) == 0);

  TEST_CHECK(str_eq(reloaded.title, config.title));
  TEST_CHECK(str_eq(reloaded.description, config.description));
  TEST_CHECK(str_eq(reloaded.author, config.author));
  TEST_CHECK(str_eq(reloaded.base_url, config.base_url));
  TEST_CHECK(str_eq(reloaded.permalink, config.permalink));
  TEST_CHECK(str_eq(reloaded.content_dir, config.content_dir));
  TEST_CHECK(str_eq(reloaded.output_dir, config.output_dir));
  TEST_CHECK(str_eq(reloaded.templates_dir, config.templates_dir));
  TEST_CHECK(str_eq(reloaded.static_dir, config.static_dir));
  TEST_CHECK(str_eq(reloaded.content_template, config.content_template));
  TEST_ASSERT(reloaded.aggregate_template_count == 2);
  TEST_CHECK(str_eq(reloaded.aggregate_templates[0], S("index.html")));
  TEST_CHECK(str_eq(reloaded.aggregate_templates[1], S("archive.html")));
  TEST_ASSERT(reloaded.feed_template_count == 1);
  TEST_CHECK(str_eq(reloaded.feed_templates[0], S("atom.xml")));
  TEST_CHECK(reloaded.feed_count == config.feed_count);
  TEST_ASSERT(reloaded.frontmatter_ignored_key_count == 2);
  TEST_CHECK(str_eq(reloaded.frontmatter_ignored_keys[0], S("identifier")));
  TEST_CHECK(str_eq(reloaded.frontmatter_ignored_keys[1], S("signature")));
}

// An empty template array survives a load followed by a print. Neither operation alone tests this
// sequence. The load stores `[]` as a pointer that is not `NULL`, as it stores every array it
// copies, so a consumer never has to tell the two apart.
static void test_print_empty_template_arrays(void) {
  const char toml[] =
      "title = \"Example Site\"\n"
      "description = \"Example notes\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "aggregate_templates = []\n"
      "feed_templates = []\n";
  struct Os* os = os_memory();
  os_memory_write(os, "sosig.toml", toml);
  struct SiteConfig config;
  site_config_init(&config);
  struct Log log = test_log();
  // Loaded from the working directory, so the directories print as the config names them.
  TEST_CHECK(load_site_config(&config, "sosig.toml", os, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_ASSERT(config.aggregate_template_count == 0);
  TEST_ASSERT(config.feed_template_count == 0);
  // The empty array is a distinct pointer that is not `NULL`.
  TEST_CHECK(config.aggregate_templates != NULL);
  TEST_CHECK(config.feed_templates != NULL);

  // Compared whole: an empty array must print as `[]` and must not disturb its neighbors.
  TEST_CHECK(os_memory_is_printed(print_config(&config), 1,
                                  "title = \"Example Site\"\n"
                                  "description = \"Example notes\"\n"
                                  "author = \"Example Author\"\n"
                                  "base_url = \"https://example.com\"\n"
                                  "permalink = \"/{section}/{slug}.html\"\n"
                                  "content_dir = \"content\"\n"
                                  "output_dir = \"public\"\n"
                                  "templates_dir = \"templates\"\n"
                                  "static_dir = \"static\"\n"
                                  "content_template = \"content.html\"\n"
                                  "aggregate_templates = []\n"
                                  "feed_templates = []\n"
                                  "feed_count = 25\n"
                                  "frontmatter_ignored_keys = []\n"));
}

// A standard output that refuses the write makes `site_config_print` report `-1` with the write's
// own `errno`, which is what lets the `config` command name a reason instead of reporting success
// over lost output. The host fails the write as a read-only descriptor does, with `EBADF`. The same
// shape is asserted for `cli_print_version` by `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct SiteConfig config;
  site_config_init(&config);
  config.title = S("Example Site");
  config.description = S("Example notes");
  config.author = S("Example Author");
  config.base_url = S("https://example.com");

  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  errno = 0;
  TEST_CHECK(site_config_print(&config, os, test_arena()) == -1);
  TEST_CHECK(errno == EBADF);
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load resolves directories against config dir",
     test_load_resolves_directories_against_config_dir},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load leaves directory overlap to the build", test_load_leaves_directory_overlap_to_the_build},
    {"load accepts valid permalinks", test_load_accepts_valid_permalinks},
    {"load accepts zero feed count", test_load_accepts_zero_feed_count},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects nul in file", test_load_rejects_nul_in_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
    {"load stops at first invalid key", test_load_stops_at_first_invalid_key},
    {"load rejects missing required key", test_load_rejects_missing_required_key},
    {"load rejects wrong key type", test_load_rejects_wrong_key_type},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects table values", test_load_rejects_table_values},
    {"load rejects invalid integer type and minimum",
     test_load_rejects_invalid_integer_type_and_minimum},
    {"load rejects nul in string values", test_load_rejects_nul_in_string_values},
    {"load rejects empty directory keys", test_load_rejects_empty_directory_keys},
    {"load rejects base url without host", test_load_rejects_base_url_without_host},
    {"load rejects empty base url", test_load_rejects_empty_base_url},
    {"load rejects base url with path breaking byte",
     test_load_rejects_base_url_with_path_breaking_byte},
    {"load rejects unsafe content template", test_load_rejects_unsafe_content_template},
    {"load rejects invalid template arrays", test_load_rejects_invalid_template_arrays},
    {"load rejects invalid frontmatter ignored keys",
     test_load_rejects_invalid_frontmatter_ignored_keys},
    {"load reports every invalid template name", test_load_reports_every_invalid_template_name},
    {"load rejects unsafe permalink", test_load_rejects_unsafe_permalink},
    {"load rejects permalink without slug", test_load_rejects_permalink_without_slug},
    {"load prefers safety over distinctness", test_load_prefers_safety_over_distinctness},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template arrays", test_print_empty_template_arrays},
    {"print reports write failure", test_print_reports_write_failure},
    {NULL, NULL},
};
