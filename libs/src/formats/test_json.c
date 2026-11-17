#include <acutest.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "formats/json.h"
#include "testing/test_support.h"

/**
 * @brief Returns `text` as a JSON string, in a buffer of its own.
 *
 * @param text Text to append.
 * @return The JSON string.
 */
static struct Str quoted(struct Str text) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  json_string_append(&buf, text, test_arena());
  return buf_str(&buf);
}

// Text that needs no escape is quoted as it is, UTF-8 included.
static void test_string_append_quotes_text(void) {
  TEST_CHECK(test_text_is(quoted(S("feature/x caf\xC3\xA9")), "\"feature/x caf\xC3\xA9\""));
  TEST_CHECK(test_text_is(quoted(S("")), "\"\""));
}

// A `"` and a `\` get a `\` before them, and a tab, a line feed and a carriage return their short
// escapes, so no byte of the text ends the string.
static void test_string_append_escapes_quotes_and_white_space(void) {
  TEST_CHECK(test_text_is(quoted(S("a\"b\\c\td\ne\rf")), "\"a\\\"b\\\\c\\td\\ne\\rf\""));
}

// Bytes that are not UTF-8 become U+FFFD, and the other control characters are dropped, so the
// string is valid JSON in any parser.
static void test_string_append_repairs_invalid_text(void) {
  TEST_CHECK(test_text_is(quoted(S("caf\xE9!")), "\"caf\xEF\xBF\xBD!\""));
  TEST_CHECK(test_text_is(quoted(S("a\001b\037c")), "\"abc\""));
}

TEST_LIST = {
    {"string append quotes text", test_string_append_quotes_text},
    {"string append escapes quotes and white space",
     test_string_append_escapes_quotes_and_white_space},
    {"string append repairs invalid text", test_string_append_repairs_invalid_text},
    {NULL, NULL},
};
