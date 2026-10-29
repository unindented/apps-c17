#include <acutest.h>
#include <stdbool.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "domain/build_cache.h"
#include "domain/site_config.h"
#include "os/os.h"
#include "testing/test_support.h"

/**
 * @brief Returns a config with the required site keys set, as a loaded one would hold them.
 *
 * @return The config.
 */
static struct SiteConfig make_site_config(void) {
  struct SiteConfig site_config;
  site_config_init(&site_config);
  site_config.title = S("Code");
  site_config.description = S("Repositories");
  site_config.author = S("Ada");
  site_config.base_url = S("https://git.example.org");
  return site_config;
}

/**
 * @brief Formats `rows` as a build cache of `site_config`.
 *
 * @param site_config Config of the page recipe. Must not be `NULL`.
 * @param rows        Rows to format, `count` of them, which this sorts. Must not be `NULL`.
 * @param count       Number of rows.
 * @return The text, in a reservation of its own.
 */
static struct Str format_rows(const struct SiteConfig* site_config,
                              struct BuildCacheRow* rows,
                              ptrdiff_t count) {
  struct Arena arena = test_arena();
  struct Buf text = {.arena = &arena};
  struct BuildCache cache = {.rows = rows, .count = count};
  build_cache_format(&text, site_config, &cache);
  return buf_str(&text);
}

/**
 * @brief Reports whether a build cache that holds `rows` after the page recipe of
 *        `make_site_config` is refused at `line`.
 *
 * @param rows Terminated text of the rows. Must not be `NULL`.
 * @param line Line of the file the refusal must name, from 1 for the recipe.
 * @return `true` when the parse failed with exactly that line, and left the arena and the cache it
 *         was given as they were.
 */
static bool is_refused_at_line(const char* rows, ptrdiff_t line) {
  const struct SiteConfig site_config = make_site_config();
  struct Arena arena = test_arena();
  struct Buf text = {.arena = &arena};
  struct BuildCacheRow no_rows[1] = {0};
  buf_append(&text, format_rows(&site_config, no_rows, 0));
  buf_append(&text, str_from_cstr(rows));
  struct Arena perm = test_arena();
  const char* const perm_beg = perm.beg;
  struct BuildCache cache = {.count = 7};
  struct Log log = test_log();
  return build_cache_parse(buf_str(&text), &site_config, &perm, &cache, &log) == -1 &&
         test_log_is(&log, "build cache does not parse at line %td\n", line) &&
         perm.beg == perm_beg && cache.count == 7 && cache.rows == NULL;
}

