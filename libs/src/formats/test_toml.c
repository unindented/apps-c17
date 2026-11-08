#include <acutest.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tomlc17.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "formats/toml.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "testing/test_support.h"

/**
 * @brief Parses a one-value TOML document.
 *
 * @param value      TOML value text assigned to key `x`.
 * @param result_out Parse result the caller must release with `toml_free`.
 * @return The datum bound to `x`, borrowing storage from `result_out`.
 */
static toml_datum_t parse_value(const char* value, toml_result_t* result_out) {
  char doc[128];
  const int n = snprintf(doc, sizeof(doc), "x = %s\n", value);
  TEST_ASSERT(n > 0 && n < (int)sizeof(doc));
  struct Log log = test_log();
  TEST_ASSERT(toml_parse_document((struct Str){doc, n}, S("test.toml"), result_out,
                                  S("failed to parse test"), &log) == 0);
  return toml_get(result_out->toptab, "x");
}

/**
 * @brief Parses a whole TOML document.
 *
 * @param document   Terminated TOML document text. Must not be `NULL`.
 * @param result_out Parse result the caller must release with `toml_free`. Must not be `NULL`.
 * @return The top-level table, borrowing storage from `result_out`.
 */
static toml_datum_t parse_table(const char* document, toml_result_t* result_out) {
  struct Log log = test_log();
  TEST_ASSERT(toml_parse_document(str_from_cstr(document), S("test.toml"), result_out,
                                  S("failed to parse test"), &log) == 0);
  return result_out->toptab;
}

/**
 * @brief Reports whether `toml_print_string` writes `text` as exactly `expected`.
 *
 * @param text     Text to quote.
 * @param expected Terminated text the quoted string must be. Must not be `NULL`.
 * @return `true` when the written text is `expected`.
 */
static bool is_printed_as(struct Str text, const char* expected) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  toml_print_string(&buffer, text);
  return test_text_is(buf_str(&buffer), "%s", expected);
}

// A parse error is logged once against the file, after the caller's operation and before tomlc17's
// own message, and the call returns `-1` with nothing left to free. The expected text came from a
// run.
static void test_parse_document_logs_parse_error(void) {
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_parse_document(S("x = \n"), S("site.toml"), &result, S("failed to parse config"),
                                 &log) == -1);
  TEST_CHECK(
      test_log_is_one(&log, "failed to parse config: (line 1) missing value (in 'site.toml')"));
}

// A plain string is text. A string carrying a `NUL` decoded from an escape is not, though its view
// still counts every byte. Every caller that turns a TOML string into a value checks this first.
// Without the escape case the check would pass for the wrong reason, since no `NUL` can appear
// literally in a TOML document.
static void test_is_text_rejects_embedded_nul(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("\"plain\"", &result);
  TEST_CHECK(toml_datum_is_text(value));
  TEST_CHECK(str_eq(toml_datum_str(value), S("plain")));
  toml_free(result);

  value = parse_value("\"pre\\u0000post\"", &result);
  TEST_CHECK(!toml_datum_is_text(value));
  TEST_CHECK(str_eq(toml_datum_str(value), S("pre\0post")));
  toml_free(result);

  // A non-string datum is not text either, so no caller reaches the copy path with an integer.
  TEST_CHECK(!toml_datum_is_text(parse_value("7", &result)));
  toml_free(result);
}

// The Unix epoch anchor converts to a zero timestamp.
static void test_unix_epoch_anchor_converts_to_zero(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("1970-01-01T00:00:00Z", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 0);
  toml_free(result);
}

// A date before the epoch converts to a negative count, which is the half of the documented signed
// range every other assertion in this file misses. All of them are at or after 1970, so a day count
// or epoch product narrowed to 32 bits would still pass them while `1900-01-01` wrapped. An app can
// sort generated output by the epoch alone, so a wrapped value would sort a 1900-dated entry as the
// newest one. Both expected values came from a run.
static void test_pre_epoch_dates_convert_to_negative(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("1969-12-31T23:59:59Z", &result);
  int64_t epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == -1);
  toml_free(result);

  // Below `INT32_MIN`, so a 32-bit narrowing anywhere on the path wraps this one.
  toml_result_t early_result;
  toml_datum_t early_value = parse_value("1900-01-01", &early_result);
  int64_t early_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(early_value, &early_epoch) == 0);
  TEST_CHECK(early_epoch == -2208988800);
  toml_free(early_result);
}

