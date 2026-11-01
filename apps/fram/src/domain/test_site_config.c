#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/error.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "domain/site_config.h"
#include "formats/toml.h"
#include "media/image.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/**
 * Size of a buffer for an expected diagnostic. The log never truncates, so this only has to hold
 * the longest message a test spells out.
 */
enum { EXPECTED_SIZE = 8192 };

/**
 * Path of the config that `load_text` writes. It lies in a directory of its own, so a relative
 * directory the config names resolves to a path below `CONFIG_DIR`, unlike one resolved against the
 * working directory.
 */
#define CONFIG_PATH "site/fram.toml"

/** Directory that holds `CONFIG_PATH`, which its relative directories resolve against. */
#define CONFIG_DIR "site"

/**
 * @brief Loads a config from a host with a fresh long-lived arena and a fresh scratch arena.
 *
 * Each call reserves its own two arenas, so the loaded values stay valid for the rest of the test
 * and never overlap the scratch the load worked in.
 *
 * @param site_config Initialized config that receives the values. Must not be `NULL`.
 * @param config_path Terminated path of the config to load. Must not be `NULL`.
 * @param os          Host that holds the config. Must not be `NULL`.
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
 * @brief Writes TOML to `CONFIG_PATH` on a fresh host and loads it, as `load_site_config` does.
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
 * @brief Prints a gallery configuration on a fresh host.
 *
 * @param site_config Configuration to print. Must not be `NULL`.
 * @return The host, whose standard output holds what `site_config_print` wrote.
 */
static struct Os* print_config(const struct SiteConfig* site_config) {
  struct Os* os = os_memory();
  TEST_CHECK(site_config_print(site_config, os, test_arena()) == 0);
  return os;
}

/**
 * @brief Reports whether a loaded directory is a relative path resolved against `CONFIG_DIR`, which
 *        holds the config.
 *
 * @param dir           Directory the load produced.
 * @param relative_path Terminated directory as the config, or its default, gives it. Must not be
 *                      `NULL`.
 * @return `true` when `dir` is `relative_path` joined below `CONFIG_DIR`, `false` otherwise.
 */
static bool is_resolved(struct Str dir, const char* relative_path) {
  struct Arena arena = test_arena();
  const struct Str expected = path_join(S(CONFIG_DIR), str_from_cstr(relative_path), &arena);
  return str_eq(dir, expected);
}

/**
 * @brief Loads a config and checks it fails with one diagnostic.
 *
 * @param toml     Terminated TOML of the whole config.
 * @param expected Exact diagnostic `site_config_load` must report.
 */
static void check_load_rejects_whole(const char* toml, const char* expected) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(&log, expected));
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
                         "title = \"Example Gallery\"\n"
                         "description = \"Example Description\"\n"
                         "author = \"Example Author\"\n"
                         "%s",
                         toml_extra);
  TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));
  check_load_rejects_whole(toml, expected);
}