// Every kind of row survives a format and a parse, sorted by kind and then by name, and an export
// row with nothing to export keeps its empty ids.
static void test_format_and_parse_round_trip(void) {
  const struct SiteConfig site_config = make_site_config();
  struct BuildCacheRow rows[] = {
      {.kind = BUILD_CACHE_KIND_PAGE, .name = S("one/tree/main/index.html"), .id = S("gg,hh")},
      {.kind = BUILD_CACHE_KIND_EXPORT,
       .name = S("one"),
       .id = S("aaaa"),
       .pack_len = 10,
       .idx_len = 20,
       .info_refs_id = S("bbbb"),
       .previous_pack_id = S("cccc")},
      {.kind = BUILD_CACHE_KIND_EXPORT, .name = S("empty"), .info_refs_id = S("dddd")},
      {.kind = BUILD_CACHE_KIND_RELEASES, .name = S("one"), .id = S("iiii")},
      {.kind = BUILD_CACHE_KIND_REF, .name = S("one/refs/heads/main"), .id = S("eeee")},
      {.kind = BUILD_CACHE_KIND_REPO, .name = S("one"), .id = S("ffff")},
      {.kind = BUILD_CACHE_KIND_REPO_MAP, .name = S("one"), .id = S("jjjj")},
      {.kind = BUILD_CACHE_KIND_STATIC,
       .name = S("style.css"),
       .len = 3,
       .mtime_s = 4,
       .mtime_ns = 5},
      {.kind = BUILD_CACHE_KIND_TEMPLATE,
       .name = S("index.html"),
       .len = 6,
       .mtime_s = 7,
       .mtime_ns = 8},
  };
  const struct Str text = format_rows(&site_config, rows, 9);
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(text, &site_config, &arena, &cache, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_ASSERT(cache.count == 9);
  TEST_CHECK(cache.rows[0].kind == BUILD_CACHE_KIND_TEMPLATE && cache.rows[0].len == 6 &&
             cache.rows[0].mtime_s == 7 && cache.rows[0].mtime_ns == 8);
  const struct BuildCacheRow* one = build_cache_find(&cache, BUILD_CACHE_KIND_EXPORT, S("one"));
  TEST_ASSERT(one != NULL);
  TEST_CHECK(str_eq(one->id, S("aaaa")) && one->pack_len == 10 && one->idx_len == 20);
  TEST_CHECK(str_eq(one->info_refs_id, S("bbbb")) && str_eq(one->previous_pack_id, S("cccc")));
  const struct BuildCacheRow* empty = build_cache_find(&cache, BUILD_CACHE_KIND_EXPORT, S("empty"));
  TEST_ASSERT(empty != NULL);
  TEST_CHECK(empty->id.len == 0 && empty->previous_pack_id.len == 0);
  TEST_CHECK(str_eq(build_cache_find(&cache, BUILD_CACHE_KIND_REF, S("one/refs/heads/main"))->id,
                    S("eeee")));
  TEST_CHECK(str_eq(build_cache_find(&cache, BUILD_CACHE_KIND_RELEASES, S("one"))->id, S("iiii")));
  // A repo map row shares its name with the repo row, and each kind finds its own.
  TEST_CHECK(str_eq(build_cache_find(&cache, BUILD_CACHE_KIND_REPO, S("one"))->id, S("ffff")));
  TEST_CHECK(str_eq(build_cache_find(&cache, BUILD_CACHE_KIND_REPO_MAP, S("one"))->id, S("jjjj")));
  // Page rows sort last, after every other kind.
  TEST_CHECK(cache.rows[8].kind == BUILD_CACHE_KIND_PAGE && str_eq(cache.rows[8].id, S("gg,hh")));
}

// A file dated before 1970 has a negative time, which its row keeps.
static void test_format_and_parse_keep_times_before_1970(void) {
  const struct SiteConfig site_config = make_site_config();
  struct BuildCacheRow rows[] = {
      {.kind = BUILD_CACHE_KIND_STATIC, .name = S("a.css"), .len = 3, .mtime_s = -4, .mtime_ns = 5},
  };
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(format_rows(&site_config, rows, 1), &site_config, &arena, &cache,
                                &log) == 0);
  const struct OsStat stat = {.len = 3, .mtime_s = -4, .mtime_ns = 5};
  TEST_CHECK(build_cache_has_stat(&cache, BUILD_CACHE_KIND_STATIC, S("a.css"), &stat));
}

// A stat row matches only the same size and modification time.
static void test_has_stat_compares_size_and_time(void) {
  const struct SiteConfig site_config = make_site_config();
  struct BuildCacheRow rows[] = {
      {.kind = BUILD_CACHE_KIND_STATIC, .name = S("a.css"), .len = 3, .mtime_s = 4, .mtime_ns = 5},
  };
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(format_rows(&site_config, rows, 1), &site_config, &arena, &cache,
                                &log) == 0);
  const struct OsStat same = {.len = 3, .mtime_s = 4, .mtime_ns = 5};
  const struct OsStat newer = {.len = 3, .mtime_s = 4, .mtime_ns = 6};
  TEST_CHECK(build_cache_has_stat(&cache, BUILD_CACHE_KIND_STATIC, S("a.css"), &same));
  TEST_CHECK(!build_cache_has_stat(&cache, BUILD_CACHE_KIND_STATIC, S("a.css"), &newer));
  TEST_CHECK(!build_cache_has_stat(&cache, BUILD_CACHE_KIND_TEMPLATE, S("a.css"), &same));
}

