#include <acutest.h>
#include <stddef.h>

#include "build/highlight_diff.h"
#include "core/arena.h"
#include "core/str.h"
#include "domain/repo_model.h"
#include "markup/highlight.h"
#include "testing/test_support.h"

// Each line gets its own fragment, escaped, and a line inside a multi-line comment keeps the
// comment's span though the comment starts on an earlier line.
static void test_lines_keep_a_comment_across_lines(void) {
  struct Arena arena = test_arena();
  struct Str* fragments = NULL;
  ptrdiff_t count = 0;
  highlight_diff_lines(S("/* a\n<b> */\nint x;\n"), highlight_grammar_find(S("c")), &arena,
                       test_arena(), &fragments, &count);
  TEST_ASSERT(count == 3);
  TEST_CHECK(test_text_is(fragments[0], "<span class=\"tok-comment\">/* a</span>"));
  TEST_CHECK(test_text_is(fragments[1], "<span class=\"tok-comment\">&lt;b&gt; */</span>"));
  TEST_CHECK(test_text_is(fragments[2],
                          "<span class=\"tok-type-builtin\">int</span> <span "
                          "class=\"tok-variable\">x</span><span "
                          "class=\"tok-delimiter\">;</span>"));
}

// Without a grammar, each line is only escaped.
static void test_lines_escape_without_a_grammar(void) {
  struct Arena arena = test_arena();
  struct Str* fragments = NULL;
  ptrdiff_t count = 0;
  highlight_diff_lines(S("a&b\nc"), NULL, &arena, test_arena(), &fragments, &count);
  TEST_ASSERT(count == 2);
  TEST_CHECK(test_text_is(fragments[0], "a&amp;b"));
  TEST_CHECK(test_text_is(fragments[1], "c"));
}

// A deletion takes the old side's fragment of its line, and an addition or a context line the new
// side's, each by its own side's number.
static void test_file_takes_each_line_from_its_side(void) {
  struct DiffLine lines[] = {
      {.origin = ' ', .old_line = 1, .new_line = 2, .text = S("c")},
      {.origin = '-', .old_line = 2, .text = S("o")},
      {.origin = '+', .new_line = 3, .text = S("n")},
  };
  const struct ChangedFile file = {.lines = lines, .line_count = 3};
  const struct Str old_fragments[] = {S("<i>o1</i>"), S("<i>o2</i>")};
  const struct Str new_fragments[] = {S("<b>n1</b>"), S("<b>n2</b>"), S("<b>n3</b>")};
  const struct HighlightDiffSides sides = {
      .old_fragments = old_fragments,
      .old_fragment_count = 2,
      .new_fragments = new_fragments,
      .new_fragment_count = 3,
  };
  struct Arena arena = test_arena();
  const struct Str* fragments = highlight_diff_file(&file, &sides, &arena);
  TEST_CHECK(test_text_is(fragments[0], "<b>n2</b>"));
  TEST_CHECK(test_text_is(fragments[1], "<i>o2</i>"));
  TEST_CHECK(test_text_is(fragments[2], "<b>n3</b>"));
}

// A line of a side that is not highlighted, a line past its side's fragments and the "no newline at
// end" line are escaped alone.
static void test_file_escapes_lines_without_a_fragment(void) {
  struct DiffLine lines[] = {
      {.origin = '-', .old_line = 1, .text = S("<o>")},
      {.origin = '+', .new_line = 2, .text = S("<n>")},
      {.origin = '\\', .text = S("No newline at end of file")},
  };
  const struct ChangedFile file = {.lines = lines, .line_count = 3};
  const struct Str new_fragments[] = {S("<b>n1</b>")};
  const struct HighlightDiffSides sides = {.new_fragments = new_fragments, .new_fragment_count = 1};
  struct Arena arena = test_arena();
  const struct Str* fragments = highlight_diff_file(&file, &sides, &arena);
  TEST_CHECK(test_text_is(fragments[0], "&lt;o&gt;"));
  TEST_CHECK(test_text_is(fragments[1], "&lt;n&gt;"));
  TEST_CHECK(test_text_is(fragments[2], "No newline at end of file"));
}

TEST_LIST = {
    {"lines keep a comment across lines", test_lines_keep_a_comment_across_lines},
    {"lines escape without a grammar", test_lines_escape_without_a_grammar},
    {"file takes each line from its side", test_file_takes_each_line_from_its_side},
    {"file escapes lines without a fragment", test_file_escapes_lines_without_a_fragment},
    {NULL, NULL},
};