// Loading a file with only the required keys fills the rest from `site_config_init`'s defaults,
// each directory resolved against the config's directory.
static void test_load_applies_required_and_defaults(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);

  TEST_CHECK(str_eq(site_config.title, S("Example Gallery")));
  TEST_CHECK(str_eq(site_config.description, S("Example Description")));
  TEST_CHECK(str_eq(site_config.author, S("Example Author")));
  TEST_CHECK(str_eq(site_config.base_url, S("")));
  TEST_CHECK(is_resolved(site_config.media_dir, "media"));
  TEST_CHECK(is_resolved(site_config.output_dir, "public"));
  TEST_CHECK(is_resolved(site_config.templates_dir, "templates"));
  TEST_CHECK(is_resolved(site_config.static_dir, "static"));
  TEST_CHECK(str_eq(site_config.album_template, S("album.html")));
  TEST_CHECK(site_config.aggregate_templates != NULL);
  TEST_CHECK(site_config.aggregate_template_count == 0);
  TEST_ASSERT(site_config.derivative_count == 3);
  TEST_CHECK(str_eq(site_config.derivatives[0].name, S("s")));
  TEST_CHECK(site_config.derivatives[0].width_px == 120);
  TEST_CHECK(site_config.derivatives[0].height_px == 120);
  TEST_CHECK(site_config.derivatives[0].quality == 70);
  TEST_CHECK(site_config.derivatives[0].is_crop);
  TEST_CHECK(str_eq(site_config.derivatives[1].name, S("m")));
  TEST_CHECK(site_config.derivatives[1].width_px == 520);
  TEST_CHECK(site_config.derivatives[1].height_px == 360);
  TEST_CHECK(site_config.derivatives[1].quality == 80);
  TEST_CHECK(site_config.derivatives[1].is_crop);
  TEST_CHECK(str_eq(site_config.derivatives[2].name, S("l")));
  TEST_CHECK(site_config.derivatives[2].width_px == 792);
  TEST_CHECK(site_config.derivatives[2].height_px == 594);
  TEST_CHECK(site_config.derivatives[2].quality == 85);
  TEST_CHECK(!site_config.derivatives[2].is_crop);
  TEST_CHECK(site_config.album_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(site_config.media_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(site_config.video_frame_seconds == 1);
}

// Loading a file that sets every optional key overrides each corresponding default.
static void test_load_overrides_optional_keys(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com\"\n"
      "media_dir = \"photos\"\n"
      "output_dir = \"dist\"\n"
      "templates_dir = \"layouts\"\n"
      "static_dir = \"files\"\n"
      "album_template = \"pages/album.html\"\n"
      "aggregate_templates = [\"sitemap.xml\", \"about/index.html\"]\n"
      "derivatives = { xs = { width = 200, height = 150, quality = 61, crop = true }, "
      "xl = { width = 1200, height = 900, quality = 93 } }\n"
      "sort = { albums = \"desc\", media = \"desc\" }\n"
      "video_frame_seconds = 17\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);

  TEST_CHECK(str_eq(site_config.base_url, S("https://example.com")));
  TEST_CHECK(is_resolved(site_config.media_dir, "photos"));
  TEST_CHECK(is_resolved(site_config.output_dir, "dist"));
  TEST_CHECK(is_resolved(site_config.templates_dir, "layouts"));
  TEST_CHECK(is_resolved(site_config.static_dir, "files"));
  TEST_CHECK(str_eq(site_config.album_template, S("pages/album.html")));
  TEST_ASSERT(site_config.aggregate_template_count == 2);
  TEST_CHECK(str_eq(site_config.aggregate_templates[0], S("sitemap.xml")));
  TEST_CHECK(str_eq(site_config.aggregate_templates[1], S("about/index.html")));
  TEST_ASSERT(site_config.derivative_count == 2);
  TEST_CHECK(str_eq(site_config.derivatives[0].name, S("xs")));
  TEST_CHECK(site_config.derivatives[0].width_px == 200);
  TEST_CHECK(site_config.derivatives[0].height_px == 150);
  TEST_CHECK(site_config.derivatives[0].quality == 61);
  TEST_CHECK(site_config.derivatives[0].is_crop);
  TEST_CHECK(str_eq(site_config.derivatives[1].name, S("xl")));
  TEST_CHECK(site_config.derivatives[1].width_px == 1200);
  TEST_CHECK(site_config.derivatives[1].height_px == 900);
  TEST_CHECK(site_config.derivatives[1].quality == 93);
  TEST_CHECK(!site_config.derivatives[1].is_crop);
  TEST_CHECK(site_config.album_sort_order == GALLERY_SORT_DESC);
  TEST_CHECK(site_config.media_sort_order == GALLERY_SORT_DESC);
  TEST_CHECK(site_config.video_frame_seconds == 17);
}

// Directory keys have trailing separators trimmed at load. Left in place, a trailing slash defeats
// the `<media_dir>/` prefix strip that places each media item in its album. The end-to-end
// consequence is asserted in the `cmd_build` tests. An absolute directory survives normalization
// unchanged, and a relative one, parent-relative included, resolves against the config's directory.
static void test_load_normalizes_directory_keys(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "media_dir = \"media/\"\n"
      "output_dir = \"/srv/www///\"\n"
      "templates_dir = \"../shared/templates/\"\n"
      "static_dir = \"files/\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(is_resolved(site_config.media_dir, "media"));
  TEST_CHECK(str_eq(site_config.output_dir, S("/srv/www")));
  TEST_CHECK(is_resolved(site_config.templates_dir, "../shared/templates"));
  TEST_CHECK(is_resolved(site_config.static_dir, "files"));
}

// An absolute `base_url` keeps its scheme, host, port and path, and loses only a trailing `/`,
// which templates would otherwise double when joining it with a path.
static void test_load_normalizes_base_url(void) {
  static const char* const cases[][2] = {
      {"https://example.com", "https://example.com"},
      {"https://example.com/", "https://example.com"},
      {"https://example.com///", "https://example.com"},
      {"http://example.com/gallery/", "http://example.com/gallery"},
      {"https://example.com:8443/gallery", "https://example.com:8443/gallery"},
      {"HTTPS://Example.COM", "HTTPS://Example.COM"},  // the scheme matches case-insensitively
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    const int n = snprintf(toml, sizeof(toml),
                           "title = \"Example Gallery\"\n"
                           "description = \"Example Description\"\n"
                           "author = \"Example Author\"\n"
                           "base_url = \"%s\"\n",
                           cases[i][0]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml));

    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, toml, &log) == 0);
    TEST_CHECK(log.errors == 0);
    TEST_CHECK(str_eq(site_config.base_url, str_from_cstr(cases[i][1])));
  }
}

// `base_url` is optional, so a present but empty `base_url` reads as absent and loads, where sosig
// and surge, whose `base_url` is required, reject it.
static void test_load_accepts_empty_base_url(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(site_config.base_url.len == 0);
}

// Overlapping directories load, because comparing path text cannot tell whether two directories
// overlap. The build compares them by identity before it walks or writes anything.
static void test_load_leaves_directory_overlap_to_the_build(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "media_dir = \"photos\"\n"
      "output_dir = \"photos/generated\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(is_resolved(site_config.media_dir, "photos"));
  TEST_CHECK(is_resolved(site_config.output_dir, "photos/generated"));
}