// `2000-02-29` is a leap day in a century year, the case `days_from_civil`'s hand-rolled
// `yoe / 4 - yoe / 100` era arithmetic gets wrong if the 400-year rule is dropped. The expected
// epoch came from a run, not from prediction.
static void test_leap_day_converts_in_century_year(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("2000-02-29T00:00:00Z", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 951782400);
  toml_free(result);

  // The two dates below exercise the conversion's `yoe / 100` century correction. `2000` cannot:
  // its year-of-era is `0`, so that term is `0` however it is spelled. The assertion above passes
  // with the correction removed. A year-of-era at or above `100` is what separates them, and
  // neither of these is a leap year in the Gregorian rule that the correction implements. An app
  // can sort generated output by the epoch alone, so a wrong one silently reorders entries rather
  // than failing anything. Both values came from a run.
  toml_result_t yoe_300;
  toml_datum_t yoe_300_value = parse_value("1900-03-01T00:00:00Z", &yoe_300);
  int64_t yoe_300_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(yoe_300_value, &yoe_300_epoch) == 0);
  TEST_CHECK(yoe_300_epoch == -2203891200);
  toml_free(yoe_300);

  toml_result_t yoe_100;
  toml_datum_t yoe_100_value = parse_value("2100-03-01T00:00:00Z", &yoe_100);
  int64_t yoe_100_epoch = 0;
  TEST_CHECK(toml_datum_to_epoch(yoe_100_value, &yoe_100_epoch) == 0);
  TEST_CHECK(yoe_100_epoch == 4107542400);
  toml_free(yoe_100);
}

// A date-only value normalizes to midnight UTC, both as an epoch and as a formatted string.
static void test_date_only_normalizes_to_midnight_utc(void) {
  struct Arena arena = test_arena();
  toml_result_t result;
  toml_datum_t value = parse_value("2030-01-02", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == 0);
  TEST_CHECK(epoch == 1893542400);
  const struct Str text = toml_datum_format_rfc3339(value, &arena);
  TEST_CHECK(str_eq(text, S("2030-01-02T00:00:00Z")));
  toml_free(result);
}

// A UTC datetime round-trips unchanged.
static void test_utc_datetime_round_trips(void) {
  struct Arena arena = test_arena();
  toml_result_t result;
  toml_datum_t value = parse_value("2026-07-01T12:00:00Z", &result);
  const struct Str text = toml_datum_format_rfc3339(value, &arena);
  TEST_CHECK(str_eq(text, S("2026-07-01T12:00:00Z")));
  toml_free(result);
}

// Negative and positive offsets convert to the same UTC instant.
static void test_offsets_convert_to_same_utc_instant(void) {
  struct Arena arena = test_arena();
  toml_result_t neg;
  toml_result_t pos;
  toml_result_t utc;
  toml_datum_t neg_value = parse_value("2026-07-05T09:00:00-07:00", &neg);
  toml_datum_t pos_value = parse_value("2026-07-05T18:00:00+02:00", &pos);
  toml_datum_t utc_value = parse_value("2026-07-05T16:00:00Z", &utc);

  int64_t neg_epoch = -1;
  int64_t pos_epoch = -1;
  int64_t utc_epoch = -1;
  const struct Str neg_text = toml_datum_format_rfc3339(neg_value, &arena);
  const struct Str pos_text = toml_datum_format_rfc3339(pos_value, &arena);
  TEST_CHECK(toml_datum_to_epoch(neg_value, &neg_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(pos_value, &pos_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(utc_value, &utc_epoch) == 0);

  TEST_CHECK(neg_epoch == utc_epoch);
  TEST_CHECK(pos_epoch == utc_epoch);
  TEST_CHECK(str_eq(neg_text, S("2026-07-05T09:00:00-07:00")));
  TEST_CHECK(str_eq(pos_text, S("2026-07-05T18:00:00+02:00")));

  toml_free(neg);
  toml_free(pos);
  toml_free(utc);
}

// Fractional seconds are preserved (trailing zeros trimmed) and truncated for the epoch.
static void test_fractional_seconds_preserved(void) {
  struct Arena arena = test_arena();
  toml_result_t half;
  toml_result_t micros;
  toml_result_t trailing;
  toml_result_t whole;
  toml_datum_t half_value = parse_value("2026-07-01T12:00:00.5Z", &half);
  toml_datum_t micros_value = parse_value("2026-07-01T12:00:00.123456Z", &micros);
  toml_datum_t trailing_value = parse_value("2026-07-01T12:00:00.120000Z", &trailing);
  toml_datum_t whole_value = parse_value("2026-07-01T12:00:00Z", &whole);

  const struct Str half_text = toml_datum_format_rfc3339(half_value, &arena);
  const struct Str micros_text = toml_datum_format_rfc3339(micros_value, &arena);
  const struct Str trailing_text = toml_datum_format_rfc3339(trailing_value, &arena);
  TEST_CHECK(str_eq(half_text, S("2026-07-01T12:00:00.5Z")));
  TEST_CHECK(str_eq(micros_text, S("2026-07-01T12:00:00.123456Z")));
  TEST_CHECK(str_eq(trailing_text, S("2026-07-01T12:00:00.12Z")));

  // A fraction of all zeros yields no fractional part at all, not a bare `.`. That is a separate
  // path from the trimming above rather than its extreme: `toml_datum_format_rfc3339` prints no `.`
  // and no digits for a zero microsecond count, so `.12` cannot reach it. A date written with an
  // explicit `.000000` is the reachable way in.
  toml_result_t zeros;
  toml_datum_t zeros_value = parse_value("2026-07-01T12:00:00.000000Z", &zeros);
  const struct Str zeros_text = toml_datum_format_rfc3339(zeros_value, &arena);
  TEST_CHECK(str_eq(zeros_text, S("2026-07-01T12:00:00Z")));
  toml_free(zeros);

  // The epoch drops sub-second precision, so fractional value matches its whole-second sibling.
  int64_t half_epoch = -1;
  int64_t whole_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(half_value, &half_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(whole_value, &whole_epoch) == 0);
  TEST_CHECK(half_epoch == whole_epoch);

  toml_free(half);
  toml_free(micros);
  toml_free(trailing);
  toml_free(whole);
}

// A timezone-less datetime is treated as UTC: emitted with `Z` and converted as the UTC instant.
static void test_timezoneless_datetime_treated_as_utc(void) {
  struct Arena arena = test_arena();
  toml_result_t local;
  toml_result_t utc;
  toml_datum_t local_value = parse_value("2026-07-01T12:00:00", &local);
  toml_datum_t utc_value = parse_value("2026-07-01T12:00:00Z", &utc);

  const struct Str text = toml_datum_format_rfc3339(local_value, &arena);
  TEST_CHECK(str_eq(text, S("2026-07-01T12:00:00Z")));

  int64_t local_epoch = -1;
  int64_t utc_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(local_value, &local_epoch) == 0);
  TEST_CHECK(toml_datum_to_epoch(utc_value, &utc_epoch) == 0);
  TEST_CHECK(local_epoch == utc_epoch);

  toml_free(local);
  toml_free(utc);
}

// A datum that is neither a date nor a datetime is rejected by the epoch conversion, which every
// caller runs before formatting the datum. The test uses two forms with different failure modes. A
// string populates `u.str` and cannot produce a plausible timestamp. A time-only datum uses the
// same `u.ts` payload as accepted types. Only its `year`, `month`, and `day` fields differ; each
// contains `-1`. Adding `TOML_TIME` to the accepted set is a plausible slip. It would turn
// `date = 12:00:00` into a garbage epoch and an `-001--1--1T12:00:00Z` timestamp in the page and
// the feed instead of a diagnostic.
static void test_rejects_non_datetime_values(void) {
  toml_result_t result;
  toml_datum_t value = parse_value("\"not a date\"", &result);
  int64_t epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(value, &epoch) == -1);
  // `epoch_out` is written only on success, so the sentinel set above survives. A caller keeping a
  // previously computed epoch in the same variable across a failed call depends on this.
  TEST_CHECK(epoch == -1);
  toml_free(result);

  toml_result_t time_result;
  toml_datum_t time_value = parse_value("12:00:00", &time_result);
  int64_t time_epoch = -1;
  TEST_CHECK(toml_datum_to_epoch(time_value, &time_epoch) == -1);
  TEST_CHECK(time_epoch == -1);
  toml_free(time_result);
}