// The rows of one kind match only the same rows, no more and no fewer.
static void test_has_rows_compares_one_kind(void) {
  const struct SiteConfig site_config = make_site_config();
  struct BuildCacheRow rows[] = {
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/a"), .id = S("1")},
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/b"), .id = S("2")},
      {.kind = BUILD_CACHE_KIND_REPO, .name = S("r"), .id = S("3")},
  };
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(format_rows(&site_config, rows, 3), &site_config, &arena, &cache,
                                &log) == 0);
  const struct BuildCacheRow same[] = {
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/a"), .id = S("1")},
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/b"), .id = S("2")},
  };
  const struct BuildCacheRow moved[] = {
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/a"), .id = S("1")},
      {.kind = BUILD_CACHE_KIND_REF, .name = S("r/refs/heads/b"), .id = S("9")},
  };
  TEST_CHECK(build_cache_has_rows(&cache, BUILD_CACHE_KIND_REF, same, 2));
  TEST_CHECK(!build_cache_has_rows(&cache, BUILD_CACHE_KIND_REF, moved, 2));
  TEST_CHECK(!build_cache_has_rows(&cache, BUILD_CACHE_KIND_REF, same, 1));
  TEST_CHECK(!build_cache_has_rows(&cache, BUILD_CACHE_KIND_EXPORT, same, 1));
}

// A name with a tab or a newline gets no row, so nothing about it is ever kept.
static void test_format_leaves_out_names_with_separators(void) {
  const struct SiteConfig site_config = make_site_config();
  struct BuildCacheRow rows[] = {
      {.kind = BUILD_CACHE_KIND_STATIC, .name = S("a\tb"), .len = 1},
      {.kind = BUILD_CACHE_KIND_STATIC, .name = S("c\nd"), .len = 1},
  };
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_ASSERT(build_cache_parse(format_rows(&site_config, rows, 2), &site_config, &arena, &cache,
                                &log) == 0);
  TEST_CHECK(cache.count == 0);
}

// A tab, a newline or a `\` in a text of the recipe is escaped, so texts that would join to the
// same line do not, and a build cache of such texts still parses for its own config.
static void test_parse_tells_escaped_recipe_texts_apart(void) {
  struct SiteConfig tab = make_site_config();
  tab.title = S("Code\tdescription=X");
  tab.description = S("Y");
  struct SiteConfig other = make_site_config();
  other.title = S("Code");
  other.description = S("X\tdescription=Y");
  struct SiteConfig escaped = make_site_config();
  escaped.title = S("Code\\tdescription=X");
  escaped.description = S("Y");
  struct SiteConfig newline = make_site_config();
  newline.description = S("a\nb\\");
  struct BuildCacheRow rows[] = {{.kind = BUILD_CACHE_KIND_REPO, .name = S("r"), .id = S("1")}};
  const struct Str text = format_rows(&tab, rows, 1);
  TEST_CHECK(
      str_startswith(text, S("title=Code\\tdescription=X\tdescription=Y\tauthor=Ada\tbase_url=")));
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &tab, &arena, &cache, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(cache.count == 1);
  struct Log other_log = test_log();
  TEST_CHECK(build_cache_parse(text, &other, &arena, &cache, &other_log) == -1);
  TEST_CHECK(test_log_is(&other_log, "build cache was written for another page recipe\n"));
  struct Log escaped_log = test_log();
  TEST_CHECK(build_cache_parse(text, &escaped, &arena, &cache, &escaped_log) == -1);
  TEST_CHECK(test_log_is(&escaped_log, "build cache was written for another page recipe\n"));
  const struct Str newline_text = format_rows(&newline, rows, 1);
  TEST_CHECK(str_startswith(
      newline_text,
      S("title=Code\tdescription=a\\nb\\\\\tauthor=Ada\tbase_url=https://git.example.org\t")));
  struct Log newline_log = test_log();
  TEST_CHECK(build_cache_parse(newline_text, &newline, &arena, &cache, &newline_log) == 0);
  TEST_CHECK(test_log_is_empty(&newline_log));
  TEST_CHECK(cache.count == 1);
}