// A relative directory, given or default, resolves against the config's directory, and an absolute
// one stays as it is, so `sub/fram.toml` without `output_dir` writes `sub/public`.
static void test_load_resolves_paths_against_config_dir(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "sub/fram.toml",
                  "title = \"Example Gallery\"\n"
                  "description = \"Example Description\"\n"
                  "author = \"Example Author\"\n"
                  "media_dir = \"../photos\"\n"
                  "templates_dir = \"tpl\"\n"
                  "static_dir = \"/srv/static\"\n");
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&site_config, "sub/fram.toml", os, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(str_eq(site_config.media_dir, S("sub/../photos")));
  TEST_CHECK(str_eq(site_config.output_dir, S("sub/public")));
  TEST_CHECK(str_eq(site_config.templates_dir, S("sub/tpl")));
  TEST_CHECK(str_eq(site_config.static_dir, S("/srv/static")));
}

// Each `sort` key is read on its own. Each case sets the two keys to different orders, so a load
// that filled one field from the other key would fail one of them.
static void test_load_reads_sort_orders_independently(void) {
  static const struct {
    const char* toml_extra;
    enum GallerySortOrder album_sort_order;
    enum GallerySortOrder media_sort_order;
  } cases[] = {
      {"[sort]\nalbums = \"desc\"\nmedia = \"asc\"\n", GALLERY_SORT_DESC, GALLERY_SORT_ASC},
      {"[sort]\nalbums = \"asc\"\nmedia = \"desc\"\n", GALLERY_SORT_ASC, GALLERY_SORT_DESC},
      {"[sort]\nmedia = \"desc\"\n", GALLERY_SORT_ASC, GALLERY_SORT_DESC},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    char toml[256];
    TEST_ASSERT(snprintf(toml, sizeof(toml),
                         "title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\n%s",
                         cases[i].toml_extra) > 0);
    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, toml, &log) == 0);
    TEST_CHECK(site_config.album_sort_order == cases[i].album_sort_order);
    TEST_CHECK(site_config.media_sort_order == cases[i].media_sort_order);
    TEST_MSG("case %td", i);
  }
}

// An empty `[sort]` table is accepted and keeps both defaults, unlike an empty `derivatives` table,
// which is rejected.
static void test_load_accepts_empty_sort_table(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "[sort]\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(site_config.album_sort_order == GALLERY_SORT_ASC);
  TEST_CHECK(site_config.media_sort_order == GALLERY_SORT_ASC);
}

// Zero is accepted, which is the boundary below every other accepting case in this file. A guard of
// `< 1` on `video_frame_seconds` would reject a legal config while still passing all of them. Zero
// takes each video's frame at its start.
static void test_load_accepts_zero_video_frame_seconds(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "video_frame_seconds = 0\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_CHECK(site_config.video_frame_seconds == 0);
}

// Loading a config path that does not exist reports a read failure naming the path and cause.
static void test_load_rejects_missing_file(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&site_config, CONFIG_PATH, os_memory(), &log) == -1);
  // The cause distinguishes a missing file from an unreadable or malformed one. Derived from the
  // running libc rather than hardcoded, so the assertion is the whole claim and stays portable.
  char reason[ERROR_SYSTEM_MESSAGE_SIZE];
  char expected[EXPECTED_SIZE];
  const int expected_len =
      snprintf(expected, sizeof(expected), "failed to read config: %s ('" CONFIG_PATH "')",
               error_system_message(reason, sizeof(reason), ENOENT));
  TEST_ASSERT(expected_len > 0 && (size_t)expected_len < sizeof(expected));
  TEST_CHECK(test_log_is_one(&log, expected));
}

// A config larger than `TOML_CONFIG_LEN_MAX` is rejected at the read, from its size, naming the
// limit and the size. The host stores only the size of the bytes past the end, so the fixture costs
// no memory.
static void test_load_rejects_oversize_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, CONFIG_PATH, "title = \"Gallery\"\n");
  os_memory_truncate(os, CONFIG_PATH, TOML_CONFIG_LEN_MAX + 1);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&site_config, CONFIG_PATH, os, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "failed to read config: exceeds max file size (%d bytes) at %d bytes "
                         "('" CONFIG_PATH "')\n",
                         TOML_CONFIG_LEN_MAX, TOML_CONFIG_LEN_MAX + 1));
  TEST_CHECK(log.errors == 1);
}

// A config file holding an embedded `NUL` is rejected before the parse, naming the config path.
// `os_read` accepts binary data because it also reads media, so the config boundary itself
// establishes the `NUL`-free text invariant.
static void test_load_rejects_nul_in_file(void) {
  static const char config_data[] = "title = \"Gallery\"\0author = \"Example Author\"\n";
  struct Os* os = os_memory();
  os_memory_write_bytes(os, S(CONFIG_PATH), (struct Str){config_data, sizeof(config_data) - 1});
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&site_config, CONFIG_PATH, os, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log, "failed to read config: contains an embedded NUL byte ('" CONFIG_PATH "')"));
}