// A table holding only listed keys passes, whatever their order, and so does an empty table checked
// against an empty list. Neither records anything.
static void test_require_known_keys_accepts_known_keys(void) {
  static const struct Str known_keys[] = {STR_INIT("title"), STR_INIT("date")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ date = 1, title = 2 }", &result), known_keys,
                                     sizeof(known_keys) / sizeof(known_keys[0]), S("config"), S(""),
                                     (struct Str){0}, &log) == 0);
  toml_free(result);

  TEST_CHECK(toml_require_known_keys(parse_value("{}", &result), NULL, 0, S("config"), S(""),
                                     (struct Str){0}, &log) == 0);
  toml_free(result);
  TEST_CHECK(test_log_is_empty(&log));
}

// An unlisted key is rejected, named with the caller's key kind and table prefix. Keys match
// exactly, so a prefix of a known key, or one extending it, is as unknown as a typo, and an empty
// list rejects every key.
static void test_require_known_keys_rejects_unknown_keys(void) {
  static const struct Str known_keys[] = {STR_INIT("title"), STR_INIT("date")};
  static const char* const cases[][4] = {
      {"{ title = 1, ttile = 2 }", "config", "", "unknown config key 'ttile'\n"},
      {"{ titl = 1 }", "frontmatter", "", "unknown frontmatter key 'titl'\n"},
      {"{ titles = 1 }", "frontmatter", "", "unknown frontmatter key 'titles'\n"},
      {"{ titel = true }", "frontmatter", "extra.", "unknown frontmatter key 'extra.titel'\n"},
  };

  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    toml_result_t result;
    TEST_CHECK(toml_require_known_keys(parse_value(cases[i][0], &result), known_keys,
                                       sizeof(known_keys) / sizeof(known_keys[0]),
                                       str_from_cstr(cases[i][1]), str_from_cstr(cases[i][2]),
                                       (struct Str){0}, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][3]));
    TEST_MSG("case %td", i);
    toml_free(result);
  }

  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ title = 1 }", &result), NULL, 0, S("config"),
                                     S(""), (struct Str){0}, &log) == -1);
  TEST_CHECK(test_log_is(&log, "unknown config key 'title'\n"));
  toml_free(result);
}

// A quoted key holding a `NUL` decoded from an escape is rejected without being named, since `%.*s`
// would stop at the `NUL` and name the known key in front of it. Each such key is still one unknown
// key, beside an ordinary unknown key that keeps its name.
static void test_require_known_keys_rejects_nul_key_unnamed(void) {
  static const struct Str known_keys[] = {STR_INIT("title")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ \"title\\u0000x\" = 1, ttile = 2 }", &result),
                                     known_keys, sizeof(known_keys) / sizeof(known_keys[0]),
                                     S("frontmatter"), S(""), S("content/a.md"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "frontmatter key must not contain a NUL byte (in 'content/a.md')\n"
                         "unknown frontmatter key 'ttile' (in 'content/a.md')\n"));
  TEST_CHECK(log.errors == 2);
  toml_free(result);
}

