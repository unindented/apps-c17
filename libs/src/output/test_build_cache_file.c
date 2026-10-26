#include <acutest.h>
#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/str.h"
#include "os/os.h"
#include "os/os_memory.h"
#include "output/build_cache_file.h"
#include "testing/test_support.h"

/** Rows a test parse collected, in order. */
struct Rows {
  /** Row count `BuildCacheFileBeginFn` received, or `-1` before it is called. */
  ptrdiff_t count;

  /** The rows, a copy of each line in the parse's arena. */
  struct Str* lines;

  /** Number of rows parsed so far. */
  ptrdiff_t parsed_count;
};

/**
 * @brief Sizes the rows, as `BuildCacheFileBeginFn`.
 *
 * @param row_count Number of rows.
 * @param perm      Arena that owns the rows. Must not be `NULL`.
 * @param userdata  The `struct Rows`.
 */
static void rows_begin(ptrdiff_t row_count, struct Arena* perm, void* userdata) {
  struct Rows* rows = userdata;
  rows->count = row_count;
  rows->lines = arena_new(perm, row_count, struct Str);
}

/**
 * @brief Keeps a row, as `BuildCacheFileRowFn`, and rejects a row that is `bad` or does not sort
 *        after the one before it.
 *
 * @param line     The row.
 * @param index    Its index.
 * @param perm     Arena that owns the copy. Must not be `NULL`.
 * @param userdata The `struct Rows`.
 * @return `0`, or `-1` for a rejected row.
 */
static int rows_parse(struct Str line, ptrdiff_t index, struct Arena* perm, void* userdata) {
  struct Rows* rows = userdata;
  if (str_eq(line, S("bad")) || (index > 0 && str_compare(rows->lines[index - 1], line) >= 0)) {
    return -1;
  }
  rows->lines[index] = str_clone(line, perm);
  rows->parsed_count = index + 1;
  return 0;
}

/**
 * @brief Parses `text` under the header `v1\n` into `rows`.
 *
 * @param text  Text of the build cache.
 * @param rows  Receives the rows. Must not be `NULL`.
 * @param arena Arena that owns the rows. Must not be `NULL`.
 * @param log   Log that receives the reason. Must not be `NULL`.
 * @return What `build_cache_file_parse` returns.
 */
static int parse_text(const char* text, struct Rows* rows, struct Arena* arena, struct Log* log) {
  *rows = (struct Rows){.count = -1};
  return build_cache_file_parse(str_from_cstr(text), S("v1\n"), S("written by another build"),
                                rows_begin, rows_parse, rows, arena, log);
}

/**
 * @brief Parses the text of a cache with `parse_text`, as `BuildCacheFileParseFn`.
 *
 * @param text     Text of the build cache.
 * @param perm     Arena that owns the rows. Must not be `NULL`.
 * @param log      Log that receives the reason. Must not be `NULL`.
 * @param userdata The `struct Rows`.
 * @return What `build_cache_file_parse` returns.
 */
static int parse_cache(struct Str text, struct Arena* perm, struct Log* log, void* userdata) {
  struct Rows* rows = userdata;
  *rows = (struct Rows){.count = -1};
  return build_cache_file_parse(text, S("v1\n"), S("written by another build"), rows_begin,
                                rows_parse, rows, perm, log);
}