// `site_config_load` reports a syntactically malformed config as a parse failure naming the config
// path, rather than reaching the field pass with an empty table and reporting a false missing-key
// error.
static void test_load_rejects_malformed_toml(void) {
  const char toml[] = "title = \n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == -1);
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

/**
 * A config that parses but holds a mistake for the unknown-key check, a string key's type,
 * `base_url`, a directory, `album_template`, a derivative, and `video_frame_seconds`.
 */
static const char TOML_KEYS_INVALID[] =
    "title = 1\n"
    "description = \"Example Description\"\n"
    "author = \"Example Author\"\n"
    "media_dir = \"/\"\n"
    "base_url = \"ftp://example.com\"\n"
    "colour = \"blue\"\n"
    "album_template = \"../album.html\"\n"
    "derivatives = { s = { width = 0, height = 120, quality = 70 } }\n"
    "video_frame_seconds = -1\n";

// The load stops at its first error, so a config with several mistakes reports only the first one
// it checks, and fails.
static void test_load_stops_at_first_error(void) {
  struct Os* os = os_memory();
  os_memory_write(os, CONFIG_PATH, TOML_KEYS_INVALID);
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  struct Arena perm = test_arena();
  TEST_CHECK(site_config_load(&site_config, S(CONFIG_PATH), os, &perm, test_arena(), &log) == -1);
  TEST_CHECK(test_log_is(&log, "unknown config key 'colour'\n"));
}

// Omitting a required key is reported as missing rather than as a type error.
static void test_load_rejects_missing_required_key(void) {
  check_load_rejects_whole(
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n",
      "missing required config key 'title'");
  check_load_rejects_whole(
      "title = \"Example Gallery\"\n"
      "author = \"Example Author\"\n",
      "missing required config key 'description'");
  check_load_rejects_whole(
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n",
      "missing required config key 'author'");
}

// A required key of the wrong TOML type is reported as a type error, not a missing key.
static void test_load_rejects_wrong_key_type(void) {
  static const char* const cases[][2] = {
      {"title = 123\ndescription = \"Example Description\"\nauthor = \"Example Author\"\n",
       "config key 'title' must be a string"},
      {"title = \"Example Gallery\"\ndescription = 123\nauthor = \"Example Author\"\n",
       "config key 'description' must be a string"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\nauthor = 123\n",
       "config key 'author' must be a string"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
  }
}

// A key the schema does not define is rejected, naming the key, so a misspelling cannot silently
// keep a default. A key that is a prefix of a known key, or extends one, is as unknown as any
// other, and so is a table the schema has no place for. A key in a derivative table is named by its
// full dotted path.
static void test_load_rejects_unknown_keys(void) {
  static const char* const cases[][2] = {
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\nouput_dir = \"site\"\n",
       "unknown config key 'ouput_dir'"},
      {"titl = \"Example Gallery\"\ntitle = \"Example Gallery\"\n"
       "description = \"Example Description\"\nauthor = \"Example Author\"\n",
       "unknown config key 'titl'"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\ntitles = \"Example Gallery\"\n",
       "unknown config key 'titles'"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\ndescriptions = \"Example Description\"\n",
       "unknown config key 'descriptions'"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\n[extra]\nkey = 1\n",
       "unknown config key 'extra'"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\n"
       "[derivatives.m]\nwidth = 10\nheight = 10\nquality = 80\ncorp = true\n",
       "unknown config key 'derivatives.m.corp'"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
  }
}

// Only `derivatives` and `sort` hold a table, so a table under any other known key fails that key's
// type check.
static void test_load_rejects_table_values(void) {
  static const char* const cases[][2] = {
      {"description = \"Example Description\"\nauthor = \"Example Author\"\n[title]\n"
       "name = \"Example Gallery\"\n",
       "config key 'title' must be a string"},
      {"title = \"Example Gallery\"\nauthor = \"Example Author\"\n[description]\n"
       "text = \"Example Description\"\n",
       "config key 'description' must be a string"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\n"
       "[aggregate_templates]\nname = \"sitemap.xml\"\n",
       "config key 'aggregate_templates' must be an array"},
      {"title = \"Example Gallery\"\ndescription = \"Example Description\"\n"
       "author = \"Example Author\"\n"
       "[video_frame_seconds]\nseconds = 1\n",
       "config key 'video_frame_seconds' must be an integer"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
    TEST_MSG("case %td", i);
  }
}

// An integer key rejects a TOML type other than integer and enforces its exact lower bound, which
// is 1 for a derivative dimension or quality and 0 for `video_frame_seconds`. A non-integer fails a
// different assertion from an out-of-range one: the user has to change the type, not the number.
static void test_load_rejects_invalid_integer_type_and_minimum(void) {
  check_load_rejects("derivatives = { s = { width = \"320\", height = 120, quality = 70 } }\n",
                     "config key 'derivatives.s.width' must be an integer");
  check_load_rejects("derivatives = { s = { width = 0, height = 120, quality = 70 } }\n",
                     "config key 'derivatives.s.width' must be at least 1: 0");
  check_load_rejects("derivatives = { s = { width = 120, height = 120, quality = 0 } }\n",
                     "config key 'derivatives.s.quality' must be at least 1: 0");
  check_load_rejects("video_frame_seconds = -1\n",
                     "config key 'video_frame_seconds' must be at least 0: -1");
}

// A TOML escape decoding to `U+0000` is rejected per key. tomlc17 accepts the escape and returns a
// string whose bytes carry an embedded `NUL`, so this is a separate boundary from the file check:
// the file itself holds no `NUL`. The byte has no meaning in any text format `fram` reads or
// writes, so the load refuses it.
static void test_load_rejects_nul_in_string_values(void) {
  static const char* const cases[][2] = {
      {"title = \"a\\u0000b\"\ndescription = \"D\"\nauthor = \"A\"\n",
       "config key 'title' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"a\\u0000b\"\nauthor = \"A\"\n",
       "config key 'description' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\nbase_url = \"a\\u0000b\"\n",
       "config key 'base_url' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\nmedia_dir = \"a\\u0000b\"\n",
       "config key 'media_dir' must not contain a NUL byte"},
      {"title = \"T\"\ndescription = \"D\"\nauthor = \"A\"\n"
       "aggregate_templates = [\"a\\u0000b\"]\n",
       "config key 'aggregate_templates' must not contain a NUL byte"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct SiteConfig site_config;
    site_config_init(&site_config);
    struct Log log = test_log();
    TEST_CHECK(load_text(&site_config, cases[i][0], &log) == -1);
    TEST_CHECK(test_log_is_one(&log, cases[i][1]));
  }
}

// A directory key that is empty, or that trims to empty, is rejected by name.
static void test_load_rejects_empty_directory_keys(void) {
  static const char* const cases[][2] = {
      {"media_dir = \"\"\n", "config key 'media_dir' must not be empty"},
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
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https:///path\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(
      &log,
      "config key 'base_url' must be an absolute 'http://' or 'https://' URL with a "
      "host: 'https:///path'"));
}

// A `base_url` with a query, a fragment, a space or a control byte would break every link below it,
// so it is rejected. `core/url`'s tests cover each byte.
static void test_load_rejects_base_url_with_path_breaking_byte(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "base_url = \"https://example.com/x?y=1\"\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == -1);
  TEST_CHECK(test_log_is_one(&log,
                             "config key 'base_url' must not hold '?', '#', a space or a control "
                             "character: 'https://example.com/x?y=1'"));
}

// An unsafe `album_template` is rejected at load, in the singular wording that names the key,
// rather than flowing through to the render and failing once per album. Parent-relative, absolute
// and empty names are all unsafe.
static void test_load_rejects_unsafe_album_template(void) {
  const char* const unsafe_names[] = {"../evil.html", "/etc/passwd", ""};

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(unsafe_names) / sizeof(unsafe_names[0])); i++) {
    char toml_extra[256];
    const int n =
        snprintf(toml_extra, sizeof(toml_extra), "album_template = \"%s\"\n", unsafe_names[i]);
    TEST_CHECK(n > 0 && (size_t)n < sizeof(toml_extra));
    char expected[EXPECTED_SIZE];
    const int expected_len = snprintf(
        expected, sizeof(expected),
        "config key 'album_template' must be a safe relative template name: '%s'", unsafe_names[i]);
    TEST_CHECK(expected_len > 0 && (size_t)expected_len < sizeof(expected));
    check_load_rejects(toml_extra, expected);
  }
}

// A non-array value, a non-string element, and an unsafe name for a template array key are each
// rejected.
static void test_load_rejects_invalid_template_arrays(void) {
  check_load_rejects("aggregate_templates = \"sitemap.xml\"\n",
                     "config key 'aggregate_templates' must be an array");
  check_load_rejects("aggregate_templates = [1]\n",
                     "config key 'aggregate_templates' must contain only strings");
  check_load_rejects("aggregate_templates = [\"../evil.html\"]\n",
                     "config key 'aggregate_templates' must contain only safe relative template "
                     "names: '../evil.html'");
}

// Every element of a template array is checked even after one fails, so each unsafe name is
// reported in element order. A wrong type or a `NUL` byte is reported once however many elements
// have it, since its message names no element.
static void test_load_reports_every_invalid_template_name(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "aggregate_templates = [\"../a.xml\", 1, \"a\\u0000b\", \"ok.xml\", \"/b.xml\", "
      "2, \"c\\u0000d\"]\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "config key 'aggregate_templates' must contain only safe relative "
                         "template names: '../a.xml'\n"
                         "config key 'aggregate_templates' must contain only strings\n"
                         "config key 'aggregate_templates' must not contain a NUL byte\n"
                         "config key 'aggregate_templates' must contain only safe relative "
                         "template names: '/b.xml'\n"));
  TEST_CHECK(log.errors == 4);
}

