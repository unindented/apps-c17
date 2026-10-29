#include <acutest.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "domain/url_segment.h"
#include "testing/test_support.h"

/**
 * @brief Returns `segment` encoded by `url_segment_append`.
 *
 * @param segment Bytes to encode.
 * @return The encoded text, in a reservation of its own.
 */
static struct Str encode(struct Str segment) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  url_segment_append(&buffer, segment);
  return buf_str(&buffer);
}

// Unreserved bytes stay as they are.
static void test_append_keeps_unreserved_bytes(void) {
  TEST_CHECK(str_eq(encode(S("Az09-._~")), S("Az09-._~")));
}

// Every other byte is `%XX` in uppercase, `+`, `/` and non-ASCII bytes included.
static void test_append_encodes_other_bytes(void) {
  TEST_CHECK(str_eq(encode(S("a b&c%d#e+f/\xc3\xb1")), S("a%20b%26c%25d%23e%2Bf%2F%C3%B1")));
}

// A path keeps its `/` and encodes each segment, an empty one included.
static void test_append_path_keeps_separators(void) {
  struct Arena arena = test_arena();
  struct Buf buffer = {.arena = &arena};
  url_segment_append_path(&buffer, S("feature/x y//z"));
  TEST_CHECK(str_eq(buf_str(&buffer), S("feature/x%20y//z")));
}

TEST_LIST = {
    {"append keeps unreserved bytes", test_append_keeps_unreserved_bytes},
    {"append encodes other bytes", test_append_encodes_other_bytes},
    {"append path keeps separators", test_append_path_keeps_separators},
    {NULL, NULL},
};
