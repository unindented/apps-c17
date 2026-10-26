#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/build_cache.h"
#include "domain/site_config.h"
#include "domain/sosig_version.h"
#include "markup/highlight.h"
#include "os/os.h"
#include "testing/test_support.h"

/**
 * @brief Formats a build cache of copies of `entries` for `site_config`, as a build writes one.
 *
 * @param entries     Rows to format, in any order. May be `NULL` only when `entry_count` is 0.
 * @param entry_count Number of rows in `entries`.
 * @param site_config Configuration whose page recipe leads the text. Must not be `NULL`.
 * @param arena       Arena that owns the text and the copies. Must not be `NULL`.
 * @return The text, owned by `arena`.
 */
static struct Str format_cache(const struct BuildCacheEntry* entries,
                               ptrdiff_t entry_count,
                               const struct SiteConfig* site_config,
                               struct Arena* arena) {
  struct BuildCache cache = {.entries = arena_new(arena, entry_count, struct BuildCacheEntry)};
  for (ptrdiff_t i = 0; i < entry_count; i++) {
    cache.entries[cache.count++] = entries[i];
  }
  struct Buf text = {.arena = arena};
  build_cache_format(&text, site_config, &cache);
  return buf_str(&text);
}

/**
 * @brief Returns the page recipe of `site_config`, with its newline.
 *
 * @param site_config Configuration of the recipe. Must not be `NULL`.
 * @param arena       Arena that owns the text. Must not be `NULL`.
 * @return The recipe, owned by `arena`.
 */
static struct Str recipe_of(const struct SiteConfig* site_config, struct Arena* arena) {
  return format_cache(NULL, 0, site_config, arena);
}

/** Three rows out of order: two content pages, one that read the entry list, and a template. */
static const struct BuildCacheEntry ENTRIES[] = {
    {.kind = BUILD_CACHE_KIND_TEMPLATE,
     .name = STR_INIT("content.html"),
     .len = 20,
     .mtime_s = 1782864000,
     .mtime_ns = 7},
    {.kind = BUILD_CACHE_KIND_CONTENT,
     .name = STR_INIT("posts/b.html"),
     .len = 1234,
     .mtime_s = 1782864001,
     .mtime_ns = 999999999,
     .is_entry_list_read = true},
    {.kind = BUILD_CACHE_KIND_CONTENT,
     .name = STR_INIT("posts/a.html"),
     .len = 99,
     .mtime_s = 1782864002},
};

// A build cache a build wrote parses back to its rows under the same page recipe, sorted by kind
// and then by name, so a row is found by its kind and name, and its stat compares to the input's.
static void test_round_trips_sorted_rows(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  const struct Str text = format_cache(ENTRIES, 3, &site_config, &arena);
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &site_config, &arena, &cache, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(cache.count == 3);
  TEST_CHECK(cache.count == 3 && str_eq(cache.entries[0].name, S("posts/a.html")) &&
             str_eq(cache.entries[1].name, S("posts/b.html")) &&
             cache.entries[2].kind == BUILD_CACHE_KIND_TEMPLATE);

  struct OsStat stat = {.len = 1234, .mtime_s = 1782864001, .mtime_ns = 999999999};
  const struct BuildCacheEntry* row =
      build_cache_find(&cache, BUILD_CACHE_KIND_CONTENT, S("posts/b.html"), &stat);
  TEST_CHECK(row != NULL && row->is_entry_list_read);
  stat.mtime_ns--;
  TEST_CHECK(build_cache_find(&cache, BUILD_CACHE_KIND_CONTENT, S("posts/b.html"), &stat) == NULL);
  stat = (struct OsStat){.len = 99, .mtime_s = 1782864002};
  row = build_cache_find(&cache, BUILD_CACHE_KIND_CONTENT, S("posts/a.html"), &stat);
  TEST_CHECK(row != NULL && !row->is_entry_list_read);
  stat = (struct OsStat){.len = 20, .mtime_s = 1782864000, .mtime_ns = 7};
  TEST_CHECK(build_cache_find(&cache, BUILD_CACHE_KIND_CONTENT, S("content.html"), &stat) == NULL);
  TEST_CHECK(build_cache_find(&cache, BUILD_CACHE_KIND_TEMPLATE, S("content.html"), &stat) != NULL);
}

// A row whose name holds a tab or a newline is left out, so its input is made every build and no