// A `NUL` key in a nested table is still not named, but its table is, without the `.` of the
// prefix.
static void test_require_known_keys_names_table_of_nul_key(void) {
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ \"x\\u0000\" = 1 }", &result), NULL, 0,
                                     S("config"), S("table.s."), S("app.toml"), &log) == -1);
  TEST_CHECK(test_log_is(
      &log, "config key under 'table.s' must not contain a NUL byte (in 'app.toml')\n"));
  toml_free(result);
}

// Every unknown key is reported, in table order, each naming the file when one is given.
static void test_require_known_keys_reports_every_unknown_key(void) {
  static const struct Str known_keys[] = {STR_INIT("title")};
  struct Log log = test_log();
  toml_result_t result;
  TEST_CHECK(toml_require_known_keys(parse_value("{ ttile = 1, title = 2, dtae = 3 }", &result),
                                     known_keys, sizeof(known_keys) / sizeof(known_keys[0]),
                                     S("frontmatter"), S(""), S("content/a.md"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "unknown frontmatter key 'ttile' (in 'content/a.md')\n"
                         "unknown frontmatter key 'dtae' (in 'content/a.md')\n"));
  TEST_CHECK(log.errors == 2);
  toml_free(result);
}

// A present string is copied into the arena, so it outlives the parse, and an empty one is a copy
// whose `data` is not `NULL`, which tells it apart from an absent key. An absent optional key
// leaves the value as it was.
static void test_copy_string_copies_present_and_keeps_absent(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  toml_result_t result;
  const toml_datum_t table = parse_table("title = \"Code\"\nempty = \"\"\n", &result);
  struct Str title = {0};
  struct Str empty = {0};
  struct Str absent = S("kept");
  TEST_CHECK(toml_copy_string(table, S(""), "title", true, &arena, &title, &log) == 0);
  TEST_CHECK(toml_copy_string(table, S(""), "empty", true, &arena, &empty, &log) == 0);
  TEST_CHECK(toml_copy_string(table, S(""), "absent", false, &arena, &absent, &log) == 0);
  toml_free(result);
  TEST_CHECK(str_eq(title, S("Code")));
  TEST_CHECK(empty.data != NULL && empty.len == 0);
  TEST_CHECK(str_eq(absent, S("kept")));
  TEST_CHECK(test_log_is_empty(&log));
}

// A missing required key, a value of another type and a value with a `NUL` byte each fail with
// their own message, which names the key after the table's prefix, and none stores a value.
static void test_copy_string_rejects_missing_mistyped_and_nul(void) {
  static const char* const cases[][3] = {
      {"", "", "missing required config key 'title'\n"},
      {"", "repo[0].", "missing required config key 'repo[0].title'\n"},
      {"title = 7\n", "", "config key 'title' must be a string\n"},
      {"title = 7\n", "repo[0].", "config key 'repo[0].title' must be a string\n"},
      {"title = \"a\\u0000b\"\n", "", "config key 'title' must not contain a NUL byte\n"},
      {"title = \"a\\u0000b\"\n", "repo[0].",
       "config key 'repo[0].title' must not contain a NUL byte\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    struct Log log = test_log();
    toml_result_t result;
    struct Str value = S("kept");
    TEST_CHECK(toml_copy_string(parse_table(cases[i][0], &result), str_from_cstr(cases[i][1]),
                                "title", true, &arena, &value, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][2]));
    TEST_CHECK(str_eq(value, S("kept")));
    TEST_MSG("case %td", i);
    toml_free(result);
  }
}

// A directory loses its trailing `/` run and resolves against the config's directory, unless it is
// absolute or the config is in the working directory. An absent optional key keeps the value.
static void test_copy_dir_trims_and_resolves(void) {
  static const char* const cases[][3] = {
      {"dir = \"public/\"\n", "sub", "sub/public"},
      {"dir = \"out//\"\n", "", "out"},
      {"dir = \"../site\"\n", "sub", "sub/../site"},
      {"dir = \"/srv/public/\"\n", "sub", "/srv/public"},
      {"dir = \"public\"\n", "/", "/public"},
      {"dir = \"public\"\n", ".", "public"},
      {"", "sub", "kept"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    struct Log log = test_log();
    toml_result_t result;
    struct Str dir = S("kept");
    TEST_CHECK(toml_copy_dir(parse_table(cases[i][0], &result), S(""), "dir", false,
                             str_from_cstr(cases[i][1]), &arena, &dir, &log) == 0);
    toml_free(result);
    TEST_CHECK(test_text_is(dir, "%s", cases[i][2]));
    TEST_CHECK(test_log_is_empty(&log));
    TEST_MSG("case %td", i);
  }
}

// An empty directory, and one that is only `/`, trims to empty and is rejected, as are a missing
// required key and a value that `toml_copy_string` rejects. Each message names the key after the
// table's prefix, and a rejected key keeps the value it had.
static void test_copy_dir_rejects_empty_and_missing(void) {
  static const char* const cases[][2] = {
      {"dir = \"\"\n", "config key 'repo[0].dir' must not be empty\n"},
      {"dir = \"/\"\n", "config key 'repo[0].dir' must not be empty\n"},
      {"dir = \"///\"\n", "config key 'repo[0].dir' must not be empty\n"},
      {"", "missing required config key 'repo[0].dir'\n"},
      {"dir = true\n", "config key 'repo[0].dir' must be a string\n"},
      {"dir = \"a\\u0000\"\n", "config key 'repo[0].dir' must not contain a NUL byte\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    struct Log log = test_log();
    toml_result_t result;
    struct Str dir = S("kept");
    TEST_CHECK(toml_copy_dir(parse_table(cases[i][0], &result), S("repo[0]."), "dir", true,
                             S("sub"), &arena, &dir, &log) == -1);
    toml_free(result);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][1]));
    TEST_CHECK(str_eq(dir, S("kept")));
    TEST_MSG("case %td", i);
  }
}