// A cache hands every row after its header to the app in order, after the row count, and a cache of
// its header alone has no rows.
static void test_parse_hands_each_row_in_order(void) {
  struct Arena arena = test_arena();
  struct Rows rows;
  struct Log log = test_log();
  TEST_CHECK(parse_text("v1\na\nb\n", &rows, &arena, &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
  TEST_CHECK(rows.count == 2 && rows.parsed_count == 2);
  TEST_CHECK(str_eq(rows.lines[0], S("a")) && str_eq(rows.lines[1], S("b")));

  TEST_CHECK(parse_text("v1\n", &rows, &arena, &log) == 0);
  TEST_CHECK(rows.count == 0 && rows.parsed_count == 0);
}

// A parse that succeeds hands back the arena past its rows, so what is allocated next does not
// overwrite them.
static void test_parse_keeps_its_rows_in_perm(void) {
  struct Arena arena = test_arena();
  const struct Arena before = arena;
  struct Rows rows;
  struct Log log = test_log();
  TEST_CHECK(parse_text("v1\na\nb\n", &rows, &arena, &log) == 0);
  TEST_CHECK(arena.beg > before.beg);
  char* after = arena_new(&arena, 16, char);
  memset(after, 'x', 16);
  TEST_CHECK(str_eq(rows.lines[0], S("a")) && str_eq(rows.lines[1], S("b")));
}

// A header of more than one line counts each of its lines, so a bad row is reported at its line in
// the file.
static void test_parse_counts_every_line_of_the_header(void) {
  struct Arena arena = test_arena();
  struct Rows rows = {.count = -1};
  struct Log log = test_log();
  TEST_CHECK(build_cache_file_parse(S("v1\nx\na\nbad\n"), S("v1\nx\n"),
                                    S("written by another build"), rows_begin, rows_parse, &rows,
                                    &arena, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache does not parse at line 4\n"));
}

// An empty file, which an interrupted write leaves, and a file of another header are each reported,
// and no row is handed on.
static void test_parse_reports_an_empty_file_and_another_header(void) {
  struct Arena arena = test_arena();
  struct Rows rows;
  struct Log log = test_log();
  TEST_CHECK(parse_text("", &rows, &arena, &log) == -1);
  TEST_CHECK(test_log_is(&log, "build cache is empty, as a build that did not finish leaves it\n"));
  TEST_CHECK(rows.count == -1);

  log = test_log();
  TEST_CHECK(parse_text("v2\na\n", &rows, &arena, &log) == -1);
  TEST_CHECK(test_log_is(&log, "written by another build\n"));
  TEST_CHECK(rows.count == -1);
}

// A row the app rejects, one out of order, and a last row cut short of its newline are each
// reported by their line in the file, counting the header, and a failed parse leaves the arena as
// it was.
static void test_parse_reports_the_line_of_a_bad_row(void) {
  static const char* const cases[][2] = {
      {"v1\na\nbad\n", "build cache does not parse at line 3\n"},
      {"v1\nb\na\n", "build cache does not parse at line 3\n"},
      {"v1\na\nb", "build cache does not parse at line 3\n"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    TEST_CASE(cases[i][0]);
    struct Arena arena = test_arena();
    const struct Arena before = arena;
    struct Rows rows;
    struct Log log = test_log();
    TEST_CHECK(parse_text(cases[i][0], &rows, &arena, &log) == -1);
    TEST_CHECK(test_log_is(&log, "%s", cases[i][1]));
    TEST_CHECK(arena.beg == before.beg);
  }
}

// A missing file is the usual first build, one that parses is loaded, and one that cannot be read
// or does not parse is unusable, with its reason in `perm`, which outlives the scratch it was
// written in.
static void test_load_tells_missing_loaded_and_unusable_apart(void) {
  struct Os* os = os_memory();
  struct Arena perm = test_arena();
  struct Rows rows;
  struct Str reason = {0};
  TEST_CHECK(build_cache_file_load(S("cache.tsv"), 1024, parse_cache, &rows, os, &perm,
                                   test_arena(), &reason) == BUILD_CACHE_FILE_MISSING);

  os_memory_write(os, "cache.tsv", "v1\na\n");
  TEST_CHECK(build_cache_file_load(S("cache.tsv"), 1024, parse_cache, &rows, os, &perm,
                                   test_arena(), &reason) == BUILD_CACHE_FILE_LOADED);
  TEST_CHECK(rows.parsed_count == 1);

  os_memory_write(os, "cache.tsv", "v1\nbad\n");
  struct Arena scratch = test_arena();
  TEST_CHECK(build_cache_file_load(S("cache.tsv"), 1024, parse_cache, &rows, os, &perm, scratch,
                                   &reason) == BUILD_CACHE_FILE_UNUSABLE);
  // What the load wrote in the scratch is free again, so the next allocation there writes over it.
  memset(arena_new(&scratch, 256, char), 'x', 256);
  TEST_CHECK(str_eq(reason, S("build cache does not parse at line 2")));

  os_memory_fail(os, OS_MEMORY_OP_READ, "cache.tsv", EACCES);
  TEST_CHECK(build_cache_file_load(S("cache.tsv"), 1024, parse_cache, &rows, os, &perm,
                                   test_arena(), &reason) == BUILD_CACHE_FILE_UNUSABLE);
  struct Arena arena = test_arena();
  struct Buf expected = {.arena = &arena};
  buf_printf(&expected, "failed to read build cache: %s ('cache.tsv')", strerror(EACCES));
  TEST_CHECK(str_eq(reason, buf_str(&expected)));
  TEST_MSG("reason: '%.*s'", STR_FMT(reason));
}

TEST_LIST = {
    {"parse hands each row in order", test_parse_hands_each_row_in_order},
    {"parse reports an empty file and another header",
     test_parse_reports_an_empty_file_and_another_header},
    {"parse keeps its rows in perm", test_parse_keeps_its_rows_in_perm},
    {"parse counts every line of the header", test_parse_counts_every_line_of_the_header},
    {"parse reports the line of a bad row", test_parse_reports_the_line_of_a_bad_row},
    {"load tells missing, loaded and unusable apart",
     test_load_tells_missing_loaded_and_unusable_apart},
    {NULL, NULL},
};
