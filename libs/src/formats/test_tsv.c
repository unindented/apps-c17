#include <acutest.h>
#include <stddef.h>

#include "core/arena.h"
#include "core/buf.h"
#include "core/str.h"
#include "formats/tsv.h"
#include "testing/test_support.h"

/**
 * @brief Returns `text` escaped as a field, in a buffer of its own.
 *
 * @param text  Text to escape.
 * @param arena Arena that owns the result. Must not be `NULL`.
 * @return The escaped text, owned by `arena`.
 */
static struct Str escaped(struct Str text, struct Arena* arena) {
  struct Buf buf = {.arena = arena};
  tsv_escape_append(&buf, text);
  return buf_str(&buf);
}

// A `\`, a tab and a newline each become two bytes, wherever they are in the text, so the field
// holds no separator.
static void test_tsv_escape_replaces_separators(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(escaped(S("a\\b\tc\nd"), &arena), S("a\\\\b\\tc\\nd")));
  TEST_CHECK(str_eq(escaped(S("\t\n\\"), &arena), S("\\t\\n\\\\")));
}

// Every other byte is copied verbatim, including a carriage return, a `NUL`, a byte above 0x7F and
// the `=` and `,` that a field's own syntax may use.
static void test_tsv_escape_copies_other_bytes_verbatim(void) {
  struct Arena arena = test_arena();
  const struct Str text = S("k=v,w\r\0caf\xC3\xA9");
  TEST_CHECK(str_eq(escaped(text, &arena), text));
}

// Texts that would read the same with their separators raw stay apart: a tab, the two bytes `\t`,
// and a `\` before a tab each escape differently.
static void test_tsv_escape_tells_texts_apart(void) {
  struct Arena arena = test_arena();
  const struct Str tab = escaped(S("a\tb"), &arena);
  const struct Str backslash_t = escaped(S("a\\tb"), &arena);
  const struct Str backslash_tab = escaped(S("a\\\tb"), &arena);
  TEST_CHECK(str_eq(tab, S("a\\tb")));
  TEST_CHECK(str_eq(backslash_t, S("a\\\\tb")));
  TEST_CHECK(str_eq(backslash_tab, S("a\\\\\\tb")));
}

// Escaping appends to whatever the buffer already holds, and only the string's own bytes are read.
static void test_tsv_escape_appends_within_length(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  buf_append(&buf, S("title="));
  tsv_escape_append(&buf, str_head(S("a\tb\nc"), 3));
  TEST_CHECK(str_eq(buf_str(&buf), S("title=a\\tb")));
}

// Escaping an empty text, with or without a pointer, leaves the buffer untouched.
static void test_tsv_escape_empty_input(void) {
  struct Arena arena = test_arena();
  struct Buf buf = {.arena = &arena};
  tsv_escape_append(&buf, S(""));
  tsv_escape_append(&buf, (struct Str){NULL, 0});
  TEST_CHECK(buf.len == 0);
  TEST_CHECK(buf.data == NULL);
}

TEST_LIST = {
    {"tsv escape replaces separators", test_tsv_escape_replaces_separators},
    {"tsv escape copies other bytes verbatim", test_tsv_escape_copies_other_bytes_verbatim},
    {"tsv escape tells texts apart", test_tsv_escape_tells_texts_apart},
    {"tsv escape appends within length", test_tsv_escape_appends_within_length},
    {"tsv escape empty input", test_tsv_escape_empty_input},
    {NULL, NULL},
};