// A boolean is copied, an absent key keeps its value, and a value of another type is rejected,
// named after the table's prefix, and keeps the value too.
static void test_copy_bool_copies_or_rejects(void) {
  struct Log log = test_log();
  toml_result_t result;
  bool value = false;
  TEST_CHECK(toml_copy_bool(parse_table("flag = true\n", &result), S(""), "flag", &value, &log) ==
             0);
  toml_free(result);
  TEST_CHECK(value);
  TEST_CHECK(toml_copy_bool(parse_table("", &result), S(""), "flag", &value, &log) == 0);
  toml_free(result);
  TEST_CHECK(value);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(toml_copy_bool(parse_table("flag = 1\n", &result), S("t."), "flag", &value, &log) ==
             -1);
  toml_free(result);
  TEST_CHECK(value);
  TEST_CHECK(test_log_is(&log, "config key 't.flag' must be a boolean\n"));
}

// A safe relative template name is copied, nested in a directory or not, and an absent key keeps
// the default.
static void test_copy_template_name_copies_present_and_keeps_absent(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  toml_result_t result;
  const toml_datum_t table =
      parse_table("tree = \"tree.html\"\ncommit = \"pages/commit-page.html\"\n", &result);
  struct Str tree = {0};
  struct Str commit = {0};
  struct Str absent = S("blob.html");
  TEST_CHECK(toml_copy_template_name(table, S("templates."), "tree", &arena, &tree, &log) == 0);
  TEST_CHECK(toml_copy_template_name(table, S("templates."), "commit", &arena, &commit, &log) == 0);
  TEST_CHECK(toml_copy_template_name(table, S("templates."), "blob", &arena, &absent, &log) == 0);
  toml_free(result);
  TEST_CHECK(test_text_is(tree, "tree.html"));
  TEST_CHECK(test_text_is(commit, "pages/commit-page.html"));
  TEST_CHECK(test_text_is(absent, "blob.html"));
  TEST_CHECK(test_log_is_empty(&log));
}

// An empty name, an unsafe name, a value of another type and a value with a `NUL` byte each fail
// with one message that names the key after the table's prefix, and none stores a value.
static void test_copy_template_name_rejects_empty_unsafe_and_mistyped(void) {
  static const char* const cases[][3] = {
      {"name = \"\"\n", "templates.",
       "config key 'templates.name' must be a safe relative template name: ''\n"},
      {"name = \"../x.html\"\n", "templates.",
       "config key 'templates.name' must be a safe relative template name: '../x.html'\n"},
      {"name = \"/x.html\"\n", "",
       "config key 'name' must be a safe relative template name: '/x.html'\n"},
      {"name = \"a b.html\"\n", "t.",
       "config key 't.name' must be a safe relative template name: 'a b.html'\n"},
      {"name = 7\n", "templates.", "config key 'templates.name' must be a string\n"},
      {"name = [\"x.html\"]\n", "templates.", "config key 'templates.name' must be a string\n"},
      {"name = \"a\\u0000b\"\n", "templates.",
       "config key 'templates.name' must not contain a NUL byte\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    struct Log log = test_log();
    toml_result_t result;
    struct Str value = S("kept");
    TEST_CHECK(toml_copy_template_name(parse_table(cases[i][0], &result),
                                       str_from_cstr(cases[i][1]), "name", &arena, &value,
                                       &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][2]));
    TEST_CHECK(str_eq(value, S("kept")));
    TEST_MSG("case %td", i);
    toml_free(result);
  }
}

