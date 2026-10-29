#include <acutest.h>

#include "core/str.h"
#include "domain/grammar_path.h"
#include "testing/test_support.h"

// An extension names its language in any case, wherever the file is.
static void test_language_reads_the_extension_in_any_case(void) {
  TEST_CHECK(test_text_is(grammar_path_language(S("lib/main.c")), "c"));
  TEST_CHECK(test_text_is(grammar_path_language(S("src/App.TSX")), "tsx"));
  TEST_CHECK(test_text_is(grammar_path_language(S("run.sh")), "bash"));
  TEST_CHECK(test_text_is(grammar_path_language(S("a/b.d/index.html")), "html"));
}

// A known base name names its language wherever the file is, and is compared exactly.
static void test_language_reads_known_names(void) {
  TEST_CHECK(test_text_is(grammar_path_language(S("CMakeLists.txt")), "cmake"));
  TEST_CHECK(test_text_is(grammar_path_language(S("tools/Makefile")), "make"));
  TEST_CHECK(grammar_path_language(S("MAKEFILE")).len == 0);
}

// A name with no known extension, or that is only an extension, names no language.
static void test_language_is_empty_for_other_names(void) {
  TEST_CHECK(grammar_path_language(S("README")).len == 0);
  TEST_CHECK(grammar_path_language(S("notes.txt")).len == 0);
  TEST_CHECK(grammar_path_language(S("dir/.c")).len == 0);
  TEST_CHECK(grammar_path_language((struct Str){0}).len == 0);
}

TEST_LIST = {
    {"language reads the extension in any case", test_language_reads_the_extension_in_any_case},
    {"language reads known names", test_language_reads_known_names},
    {"language is empty for other names", test_language_is_empty_for_other_names},
    {NULL, NULL},
};