// A file dated before 1970 has negative seconds, and its row parses and matches like any other, so
// one such file does not make the whole build cache unreadable.
static void test_round_trips_a_time_before_1970(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  const struct BuildCacheEntry entries[] = {
      {.kind = BUILD_CACHE_KIND_CONTENT, .name = S("old.html"), .len = 3, .mtime_s = -315590400},
  };
  const struct Str text = format_cache(entries, 1, &site_config, &arena);
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(text, &site_config, &arena, &cache, &log) == 0);
  const struct OsStat stat = {.len = 3, .mtime_s = -315590400};
  TEST_CHECK(build_cache_find(&cache, BUILD_CACHE_KIND_CONTENT, S("old.html"), &stat) != NULL);
}
// name needs escaping.
static void test_leaves_out_name_with_tab_or_newline(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  struct BuildCacheEntry entries[2] = {ENTRIES[0], ENTRIES[0]};
  entries[0].name = S("a\tb.html");
  entries[1].name = S("a\nb.html");
  const struct Str text = format_cache(entries, 2, &site_config, &arena);
  TEST_CHECK(str_eq(text, recipe_of(&site_config, &arena)));
}

// The page recipe starts with the site fields a render reads, in the order the config lists them,
// so another title, description, author or base URL is another page recipe.
static void test_recipe_starts_with_site_fields(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Site");
  site_config.description = S("Notes");
  site_config.author = S("Ada");
  site_config.base_url = S("https://example.com");
  TEST_CHECK(str_startswith(recipe_of(&site_config, &arena),
                            S("title=Site\tdescription=Notes\tauthor=Ada\t"
                              "base_url=https://example.com\tpermalink=")));
}

// The page recipe records the grammars the binary links, just before `PAGE_RECIPE_VERSION`, so a
// build cache that a binary of another set wrote is another page recipe's.
static void test_recipe_records_grammars(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  const struct Str grammars = highlight_grammar_list();
  struct Buf tail = {.arena = &arena};
  buf_printf(&tail, "\tgrammars=%.*s\tpage_recipe_version=%d\n", STR_FMT(grammars),
             PAGE_RECIPE_VERSION);
  TEST_CHECK(str_endswith(recipe_of(&site_config, &arena), buf_str(&tail)));
}

// Each `\`, tab and newline of a config text is escaped, and each `,` inside a template name, so a
// text that holds the next field's `name=`, a text that holds the escape itself, and one name that
// holds a comma each give another page recipe than the configs they would otherwise look like.
static void test_parse_tells_escaped_recipe_texts_apart(void) {
  struct Arena arena = test_arena();
  struct SiteConfig tab;
  site_config_init(&tab);
  tab.title = S("Site\tdescription=X");
  tab.description = S("Y");
  struct SiteConfig other = tab;
  other.title = S("Site");
  other.description = S("X\tdescription=Y");
  struct SiteConfig escaped = tab;
  escaped.title = S("Site\\tdescription=X");
  const struct Str text = format_cache(ENTRIES, 3, &tab, &arena);
  TEST_CHECK(str_startswith(text, S("title=Site\\tdescription=X\tdescription=Y\tauthor=\t")));
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &tab, &arena, &cache, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(cache.count == 3);
  struct Log other_log = test_log();
  TEST_CHECK(build_cache_parse(text, &other, &arena, &cache, &other_log) == -1);
  TEST_CHECK(test_log_is(&other_log, "build cache was written for another page recipe\n"));
  struct Log escaped_log = test_log();
  TEST_CHECK(build_cache_parse(text, &escaped, &arena, &cache, &escaped_log) == -1);
  TEST_CHECK(test_log_is(&escaped_log, "build cache was written for another page recipe\n"));

  struct SiteConfig newline;
  site_config_init(&newline);
  newline.description = S("a\nb\\");
  newline.author = S("Ada");
  const struct Str newline_text = format_cache(ENTRIES, 3, &newline, &arena);
  TEST_CHECK(str_startswith(newline_text, S("title=\tdescription=a\\nb\\\\\tauthor=Ada\t")));
  struct Log newline_log = test_log();
  TEST_CHECK(build_cache_parse(newline_text, &newline, &arena, &cache, &newline_log) == 0);
  TEST_CHECK(test_log_is_empty(&newline_log));
  TEST_CHECK(cache.count == 3);

  static const struct Str comma_names[] = {STR_INIT("a.html,b.html")};
  static const struct Str two_names[] = {STR_INIT("a.html"), STR_INIT("b.html")};
  struct SiteConfig comma;
  site_config_init(&comma);
  comma.feed_templates = comma_names;
  comma.feed_template_count = 1;
  struct SiteConfig two = comma;
  two.feed_templates = two_names;
  two.feed_template_count = 2;
  const struct Str comma_text = format_cache(ENTRIES, 3, &comma, &arena);
  const struct Str generator = sosig_generator_string();
  const struct Str grammars = highlight_grammar_list();
  struct Buf comma_tail = {.arena = &arena};
  buf_printf(&comma_tail,
             "\tfeed_templates=a.html\\,b.html\tfeed_count=%td\tgenerator=%.*s\tgrammars=%.*s\t"
             "page_recipe_version=%d\n",
             comma.feed_count, STR_FMT(generator), STR_FMT(grammars), PAGE_RECIPE_VERSION);
  TEST_CHECK(str_endswith(recipe_of(&comma, &arena), buf_str(&comma_tail)));
  struct Log comma_log = test_log();
  TEST_CHECK(build_cache_parse(comma_text, &two, &arena, &cache, &comma_log) == -1);
  TEST_CHECK(test_log_is(&comma_log, "build cache was written for another page recipe\n"));
}