// An array of strings is copied in order, empty included, an absent key keeps the list it had, and
// a list of template names accepts only safe relative names.
static void test_copy_strings_copies_each_string(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  toml_result_t result;
  const struct Str kept[] = {STR_INIT("kept")};
  const struct Str* items = kept;
  ptrdiff_t item_count = 1;
  TEST_CHECK(toml_copy_strings(parse_table("names = [\"a.html\", \"b/c.html\"]\n", &result), S(""),
                               "names", true, &arena, &items, &item_count, &log) == 0);
  toml_free(result);
  TEST_ASSERT(item_count == 2);
  TEST_CHECK(test_text_is(items[0], "a.html"));
  TEST_CHECK(test_text_is(items[1], "b/c.html"));
  TEST_CHECK(toml_copy_strings(parse_table("names = []\n", &result), S(""), "names", true, &arena,
                               &items, &item_count, &log) == 0);
  toml_free(result);
  TEST_CHECK(items != NULL && item_count == 0);
  items = kept;
  item_count = 1;
  TEST_CHECK(toml_copy_strings(parse_table("", &result), S(""), "names", true, &arena, &items,
                               &item_count, &log) == 0);
  toml_free(result);
  TEST_CHECK(items == kept && item_count == 1);
  TEST_CHECK(toml_copy_strings(parse_table("keys = [\"../x\"]\n", &result), S(""), "keys", false,
                               &arena, &items, &item_count, &log) == 0);
  toml_free(result);
  TEST_CHECK(item_count == 1 && test_text_is(items[0], "../x"));
  TEST_CHECK(test_log_is_empty(&log));
}

// A value that is not an array, an element that is not a string or holds a `NUL`, and an unsafe
// template name are rejected. A wrong type or a `NUL` is reported once, each unsafe name once,
// every message names the key after the table's prefix, and a rejected key keeps the list it had.
static void test_copy_strings_rejects_bad_elements(void) {
  struct Arena arena = test_arena();
  struct Log log = test_log();
  toml_result_t result;
  const struct Str* items = NULL;
  ptrdiff_t item_count = 0;
  TEST_CHECK(toml_copy_strings(parse_table("names = \"a\"\n", &result), S("t."), "names", true,
                               &arena, &items, &item_count, &log) == -1);
  toml_free(result);
  TEST_CHECK(
      toml_copy_strings(
          parse_table("names = [1, 2, \"a\\u0000b\", \"c\\u0000\", \"../x\", \"/y\"]\n", &result),
          S("t."), "names", true, &arena, &items, &item_count, &log) == -1);
  toml_free(result);
  TEST_CHECK(items == NULL && item_count == 0);
  TEST_CHECK(test_log_is(&log,
                         "config key 't.names' must be an array\n"
                         "config key 't.names' must contain only strings\n"
                         "config key 't.names' must not contain a NUL byte\n"
                         "config key 't.names' must contain only safe relative template names: "
                         "'../x'\n"
                         "config key 't.names' must contain only safe relative template names: "
                         "'/y'\n"));
}

// A base URL is copied without its trailing `/`, and an optional one may be absent or empty, which
// means none.
static void test_copy_base_url_trims_and_accepts_an_empty_optional_url(void) {
  toml_result_t result;
  const toml_datum_t table =
      parse_table("url = \"https://example.com/notes//\"\nempty = \"\"\n", &result);
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct Str url = {0};
  TEST_CHECK(toml_copy_base_url(table, S(""), "url", true, &arena, &url, &log) == 0);
  TEST_CHECK(str_eq(url, S("https://example.com/notes")));
  struct Str empty = S("kept");
  TEST_CHECK(toml_copy_base_url(table, S(""), "empty", false, &arena, &empty, &log) == 0);
  TEST_CHECK(empty.len == 0);
  struct Str absent = S("kept");
  TEST_CHECK(toml_copy_base_url(table, S(""), "absent", false, &arena, &absent, &log) == 0);
  TEST_CHECK(str_eq(absent, S("kept")));
  TEST_CHECK(test_log_is_empty(&log));
  toml_free(result);
}

// A base URL that is not an absolute `http` URL with a host, or that holds a byte that would break
// the path a link appends, is reported with its key and value and never stored, and so is a
// required one that is empty. A mistyped one, one with a NUL byte and a missing required one are
// reported once, as `toml_copy_string` reports them.
static void test_copy_base_url_rejects_an_unusable_url(void) {
  toml_result_t result;
  const toml_datum_t table = parse_table(
      "ftp = \"ftp://example.com\"\nquery = \"https://example.com/?a\"\nempty = \"\"\n"
      "number = 1\nnul = \"https://a\\u0000b\"\n",
      &result);
  struct Arena arena = test_arena();
  struct Log log = test_log();
  struct Str url = S("kept");
  TEST_CHECK(toml_copy_base_url(table, S("site."), "ftp", true, &arena, &url, &log) == -1);
  TEST_CHECK(toml_copy_base_url(table, S("site."), "query", true, &arena, &url, &log) == -1);
  TEST_CHECK(toml_copy_base_url(table, S("site."), "empty", true, &arena, &url, &log) == -1);
  TEST_CHECK(toml_copy_base_url(table, S("site."), "number", true, &arena, &url, &log) == -1);
  TEST_CHECK(toml_copy_base_url(table, S("site."), "nul", true, &arena, &url, &log) == -1);
  TEST_CHECK(toml_copy_base_url(table, S("site."), "absent", true, &arena, &url, &log) == -1);
  TEST_CHECK(test_log_is(
      &log,
      "config key 'site.ftp' must be an absolute 'http://' or 'https://' URL with a host: "
      "'ftp://example.com'\n"
      "config key 'site.query' must not hold '?', '#', a space or a control character: "
      "'https://example.com/?a'\n"
      "config key 'site.empty' must be an absolute 'http://' or 'https://' URL with a host: ''\n"
      "config key 'site.number' must be a string\n"
      "config key 'site.nul' must not contain a NUL byte\n"
      "missing required config key 'site.absent'\n"));
  TEST_CHECK(str_eq(url, S("kept")));
  toml_free(result);
}

