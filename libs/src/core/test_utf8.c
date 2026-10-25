#include <acutest.h>
#include <stdbool.h>

#include "core/arena.h"
#include "core/str.h"
#include "core/utf8.h"
#include "testing/test_support.h"

/** U+FFFD REPLACEMENT CHARACTER in UTF-8. */
#define FFFD "\xEF\xBF\xBD"

// Valid text, multi-byte sequences included, comes back as the same bytes, without a copy.
static void test_repair_keeps_valid_text(void) {
  struct Arena arena = test_arena();
  const struct Str text = S("caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80");
  const struct Str repaired = utf8_repair_for_xml(text, &arena);
  TEST_CHECK(repaired.data == text.data && repaired.len == text.len);
}

// A lone continuation byte, an invalid lead byte and a byte past U+10FFFF each become one U+FFFD.
static void test_repair_replaces_invalid_bytes(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("a\x80"
                                          "b\xff"
                                          "c\xf5\x80"),
                                        &arena),
                    S("a" FFFD "b" FFFD "c" FFFD FFFD)));
}

// A truncated sequence is one U+FFFD for its maximal valid prefix, and the byte after it stays.
static void test_repair_replaces_truncated_sequences(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xe2\x82"
                                          "x\xf0\x9f\x98"),
                                        &arena),
                    S(FFFD "x" FFFD)));
}

// Overlong forms and surrogates are invalid, one U+FFFD per byte that cannot start a sequence.
static void test_repair_rejects_overlong_forms_and_surrogates(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xc0\xaf"), &arena), S(FFFD FFFD)));
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xed\xa0\x80"), &arena), S(FFFD FFFD FFFD)));
}

// After `E0`, `F0` and `F4` the second byte has a narrower range, so the overlong three- and
// four-byte forms and a code point past U+10FFFF are one U+FFFD per byte, and the first and last
// code points of each range stay.
static void test_repair_checks_the_second_byte_of_e0_f0_and_f4(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xe0\x80\x80"), &arena), S(FFFD FFFD FFFD)));
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xf0\x80\x80\x80"), &arena), S(FFFD FFFD FFFD FFFD)));
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("\xf4\x90\x80\x80"), &arena), S(FFFD FFFD FFFD FFFD)));
  TEST_CHECK(utf8_is_valid(S("\xe0\xa0\x80")));
  TEST_CHECK(utf8_is_valid(S("\xf0\x90\x80\x80")));
  TEST_CHECK(utf8_is_valid(S("\xf4\x8f\xbf\xbf")));
}

// Text without a control character is valid exactly when the repair hands it back as it is: a
// truncated sequence and a surrogate are not.
static void test_is_valid_agrees_with_repair(void) {
  struct Arena arena = test_arena();
  const struct Str texts[] = {S("caf\xc3\xa9"), (struct Str){0}, S("caf\xc3"), S("\xed\xa0\x80")};
  const bool expected[] = {true, true, false, false};
  for (ptrdiff_t i = 0; i < (ptrdiff_t)(sizeof(texts) / sizeof(texts[0])); i++) {
    TEST_CHECK(utf8_is_valid(texts[i]) == expected[i]);
    TEST_CHECK((utf8_repair_for_xml(texts[i], &arena).data == texts[i].data) == expected[i]);
  }
}

// The repair also drops the C0 controls and the noncharacters U+FFFE and U+FFFF that XML forbids,
// and keeps tab, line feed, return and every other character.
static void test_repair_drops_what_xml_forbids(void) {
  struct Arena arena = test_arena();
  TEST_CHECK(
      str_eq(utf8_repair_for_xml(S("a\x01\tb\x1b[31m\n\r\xff"), &arena), S("a\tb[31m\n\r" FFFD)));
  TEST_CHECK(str_eq(utf8_repair_for_xml(S("x\xef\xbf\xbey\xef\xbf\xbfz\xef\xbf\xbd"), &arena),
                    S("xyz\xef\xbf\xbd")));
}

TEST_LIST = {
    {"repair keeps valid text", test_repair_keeps_valid_text},
    {"repair replaces invalid bytes", test_repair_replaces_invalid_bytes},
    {"repair replaces truncated sequences", test_repair_replaces_truncated_sequences},
    {"repair rejects overlong forms and surrogates",
     test_repair_rejects_overlong_forms_and_surrogates},
    {"repair checks the second byte of e0 f0 and f4",
     test_repair_checks_the_second_byte_of_e0_f0_and_f4},
    {"is valid agrees with repair", test_is_valid_agrees_with_repair},
    {"repair drops what xml forbids", test_repair_drops_what_xml_forbids},
    {NULL, NULL},
};