// Dimension, quality, and video-frame upper bounds are exact and named in diagnostics.
static void test_load_rejects_integer_upper_bounds(void) {
  char toml_extra[256];
  TEST_ASSERT(snprintf(toml_extra, sizeof(toml_extra),
                       "derivatives = { l = { width = %d, height = 1, quality = 85 } }\n",
                       IMAGE_ENCODED_DIMENSION_MAX + 1) > 0);
  char expected[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.l.width' exceeds max WebP dimension (%d) at %d",
                       IMAGE_ENCODED_DIMENSION_MAX, IMAGE_ENCODED_DIMENSION_MAX + 1) > 0);
  check_load_rejects(toml_extra, expected);

  check_load_rejects("derivatives = { l = { width = 1, height = 1, quality = 101 } }\n",
                     "config key 'derivatives.l.quality' exceeds max WebP quality (100) at 101");
  // `VIDEO_FRAME_SECONDS_MAX` is file-local to `site_config.c`, so the one day below is spelled out
  // and must change with it.
  check_load_rejects("video_frame_seconds = 86401\n",
                     "config key 'video_frame_seconds' exceeds max video frame seconds (86400) at "
                     "86401");
}

// Even individually valid dimensions cannot describe a box beyond the decoder's pixel bound.
static void test_load_rejects_dimension_product_overflow(void) {
  char toml_extra[256];
  TEST_ASSERT(snprintf(toml_extra, sizeof(toml_extra),
                       "derivatives = { xxl = { width = %d, height = %d, quality = 85 } }\n",
                       IMAGE_ENCODED_DIMENSION_MAX, IMAGE_ENCODED_DIMENSION_MAX) > 0);
  char expected[EXPECTED_SIZE];
  TEST_ASSERT(snprintf(expected, sizeof(expected),
                       "config key 'derivatives.xxl' dimensions exceed max pixel count (%d "
                       "pixels) at %d x %d",
                       IMAGE_PIXEL_COUNT_MAX, IMAGE_ENCODED_DIMENSION_MAX,
                       IMAGE_ENCODED_DIMENSION_MAX) > 0);
  check_load_rejects(toml_extra, expected);
}