// A config file is read and parsed, and one that is missing, too large or not TOML is reported with
// the read's or the parse's message.
static void test_load_config_reads_and_parses_the_file(void) {
  struct Os* os = os_memory();
  os_memory_write(os, "site.toml", "title = \"Notes\"\n");
  toml_result_t result;
  struct Log log = test_log();
  TEST_ASSERT(toml_load_config(S("site.toml"), os, test_arena(), &result, &log) == 0);
  TEST_CHECK(str_eq(toml_datum_str(toml_get(result.toptab, "title")), S("Notes")));
  toml_free(result);

  TEST_CHECK(toml_load_config(S("gone.toml"), os, test_arena(), &result, &log) == -1);
  TEST_CHECK(test_log_is(&log, "failed to read config: %s ('gone.toml')\n", strerror(ENOENT)));
  os_memory_write(os, "bad.toml", "title = \n");
  log = test_log();
  TEST_CHECK(toml_load_config(S("bad.toml"), os, test_arena(), &result, &log) == -1);
  TEST_CHECK(str_startswith(buf_str(&log.buf), S("failed to parse config: ")));
}

// A value at either bound is accepted, and an absent optional key leaves the field as it was.
static void test_copy_size_accepts_bounds_and_keeps_absent(void) {
  struct Log log = test_log();
  toml_result_t result;
  const toml_datum_t table = parse_table("low = 1\nhigh = 10\n", &result);
  ptrdiff_t low = 0;
  ptrdiff_t high = 0;
  ptrdiff_t absent = 5;
  const struct TomlSizeKey specs[] = {
      {.key = "low", .field = &low, .value_min = 1, .value_max = 10, .limit_name = "count"},
      {.key = "high", .field = &high, .value_min = 1, .value_max = 10, .limit_name = "count"},
      {.key = "absent", .field = &absent, .value_min = 1, .value_max = 10, .limit_name = "count"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(specs) / sizeof(specs[0])); i++) {
    TEST_CHECK(toml_copy_size(table, S(""), &specs[i], &log) == 0);
  }
  toml_free(result);
  TEST_CHECK(low == 1);
  TEST_CHECK(high == 10);
  TEST_CHECK(absent == 5);
  TEST_CHECK(test_log_is_empty(&log));
}