// A build cache written under another page recipe is not used: another value of a config field a
// render reads changes the first line, and so does another generator or `PAGE_RECIPE_VERSION`.
static void test_rejects_another_page_recipe(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Site");
  const struct Str text = format_cache(ENTRIES, 3, &site_config, &arena);
  site_config.title = S("Renamed");
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &site_config, &arena, &cache, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache was written for another page recipe\n"));
  TEST_CHECK(cache.count == 0);

  // The description is a field a render reads too.
  site_config.title = S("Site");
  site_config.description = S("Notes");
  const struct Str described = format_cache(ENTRIES, 3, &site_config, &arena);
  site_config.description = S("Other notes");
  struct Log description_log = test_log();
  TEST_CHECK(build_cache_parse(described, &site_config, &arena, &cache, &description_log) == -1);
  TEST_CHECK(test_log_is(&description_log, "build cache was written for another page recipe\n"));
  TEST_CHECK(cache.count == 0);
}

// An empty file, which a build that did not finish leaves, and one that does not parse are an empty
// build cache, reported with the first line that does not parse.
static void test_rejects_rows_that_do_not_parse(void) {
  struct Arena arena = test_arena();
  struct SiteConfig site_config;
  site_config_init(&site_config);
  const struct Str recipe = recipe_of(&site_config, &arena);
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(S(""), &site_config, &arena, &cache, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache is empty, as a build that did not finish leaves it\n"));

  static const char* const bad_rows[] = {
      "content\ta.html\t1\t2\t3\t0",             // no newline
      "content\ta.html\t1\t2\t3\n",              // no entry-list flag
      "content\ta.html\t1\t2\t3\t2\n",           // a flag that is neither 0 nor 1
      "template\ta.html\t1\t2\t3\t0\n",          // a flag on a template row
      "page\ta.html\t1\t2\t3\t0\n",              // another kind
      "content\ta.html\t1\t2\t1000000000\t0\n",  // nanoseconds out of range
      "content\t\t1\t2\t3\t0\n",                 // no name
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(bad_rows) / sizeof(bad_rows[0])); i++) {
    struct Buf bad = {.arena = &arena};
    buf_append(&bad, recipe);
    buf_append(&bad, str_from_cstr(bad_rows[i]));
    log = test_log();
    TEST_CHECK_(build_cache_parse(buf_str(&bad), &site_config, &arena, &cache, &log) == -1,
                "row %td", i);
    TEST_CHECK(test_log_is(&log, "build cache does not parse at line 2\n"));
  }

  struct Buf unsorted = {.arena = &arena};
  buf_append(&unsorted, recipe);
  buf_append(&unsorted, S("template\ta.html\t1\t2\t3\ncontent\ta.html\t1\t2\t3\t0\n"));
  log = test_log();
  TEST_CHECK(build_cache_parse(buf_str(&unsorted), &site_config, &arena, &cache, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache does not parse at line 3\n"));
}

TEST_LIST = {
    {"round trips sorted rows", test_round_trips_sorted_rows},
    {"round trips a time before 1970", test_round_trips_a_time_before_1970},
    {"leaves out name with tab or newline", test_leaves_out_name_with_tab_or_newline},
    {"recipe starts with site fields", test_recipe_starts_with_site_fields},
    {"recipe records grammars", test_recipe_records_grammars},
    {"parse tells escaped recipe texts apart", test_parse_tells_escaped_recipe_texts_apart},
    {"rejects another page recipe", test_rejects_another_page_recipe},
    {"rejects rows that do not parse", test_rejects_rows_that_do_not_parse},
    {NULL, NULL},
};