// Derivative tables require one of the fixed names, dimensions, and boolean crop flags. A name that
// holds a `NUL` byte is rejected without being named, since printing it would stop at the `NUL`.
static void test_load_rejects_invalid_derivative_tables(void) {
  check_load_rejects("derivatives = { s = { width = 120, quality = 70 } }\n",
                     "missing required config key 'derivatives.s.height'");
  check_load_rejects("derivatives = { s = { width = 120, height = 120 } }\n",
                     "missing required config key 'derivatives.s.quality'");
  check_load_rejects("derivatives = { XL = { width = 1, height = 1, quality = 1 } }\n",
                     "config key 'derivatives' name must be one of 'xs', 's', 'm', 'l', 'xl' or "
                     "'xxl': 'XL'");
  check_load_rejects("derivatives = { thumb = { width = 1, height = 1, quality = 1 } }\n",
                     "config key 'derivatives' name must be one of 'xs', 's', 'm', 'l', 'xl' or "
                     "'xxl': 'thumb'");
  check_load_rejects("derivatives = { \"a\\u0000b\" = { width = 1, height = 1, quality = 1 } }\n",
                     "config key 'derivatives' name must not contain a NUL byte");
  check_load_rejects("derivatives = { s = { width = 1, height = 1, quality = 70, crop = 1 } }\n",
                     "config key 'derivatives.s.crop' must be a boolean");
}

// The `sort` table must be a table, holds only its two keys, and each key takes exactly the
// lowercase string `asc` or `desc`. A case variant, a longer word, and an empty string are each
// rejected and named. A value holding a `NUL` byte is rejected without being named, since printing
// it would stop at the `NUL`.
static void test_load_rejects_invalid_sort_tables(void) {
  check_load_rejects("sort = \"desc\"\n", "config key 'sort' must be a table");
  check_load_rejects("sort = { album = \"desc\" }\n", "unknown config key 'sort.album'");
  check_load_rejects("sort = { media = 1 }\n", "config key 'sort.media' must be a string");
  check_load_rejects("sort = { albums = \"ASC\" }\n",
                     "config key 'sort.albums' must be 'asc' or 'desc': 'ASC'");
  check_load_rejects("sort = { media = \"descending\" }\n",
                     "config key 'sort.media' must be 'asc' or 'desc': 'descending'");
  check_load_rejects("sort = { media = \"\" }\n",
                     "config key 'sort.media' must be 'asc' or 'desc': ''");
  check_load_rejects("sort = { albums = \"a\\u0000sc\" }\n",
                     "config key 'sort.albums' must not contain a NUL byte");
}

// Printing emits every key, with the defaults `site_config_init` supplies for the optional ones.
// `title`, `description`, and `author` have no default and are set here so the output shows each. A
// derivative prints `crop` only when it crops.
static void test_print_defaults(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Example Gallery");
  site_config.description = S("Example Description");
  site_config.author = S("Example Author");

  // The whole output, not a set of per-line searches: only an exact comparison can catch a key that
  // is missing, duplicated, extra, or emitted in the wrong order.
  TEST_CHECK(os_memory_is_printed(
      print_config(&site_config), 1,
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "media_dir = \"media\"\n"
      "output_dir = \"public\"\n"
      "templates_dir = \"templates\"\n"
      "static_dir = \"static\"\n"
      "album_template = \"album.html\"\n"
      "aggregate_templates = []\n"
      "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
      "m = { width = 520, height = 360, quality = 80, crop = true }, "
      "l = { width = 792, height = 594, quality = 85 } }\n"
      "sort = { albums = \"asc\", media = \"asc\" }\n"
      "video_frame_seconds = 1\n"));
}

// Printing escapes quotes, backslashes, and every named control escape (\t \b \n \f \r). It uses
// the `\uXXXX` fallback for a sub-`0x20` byte and `U+007F` (`DEL`).
static void test_print_escapes_strings(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Quote \" and \\ backslash");
  site_config.description = S("Example Description");
  // Each hex escape is followed by a non-hex-digit char ('Z', 'G') so it ends after one byte.
  site_config.author = S("a\tA\bB\nC\fD\rE\x01Z\x7FG");

  // Compared whole, so an escape leaking into a neighboring field cannot hide.
  TEST_CHECK(os_memory_is_printed(
      print_config(&site_config), 1,
      "title = \"Quote \\\" and \\\\ backslash\"\n"
      "description = \"Example Description\"\n"
      "author = \"a\\tA\\bB\\nC\\fD\\rE\\u0001Z\\u007FG\"\n"
      "media_dir = \"media\"\n"
      "output_dir = \"public\"\n"
      "templates_dir = \"templates\"\n"
      "static_dir = \"static\"\n"
      "album_template = \"album.html\"\n"
      "aggregate_templates = []\n"
      "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
      "m = { width = 520, height = 360, quality = 80, crop = true }, "
      "l = { width = 792, height = 594, quality = 85 } }\n"
      "sort = { albums = \"asc\", media = \"asc\" }\n"
      "video_frame_seconds = 1\n"));
}