// Each rejection has its own message, which names the key after the table's prefix, and a length
// names its bound and its value in bytes. A rejected value leaves the field as it was.
static void test_copy_size_rejects_out_of_bounds(void) {
  static const struct {
    const char* document;
    bool is_required;
    bool is_len;
    const char* expected;
  } cases[] = {
      {"", true, false, "missing required config key 'limits.size'\n"},
      {"size = \"7\"\n", false, false, "config key 'limits.size' must be an integer\n"},
      {"size = 1.5\n", false, false, "config key 'limits.size' must be an integer\n"},
      {"size = 1\n", false, false, "config key 'limits.size' must be at least 2: 1\n"},
      {"size = -1\n", false, true, "config key 'limits.size' must be at least 2: -1\n"},
      {"size = 11\n", false, false, "config key 'limits.size' exceeds max size limit (10) at 11\n"},
      {"size = 11\n", false, true,
       "config key 'limits.size' exceeds max size limit (10 bytes) at 11 bytes\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    toml_result_t result;
    ptrdiff_t size = 5;
    const struct TomlSizeKey spec = {
        .key = "size",
        .field = &size,
        .value_min = 2,
        .value_max = 10,
        .limit_name = "size limit",
        .is_required = cases[i].is_required,
        .is_len = cases[i].is_len,
    };
    TEST_CHECK(toml_copy_size(parse_table(cases[i].document, &result), S("limits."), &spec, &log) ==
               -1);
    toml_free(result);
    TEST_CHECK(test_log_is(&log, "%s", cases[i].expected));
    TEST_CHECK(size == 5);
    TEST_MSG("case %td", i);
  }
}

// Plain text and UTF-8 pass through inside the quotes, while `"` and `\` are escaped.
static void test_print_string_quotes_and_escapes_delimiters(void) {
  TEST_CHECK(is_printed_as(S(""), "\"\""));
  TEST_CHECK(is_printed_as(S("caf\xc3\xa9 ok"), "\"caf\xc3\xa9 ok\""));
  TEST_CHECK(is_printed_as(S("a\"b\\c"), "\"a\\\"b\\\\c\""));
}

// A control byte with a short TOML escape takes it, and every other control byte below `0x20`, and
// `0x7F`, takes `\uXXXX`, so the printed string is valid TOML for any bytes a config held.
static void test_print_string_escapes_control_bytes(void) {
  TEST_CHECK(is_printed_as(S("\b\t\n\f\r"), "\"\\b\\t\\n\\f\\r\""));
  TEST_CHECK(is_printed_as(S("\x01\x1f\x7f"), "\"\\u0001\\u001F\\u007F\""));
  TEST_CHECK(is_printed_as(S("a\0b"), "\"a\\u0000b\""));
}

// Every control byte below `0x20`, and `0x7F`, prints as an escape that tomlc17 reads back as the
// same byte, so a printed config loads to the value it held.
static void test_print_string_round_trips_every_control_byte(void) {
  for (int byte = 0; byte <= 0x7F; byte++) {
    if (byte >= 0x20 && byte < 0x7F) {
      continue;
    }
    struct Arena arena = test_arena();
    struct Buf buffer = {.arena = &arena};
    const char text[] = {'a', (char)byte, 'b'};
    buf_append(&buffer, S("value = "));
    toml_print_string(&buffer, (struct Str){text, 3});
    buf_append_char(&buffer, '\n');
    toml_result_t result;
    const toml_datum_t table = parse_table(buf_cstr(&buffer), &result);
    const toml_datum_t value = toml_get(table, "value");
    TEST_CHECK(value.type == TOML_STRING && value.u.str.len == 3 &&
               memcmp(value.u.str.ptr, text, 3) == 0);
    TEST_MSG("byte %d", byte);
    toml_free(result);
  }
}

// A key line quotes its value, an optional one writes nothing for an empty value, and an array line
// quotes each item the same way, including an empty array with no items.
static void test_print_key_lines(void) {
  static const struct Str items[] = {STR_INIT("index.html"), STR_INIT("a\"b")};
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  toml_print_key_string(&buffer, "title", S("Code\tbase"));
  toml_print_key_string(&buffer, "empty", S(""));
  toml_print_key_string_optional(&buffer, "absent", S(""));
  toml_print_key_string_optional(&buffer, "present", S("x"));
  toml_print_key_string_array(&buffer, "items", items, sizeof(items) / sizeof(items[0]));
  toml_print_key_string_array(&buffer, "none", NULL, 0);
  TEST_CHECK(test_text_is(buf_str(&buffer), "%s",
                          "title = \"Code\\tbase\"\n"
                          "empty = \"\"\n"
                          "present = \"x\"\n"
                          "items = [\"index.html\", \"a\\\"b\"]\n"
                          "none = []\n"));
}

TEST_LIST = {
    {"parse document logs parse error", test_parse_document_logs_parse_error},
    {"is text rejects embedded nul", test_is_text_rejects_embedded_nul},
    {"unix epoch anchor converts to zero", test_unix_epoch_anchor_converts_to_zero},
    {"pre epoch dates convert to negative", test_pre_epoch_dates_convert_to_negative},
    {"leap day converts in century year", test_leap_day_converts_in_century_year},
    {"date only normalizes to midnight utc", test_date_only_normalizes_to_midnight_utc},
    {"utc datetime round trips", test_utc_datetime_round_trips},
    {"offsets convert to same utc instant", test_offsets_convert_to_same_utc_instant},
    {"fractional seconds preserved", test_fractional_seconds_preserved},
    {"timezoneless datetime treated as utc", test_timezoneless_datetime_treated_as_utc},
    {"rejects non-datetime values", test_rejects_non_datetime_values},
    {"require known keys accepts known keys", test_require_known_keys_accepts_known_keys},
    {"require known keys rejects unknown keys", test_require_known_keys_rejects_unknown_keys},
    {"require known keys rejects nul key unnamed", test_require_known_keys_rejects_nul_key_unnamed},
    {"require known keys names table of nul key", test_require_known_keys_names_table_of_nul_key},
    {"require known keys reports every unknown key",
     test_require_known_keys_reports_every_unknown_key},
    {"copy string copies present and keeps absent",
     test_copy_string_copies_present_and_keeps_absent},
    {"copy string rejects missing mistyped and nul",
     test_copy_string_rejects_missing_mistyped_and_nul},
    {"copy dir trims and resolves", test_copy_dir_trims_and_resolves},
    {"copy dir rejects empty and missing", test_copy_dir_rejects_empty_and_missing},
    {"copy bool copies or rejects", test_copy_bool_copies_or_rejects},
    {"copy template name copies present and keeps absent",
     test_copy_template_name_copies_present_and_keeps_absent},
    {"copy template name rejects empty unsafe and mistyped",
     test_copy_template_name_rejects_empty_unsafe_and_mistyped},
    {"copy strings copies each string", test_copy_strings_copies_each_string},
    {"copy strings rejects bad elements", test_copy_strings_rejects_bad_elements},
    {"copy base url trims and accepts an empty optional url",
     test_copy_base_url_trims_and_accepts_an_empty_optional_url},
    {"copy base url rejects an unusable url", test_copy_base_url_rejects_an_unusable_url},
    {"load config reads and parses the file", test_load_config_reads_and_parses_the_file},
    {"copy size accepts bounds and keeps absent", test_copy_size_accepts_bounds_and_keeps_absent},
    {"copy size rejects out of bounds", test_copy_size_rejects_out_of_bounds},
    {"print string quotes and escapes delimiters", test_print_string_quotes_and_escapes_delimiters},
    {"print string escapes control bytes", test_print_string_escapes_control_bytes},
    {"print string round trips every control byte",
     test_print_string_round_trips_every_control_byte},
    {"print key lines", test_print_key_lines},
    {NULL, NULL},
};
