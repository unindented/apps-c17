#include <acutest.h>
#include <stddef.h>
#include <string.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/log.h"
#include "core/path.h"
#include "core/str.h"
#include "output/output_path.h"
#include "testing/test_support.h"

// A path within both limits is accepted and records nothing.
static void test_check_limits_accepts_bounded_path(void) {
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(S("album/index.html"), S("album"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// Each `/` in an output file path adds one `../`, so a top-level file gets an empty prefix.
static void test_to_root_climbs_one_level_per_separator(void) {
  struct Arena arena = test_arena();
  struct Str top = output_path_to_root(S("index.html"), &arena);
  struct Str nested = output_path_to_root(S("a/b/index.html"), &arena);
  TEST_CHECK(top.len == 0);
  TEST_CHECK(str_eq(nested, S("../../")));
}

// A not-found path at the top or in a directory, as bunny.net wants it, is accepted.
static void test_check_not_found_accepts_html_paths(void) {
  struct Log log = test_log();
  TEST_CHECK(output_path_check_not_found(S("404.html"), &log) == 0);
  TEST_CHECK(output_path_check_not_found(S("bunnycdn_errors/404.html"), &log) == 0);
  TEST_CHECK(test_log_is_empty(&log));
}

// The prefix is the path of `base_url` with one trailing `/`, and `/` for a URL without a path or
// for no URL at all.
static void test_not_found_root_is_the_base_url_path(void) {
  static const char* const cases[][2] = {
      {"https://example.com", "/"},           {"https://example.com:8080", "/"},
      {"https://example.com/blog", "/blog/"}, {"http://example.com/a/b", "/a/b/"},
      {"https://example.com/a/b/", "/a/b/"},  {"", "/"},
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Arena arena = test_arena();
    const struct Str root = output_path_not_found_root(str_from_cstr(cases[i][0]), &arena);
    TEST_CHECK(test_text_is(root, "%s", cases[i][1]));
    TEST_MSG("case %td", i);
  }
}

// A path over the whole-path limit names the limit, the measured length, the producer, and the
// whole path.
static void test_check_limits_rejects_overlong_path(void) {
  enum { SEGMENT_LEN = 200, SEGMENT_COUNT = 6 };
  char path[SEGMENT_COUNT * (SEGMENT_LEN + 1)];
  ptrdiff_t path_len = 0;
  for (ptrdiff_t i = 0; i < SEGMENT_COUNT; i++) {
    if (i > 0) {
      path[path_len++] = '/';
    }
    memset(path + path_len, 'a', SEGMENT_LEN);
    path_len += SEGMENT_LEN;
  }
  const struct Str overlong = {path, path_len};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(overlong, S("src.jpg"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path for 'src.jpg' exceeds max output path length (%d bytes) at "
                         "%td bytes: '%.*s'\n",
                         OUTPUT_PATH_RELATIVE_LEN_MAX, path_len, STR_FMT(overlong)));
  TEST_CHECK(log.errors == 1);
}

// A path within the whole-path limit but with one overlong segment names only that segment as the
// offending value.
static void test_check_limits_rejects_overlong_segment(void) {
  enum { SEGMENT_LEN = FILENAME_LEN_MAX + 1 };
  char path[sizeof("album/") - 1 + SEGMENT_LEN];
  memcpy(path, "album/", sizeof("album/") - 1);
  memset(path + sizeof("album/") - 1, 'b', SEGMENT_LEN);
  const struct Str segment = {path + sizeof("album/") - 1, SEGMENT_LEN};
  const struct Str whole = {path, (ptrdiff_t)sizeof(path)};
  struct Log log = test_log();
  TEST_CHECK(output_path_check_limits(whole, S("src.jpg"), &log) == -1);
  TEST_CHECK(test_log_is(&log,
                         "output path segment for 'src.jpg' exceeds max filename length (%d "
                         "bytes) at %d bytes: '%.*s'\n",
                         FILENAME_LEN_MAX, SEGMENT_LEN, STR_FMT(segment)));
}

// A not-found path that is not a safe relative path ending in `.html` names the key and the value.
static void test_check_not_found_rejects_unsafe_and_non_html(void) {
  static const char* const cases[] = {
      "404.htm",   "404",     "",         "errors//404.html", "../404.html", "errors/../404.html",
      "/404.html", "errors/", "a b.html",
  };
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(cases) / sizeof(cases[0])); i++) {
    struct Log log = test_log();
    TEST_CHECK(output_path_check_not_found(str_from_cstr(cases[i]), &log) == -1);
    TEST_CHECK(test_log_is(
        &log, "config key 'not_found_path' must be a safe relative path ending in '.html': '%s'\n",
        cases[i]));
    TEST_MSG("case %td", i);
  }
}

TEST_LIST = {
    {"check limits accepts bounded path", test_check_limits_accepts_bounded_path},
    {"to root climbs one level per separator", test_to_root_climbs_one_level_per_separator},
    {"check not found accepts html paths", test_check_not_found_accepts_html_paths},
    {"not found root is the base url path", test_not_found_root_is_the_base_url_path},
    {"check limits rejects overlong path", test_check_limits_rejects_overlong_path},
    {"check limits rejects overlong segment", test_check_limits_rejects_overlong_segment},
    {"check not found rejects unsafe and non html",
     test_check_not_found_rejects_unsafe_and_non_html},
    {NULL, NULL},
};