// Printing a config and loading the printed output back yields the same field values, exercising
// the round-trip invariant documented on `site_config_print` across escaped strings (including a
// `U+007F` byte), multi-element template arrays, cropping and non-cropping derivatives, and two
// different sort orders. The printed output is loaded from `CONFIG_DIR`, not the working directory,
// so its relative directories come back resolved against that directory, and only the absolute one
// comes back as it is.
static void test_print_load_round_trips(void) {
  static const struct Str aggregates[] = {STR_INIT("sitemap.xml"), STR_INIT("about/index.html")};
  static const struct GalleryDerivative derivatives[] = {
      {.name = STR_INIT("xs"), .width_px = 200, .height_px = 150, .quality = 61, .is_crop = true},
      {.name = STR_INIT("xl"), .width_px = 1200, .height_px = 900, .quality = 93, .is_crop = false},
  };

  struct SiteConfig site_config;
  site_config_init(&site_config);
  // '!' after `\x7F` is not a hex digit, so the escape ends after the single `DEL` byte.
  site_config.title = S("Quote \" back\\slash\nnewline\ttab\x7F!");
  site_config.description = S("Example Description");
  site_config.author = S("Example Author");
  site_config.base_url = S("https://example.com/gallery");
  site_config.media_dir = S("photos");
  site_config.output_dir = S("dist");
  site_config.templates_dir = S("layouts");
  site_config.static_dir = S("/srv/files");
  site_config.album_template = S("pages/album.html");
  site_config.aggregate_templates = aggregates;
  site_config.aggregate_template_count = 2;
  site_config.derivatives = derivatives;
  site_config.derivative_count = 2;
  site_config.album_sort_order = GALLERY_SORT_DESC;
  site_config.media_sort_order = GALLERY_SORT_ASC;
  site_config.video_frame_seconds = 0;

  struct Os* os = os_memory();
  os_memory_write_bytes(os, S(CONFIG_PATH), os_memory_printed(print_config(&site_config), 1));
  struct SiteConfig reloaded;
  site_config_init(&reloaded);
  struct Log log = test_log();
  TEST_CHECK(load_site_config(&reloaded, CONFIG_PATH, os, &log) == 0);

  TEST_CHECK(str_eq(reloaded.title, site_config.title));
  TEST_CHECK(str_eq(reloaded.description, site_config.description));
  TEST_CHECK(str_eq(reloaded.author, site_config.author));
  TEST_CHECK(str_eq(reloaded.base_url, site_config.base_url));
  TEST_CHECK(is_resolved(reloaded.media_dir, "photos"));
  TEST_CHECK(is_resolved(reloaded.output_dir, "dist"));
  TEST_CHECK(is_resolved(reloaded.templates_dir, "layouts"));
  TEST_CHECK(str_eq(reloaded.static_dir, site_config.static_dir));
  TEST_CHECK(str_eq(reloaded.album_template, site_config.album_template));
  TEST_ASSERT(reloaded.aggregate_template_count == 2);
  TEST_CHECK(str_eq(reloaded.aggregate_templates[0], S("sitemap.xml")));
  TEST_CHECK(str_eq(reloaded.aggregate_templates[1], S("about/index.html")));
  TEST_ASSERT(reloaded.derivative_count == 2);
  for (ptrdiff_t i = 0; i < 2; i++) {
    TEST_CHECK(str_eq(reloaded.derivatives[i].name, derivatives[i].name));
    TEST_CHECK(reloaded.derivatives[i].width_px == derivatives[i].width_px);
    TEST_CHECK(reloaded.derivatives[i].height_px == derivatives[i].height_px);
    TEST_CHECK(reloaded.derivatives[i].quality == derivatives[i].quality);
    TEST_CHECK(reloaded.derivatives[i].is_crop == derivatives[i].is_crop);
  }
  TEST_CHECK(reloaded.album_sort_order == site_config.album_sort_order);
  TEST_CHECK(reloaded.media_sort_order == site_config.media_sort_order);
  TEST_CHECK(reloaded.video_frame_seconds == site_config.video_frame_seconds);
}

// An empty template array survives a load followed by a print, and prints as `[]`. Neither
// operation alone tests this sequence, which `fram config` runs on a config that the loader
// accepted.
static void test_print_empty_template_array(void) {
  const char toml[] =
      "title = \"Example Gallery\"\n"
      "description = \"Example Description\"\n"
      "author = \"Example Author\"\n"
      "aggregate_templates = []\n";

  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Log log = test_log();
  TEST_CHECK(load_text(&site_config, toml, &log) == 0);
  TEST_CHECK(log.errors == 0);
  TEST_ASSERT(site_config.aggregate_template_count == 0);
  // The empty array is a distinct non-`NULL` pointer, which the `nonnull` consumers need.
  TEST_CHECK(site_config.aggregate_templates != NULL);

  // Compared whole: an empty array must print as `[]` and must not disturb its neighbors. The
  // directories print resolved against `CONFIG_DIR`, which holds the config.
  TEST_CHECK(
      os_memory_is_printed(
          print_config(&site_config), 1,
          "title = \"Example Gallery\"\n"
          "description = \"Example Description\"\n"
          "author = \"Example Author\"\n"
          "media_dir = \"" CONFIG_DIR "/media\"\n"
          "output_dir = \"" CONFIG_DIR "/public\"\n"
          "templates_dir = \"" CONFIG_DIR "/templates\"\n"
          "static_dir = \"" CONFIG_DIR "/static\"\n"
          "album_template = \"album.html\"\n"
          "aggregate_templates = []\n"
          "derivatives = { s = { width = 120, height = 120, quality = 70, crop = true }, "
          "m = { width = 520, height = 360, quality = 80, crop = true }, "
          "l = { width = 792, height = 594, quality = 85 } }\n"
          "sort = { albums = \"asc\", media = \"asc\" }\n"
          "video_frame_seconds = 1\n"));
}