// A build cache of another recipe, such as another title or author, an empty one, and a row that
// does not parse are each refused, with the reason.
static void test_parse_rejects_unusable_files(void) {
  const struct SiteConfig site_config = make_site_config();
  struct SiteConfig other = make_site_config();
  other.title = S("Other");
  struct BuildCacheRow rows[] = {{.kind = BUILD_CACHE_KIND_REPO, .name = S("r"), .id = S("1")}};
  const struct Str text = format_rows(&other, rows, 1);
  struct Arena arena = test_arena();
  struct BuildCache cache = {0};
  struct Log log = test_log();
  TEST_CHECK(build_cache_parse(text, &site_config, &arena, &cache, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache was written for another page recipe\n"));
  struct SiteConfig other_author = make_site_config();
  other_author.author = S("Grace");
  struct Log author_log = test_log();
  TEST_CHECK(build_cache_parse(format_rows(&other_author, rows, 1), &site_config, &arena, &cache,
                               &author_log) == -1);
  TEST_CHECK(test_log_is(&author_log, "build cache was written for another page recipe\n"));
  struct Log empty_log = test_log();
  TEST_CHECK(build_cache_parse((struct Str){0}, &site_config, &arena, &cache, &empty_log) == -1);
  TEST_CHECK(
      test_log_is(&empty_log, "build cache is empty, as a build that did not finish leaves it\n"));
  struct Arena bad_arena = test_arena();
  struct Buf bad = {.arena = &bad_arena};
  buf_append(&bad, format_rows(&site_config, rows, 0));
  buf_append(&bad, S("repo\tr\n"));
  struct Log bad_log = test_log();
  TEST_CHECK(build_cache_parse(buf_str(&bad), &site_config, &arena, &cache, &bad_log) == -1);
  TEST_CHECK(test_log_is(&bad_log, "build cache does not parse at line 2\n"));
}

// A row that does not sort after the one before it, a duplicate included, is refused, because a
// lookup searches the rows by halves.
static void test_parse_rejects_rows_out_of_order(void) {
  TEST_CHECK(is_refused_at_line("repo\tb\t1\nrepo\ta\t2\n", 3));
  TEST_CHECK(is_refused_at_line("repo\ta\t1\nrepo\ta\t2\n", 3));
  TEST_CHECK(is_refused_at_line("ref\tr/main\t1\nrepo\tr\t2\n", 3));
}

// A last row without its newline is refused, as an interrupted write leaves it.
static void test_parse_rejects_an_unterminated_row(void) {
  TEST_CHECK(is_refused_at_line("repo\ta\t1", 2));
  TEST_CHECK(is_refused_at_line("repo\ta\t1\nrepo\tb\t2", 3));
}

// A size below zero, nanoseconds of a whole second or more, and a kind the format does not have are
// each refused.
static void test_parse_rejects_numbers_out_of_range_and_unknown_kinds(void) {
  TEST_CHECK(is_refused_at_line("template\tpage.html\t-1\t5\t0\n", 2));
  TEST_CHECK(is_refused_at_line("template\tpage.html\t10\t5\t1000000000\n", 2));
  TEST_CHECK(is_refused_at_line("template\tpage.html\t10\t5\t-1\n", 2));
  TEST_CHECK(is_refused_at_line("export\tr\t-\t-5\t0\t-\t-\n", 2));
  TEST_CHECK(is_refused_at_line("branch\tr\t1\n", 2));
}

TEST_LIST = {
    {"format and parse round trip", test_format_and_parse_round_trip},
    {"format and parse keep times before 1970", test_format_and_parse_keep_times_before_1970},
    {"has stat compares size and time", test_has_stat_compares_size_and_time},
    {"has rows compares one kind", test_has_rows_compares_one_kind},
    {"format leaves out names with separators", test_format_leaves_out_names_with_separators},
    {"parse tells escaped recipe texts apart", test_parse_tells_escaped_recipe_texts_apart},
    {"parse rejects unusable files", test_parse_rejects_unusable_files},
    {"parse rejects rows out of order", test_parse_rejects_rows_out_of_order},
    {"parse rejects an unterminated row", test_parse_rejects_an_unterminated_row},
    {"parse rejects numbers out of range and unknown kinds",
     test_parse_rejects_numbers_out_of_range_and_unknown_kinds},
    {NULL, NULL},
};