// A standard output that refuses the write makes `site_config_print` report `-1` with the write's
// own `errno`, which is what lets the `config` command name a reason instead of reporting success
// over lost output. The same shape is asserted for `cli_print_version` by
// `test_print_version_reports_write_failure`.
static void test_print_reports_write_failure(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Example Gallery");
  site_config.description = S("Example Description");
  site_config.author = S("Example Author");

  struct Os* os = os_memory();
  os_memory_fail_print(os, 1, 0, EBADF);
  errno = 0;
  TEST_CHECK(site_config_print(&site_config, os, test_arena()) == -1);
  TEST_CHECK(errno == EBADF);
}

// The recipe component names every output-affecting value: the recipe version, each derivative's
// name, box, crop flag, and quality in configuration order, and the video frame time in
// milliseconds.
static void test_derivative_recipe_encodes_output_values(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct Arena arena = test_arena();
  struct Str recipe = site_config_derivative_recipe(&site_config, &arena);
  TEST_CHECK(str_eq(recipe, S("v2-s-120x120c-q70-m-520x360c-q80-l-792x594-q85-f1000")));

  const struct GalleryDerivative custom[] = {
      {.name = STR_INIT("xs"), .width_px = 7, .height_px = 8, .quality = 42, .is_crop = true},
      {.name = STR_INIT("xl"), .width_px = 90, .height_px = 100, .quality = 91, .is_crop = false},
  };
  site_config.derivatives = custom;
  site_config.derivative_count = sizeof(custom) / sizeof(custom[0]);
  site_config.video_frame_seconds = 0;
  recipe = site_config_derivative_recipe(&site_config, &arena);
  TEST_CHECK(str_eq(recipe, S("v2-xs-7x8c-q42-xl-90x100-q91-f0")));
}

TEST_LIST = {
    {"load applies required and defaults", test_load_applies_required_and_defaults},
    {"load overrides optional keys", test_load_overrides_optional_keys},
    {"load normalizes directory keys", test_load_normalizes_directory_keys},
    {"load normalizes base url", test_load_normalizes_base_url},
    {"load accepts empty base url", test_load_accepts_empty_base_url},
    {"load leaves directory overlap to the build", test_load_leaves_directory_overlap_to_the_build},
    {"load resolves paths against config dir", test_load_resolves_paths_against_config_dir},
    {"load reads sort orders independently", test_load_reads_sort_orders_independently},
    {"load accepts empty sort table", test_load_accepts_empty_sort_table},
    {"load accepts zero video frame seconds", test_load_accepts_zero_video_frame_seconds},
    {"load rejects missing file", test_load_rejects_missing_file},
    {"load rejects oversize file", test_load_rejects_oversize_file},
    {"load rejects nul in file", test_load_rejects_nul_in_file},
    {"load rejects malformed toml", test_load_rejects_malformed_toml},
    {"load stops at first error", test_load_stops_at_first_error},
    {"load rejects missing required key", test_load_rejects_missing_required_key},
    {"load rejects wrong key type", test_load_rejects_wrong_key_type},
    {"load rejects unknown keys", test_load_rejects_unknown_keys},
    {"load rejects table values", test_load_rejects_table_values},
    {"load rejects invalid integer type and minimum",
     test_load_rejects_invalid_integer_type_and_minimum},
    {"load rejects nul in string values", test_load_rejects_nul_in_string_values},
    {"load rejects empty directory keys", test_load_rejects_empty_directory_keys},
    {"load rejects base url without host", test_load_rejects_base_url_without_host},
    {"load rejects base url with path breaking byte",
     test_load_rejects_base_url_with_path_breaking_byte},
    {"load rejects unsafe album template", test_load_rejects_unsafe_album_template},
    {"load rejects invalid template arrays", test_load_rejects_invalid_template_arrays},
    {"load reports every invalid template name", test_load_reports_every_invalid_template_name},
    {"load rejects integer upper bounds", test_load_rejects_integer_upper_bounds},
    {"load rejects dimension product overflow", test_load_rejects_dimension_product_overflow},
    {"load rejects invalid derivative tables", test_load_rejects_invalid_derivative_tables},
    {"load rejects invalid sort tables", test_load_rejects_invalid_sort_tables},
    {"print defaults", test_print_defaults},
    {"print escapes strings", test_print_escapes_strings},
    {"print load round trips", test_print_load_round_trips},
    {"print empty template array", test_print_empty_template_array},
    {"print reports write failure", test_print_reports_write_failure},
    {"derivative recipe encodes output values", test_derivative_recipe_encodes_output_values},
    {NULL, NULL},
};
